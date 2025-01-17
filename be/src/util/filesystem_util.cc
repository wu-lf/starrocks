// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/util/filesystem_util.cc

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

#include "util/filesystem_util.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <filesystem>
#include <system_error>

#include "util/error_util.h"

using std::exception;
using std::string;
using std::vector;

// std::filesystem functions must be given an errcode parameter to avoid the variants
// of those functions that throw exceptions.
namespace starrocks {

Status FileSystemUtil::create_directory(const string& directory) {
    std::error_code errcode;
    bool exists = std::filesystem::exists(directory, errcode);
    // Need to check for no_such_file_or_directory error case - std's exists() sometimes
    // returns an error when it should simply return false.
    if (errcode && errcode != std::errc::no_such_file_or_directory) {
        std::stringstream error_msg;
        error_msg << "Encountered error checking existence of directory: " << directory << ": " << errcode.message();
        return Status::InternalError(error_msg.str());
    }
    if (exists) {
        // Attempt to remove the directory and its contents so that we can create a fresh
        // empty directory that we will have permissions for.
        std::filesystem::remove_all(directory, errcode);
        if (errcode) {
            std::stringstream error_msg;
            error_msg << "Encountered error removing directory " << directory << errcode.message();
            return Status::InternalError(error_msg.str());
        }
    }
    std::filesystem::create_directories(directory, errcode);
    if (errcode) {
        std::stringstream error_msg;
        error_msg << "Encountered error creating directory " << directory << errcode.message();
        return Status::InternalError(error_msg.str());
    }
    return Status::OK();
}

Status FileSystemUtil::remove_paths(const vector<string>& directories) {
    for (int i = 0; i < directories.size(); ++i) {
        std::error_code errcode;
        std::filesystem::remove_all(directories[i], errcode);
        if (errcode) {
            std::stringstream error_msg;
            error_msg << "Encountered error removing directory " << directories[i] << ": " << errcode.message();
            return Status::InternalError(error_msg.str());
        }
    }

    return Status::OK();
}

Status FileSystemUtil::create_file(const string& file_path) {
    int fd = creat(file_path.c_str(), S_IRUSR | S_IWUSR);

    if (fd < 0) {
        std::stringstream error_msg;
        error_msg << "Create file " << file_path.c_str() << " failed with errno=" << errno
                  << "description=" << get_str_err_msg();
        return Status::InternalError(error_msg.str());
    }

    int success = close(fd);
    if (success < 0) {
        std::stringstream error_msg;
        error_msg << "Close file " << file_path.c_str() << " failed with errno=" << errno
                  << " description=" << get_str_err_msg();
        return Status::InternalError(error_msg.str());
    }

    return Status::OK();
}

Status FileSystemUtil::resize_file(const string& file_path, int64_t trunc_len) {
    int success = truncate(file_path.c_str(), trunc_len);
    if (success != 0) {
        std::stringstream error_msg;
        error_msg << "Truncate file " << file_path << " to length " << trunc_len << " failed with " << errno << " ("
                  << get_str_err_msg() << ")";
        return Status::InternalError(error_msg.str());
    }

    return Status::OK();
}

Status FileSystemUtil::verify_is_directory(const string& directory_path) {
    std::error_code errcode;
    bool exists = std::filesystem::exists(directory_path, errcode);
    if (errcode) {
        std::stringstream error_msg;
        error_msg << "Encountered exception while verifying existence of directory path " << directory_path << ": "
                  << errcode.message();
        return Status::InternalError(error_msg.str());
    }
    if (!exists) {
        std::stringstream error_msg;
        error_msg << "Directory path " << directory_path << " does not exist ";
        return Status::InternalError(error_msg.str());
    }
    bool is_dir = std::filesystem::is_directory(directory_path, errcode);
    if (errcode) {
        std::stringstream error_msg;
        error_msg << "Encountered exception while verifying existence of directory path " << directory_path << ": "
                  << errcode.message();
        return Status::InternalError(error_msg.str());
    }
    if (!is_dir) {
        std::stringstream error_msg;
        error_msg << "Path " << directory_path << " is not a directory";
        return Status::InternalError(error_msg.str());
    }
    return Status::OK();
}

Status FileSystemUtil::get_space_available(const string& directory_path, uint64_t* available_bytes) {
    std::error_code errcode;
    std::filesystem::space_info info = std::filesystem::space(directory_path, errcode);
    if (errcode) {
        std::stringstream error_msg;
        error_msg << "Encountered exception while checking available space for path " << directory_path << ": "
                  << errcode.message();
        return Status::InternalError(error_msg.str());
    }
    *available_bytes = info.available;
    return Status::OK();
}

uint64_t FileSystemUtil::max_num_file_handles() {
    struct rlimit data;
    if (getrlimit(RLIMIT_NOFILE, &data) == 0) {
        return static_cast<uint64_t>(data.rlim_cur);
    }
    return 0ul;
}

// NOTE: the parent_path and sub_path can either dir or file.
//   return true if parent_path == sub_path
bool FileSystemUtil::contain_path(const std::string& parent_path, const std::string& sub_path) {
    std::filesystem::path parent(parent_path);
    std::filesystem::path sub(sub_path);
    parent = parent.lexically_normal();
    sub = sub.lexically_normal();

    if (parent == sub) {
        return true;
    }

    if (parent.filename() == ".") {
        parent.remove_filename();
    }

    // We're also not interested in the file's name.
    if (sub.has_filename()) {
        sub.remove_filename();
    }
    // If dir has more components than file, then file can't possibly reside in dir.
    auto dir_len = std::distance(parent.begin(), parent.end());
    auto file_len = std::distance(sub.begin(), sub.end());
    if (dir_len > file_len) {
        return false;
    }

    return std::equal(parent.begin(), parent.end(), sub.begin(), [](auto&& __first1, auto&& __first2) {
        if (__first1.string().empty()) {
            return true;
        }
        return __first1 == __first2;
    });
}

} // end namespace starrocks
