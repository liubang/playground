//=====================================================================
//
// fs_writer.h -
//
// Created by liubang on 2023/05/26 16:47
// Last Modified: 2023/05/26 16:47
//
//=====================================================================

#pragma once

#include "cpp/tools/binary.h"

namespace playground::cpp::misc::fs {

class FsWriter {
public:
  ~FsWriter() = default;
  FsWriter(const FsWriter&) = delete;
  FsWriter& operator=(const FsWriter&) = delete;
  virtual Status Append(const tools::Binary& data) = 0;
  virtual Status Close() = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
};

}  // namespace playground::cpp::misc::fs
