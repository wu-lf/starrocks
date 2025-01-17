// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/exec/except_node.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/except_node.h"

#include "exec/hash_table.hpp"
#include "exprs/expr.h"
#include "runtime/row_batch.h"
#include "runtime/runtime_state.h"

namespace starrocks {
ExceptNode::ExceptNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
        : SetOperationNode(pool, tnode, descs, tnode.except_node.tuple_id) {}

Status ExceptNode::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(SetOperationNode::init(tnode, state));
    // Create result_expr_ctx_lists_ from thrift exprs.
    auto& result_texpr_lists = tnode.except_node.result_expr_lists;
    for (auto& texprs : result_texpr_lists) {
        std::vector<ExprContext*> ctxs;
        RETURN_IF_ERROR(Expr::create_expr_trees(_pool, texprs, &ctxs));
        _child_expr_lists.push_back(ctxs);
    }
    return Status::OK();
}

Status ExceptNode::open(RuntimeState* state) {
    RETURN_IF_ERROR(SetOperationNode::open(state));
    // if a table is empty, the result must be empty

    if (_hash_tbl->size() == 0) {
        _hash_tbl_iterator = _hash_tbl->begin();
        return Status::OK();
    }
    bool eos = false;

    for (int i = 1; i < _children.size(); ++i) {
        // If user send TableA except TableB except TableC.
        // We should rebuild the hashtable after TableA except TableB.
        // Of course, initialized hashtable in building TableA
        //    have remove the duplicated value.
        if (i > 1) {
            SCOPED_TIMER(_build_timer);
            std::unique_ptr<HashTable> temp_tbl(new HashTable(_child_expr_lists[0], _child_expr_lists[i],
                                                              _build_tuple_size, true, _find_nulls, id(), mem_tracker(),
                                                              1024));
            _hash_tbl_iterator = _hash_tbl->begin();
            while (_hash_tbl_iterator.has_next()) {
                // one bucket will have many nodes.
                // Some of this nodes may already be excepted, but another is not.
                // So we should traverse all the nodes to rebuild the hashtable.
                if (!_hash_tbl_iterator.matched()) {
                    VLOG_ROW << "rebuild row: "
                             << get_row_output_string(_hash_tbl_iterator.get_row(), child(0)->row_desc());
                    temp_tbl->insert(_hash_tbl_iterator.get_row());
                }
                _hash_tbl_iterator.next<false>();
            }
            _hash_tbl.swap(temp_tbl);
            temp_tbl->close();
        }
        // probe
        _probe_batch.reset(new RowBatch(child(i)->row_desc(), state->batch_size(), mem_tracker()));
        ScopedTimer<MonotonicStopWatch> probe_timer(_probe_timer);
        RETURN_IF_ERROR(child(i)->open(state));
        eos = false;
        while (!eos) {
            RETURN_IF_CANCELLED(state);
            RETURN_IF_ERROR(child(i)->get_next(state, _probe_batch.get(), &eos));
            RETURN_IF_LIMIT_EXCEEDED(state, " Except , while probing the hash table.");
            for (int j = 0; j < _probe_batch->num_rows(); ++j) {
                VLOG_ROW << "probe row: " << get_row_output_string(_probe_batch->get_row(j), child(i)->row_desc());
                _hash_tbl_iterator = _hash_tbl->find(_probe_batch->get_row(j));
                if (_hash_tbl_iterator != _hash_tbl->end()) {
                    _hash_tbl_iterator.set_matched();
                    VLOG_ROW << "probe matched: "
                             << get_row_output_string(_hash_tbl_iterator.get_row(), child(0)->row_desc());
                }
            }
            _probe_batch->reset();
        }
        // if a table is empty, the result must be empty
        if (_hash_tbl->size() == 0) {
            break;
        }
    }
    _hash_tbl_iterator = _hash_tbl->begin();
    return Status::OK();
}

Status ExceptNode::get_next(RuntimeState* state, RowBatch* out_batch, bool* eos) {
    RETURN_IF_ERROR(exec_debug_action(TExecNodePhase::GETNEXT));
    RETURN_IF_CANCELLED(state);
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    *eos = true;
    if (reached_limit()) {
        return Status::OK();
    }
    int64_t tuple_buf_size;
    uint8_t* tuple_buf;
    RETURN_IF_ERROR(out_batch->resize_and_allocate_tuple_buffer(state, &tuple_buf_size, &tuple_buf));
    memset(tuple_buf, 0, tuple_buf_size);
    while (_hash_tbl_iterator.has_next()) {
        VLOG_ROW << "find row: " << get_row_output_string(_hash_tbl_iterator.get_row(), child(0)->row_desc())
                 << " matched: " << _hash_tbl_iterator.matched();
        if (!_hash_tbl_iterator.matched()) {
            create_output_row(_hash_tbl_iterator.get_row(), out_batch, tuple_buf);
            tuple_buf += _tuple_desc->byte_size();
            ++_num_rows_returned;
        }
        _hash_tbl_iterator.next<false>();
        *eos = !_hash_tbl_iterator.has_next() || reached_limit();
        if (out_batch->is_full() || out_batch->at_resource_limit() || *eos) {
            return Status::OK();
        }
    }
    return Status::OK();
}

} // namespace starrocks
