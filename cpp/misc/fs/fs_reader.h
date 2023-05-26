//=====================================================================
//
// fs_reader.h -
//
// Created by liubang on 2023/05/26 16:50
// Last Modified: 2023/05/26 16:50
//
//=====================================================================

#pragma once

#include "cpp/tools/binary.h"

namespace playground::cpp::misc::fs {

class FsReader {
public:
  virtual Status Read(uint64_t offset, size_t n, tools::Binary* result, char* scratch) const = 0;
};

}  // namespace playground::cpp::misc::fs
