//=====================================================================
//
// table_builder.h -
//
// Created by liubang on 2023/05/26 16:34
// Last Modified: 2023/05/26 16:34
//
//=====================================================================

#pragma once

#include "cpp/misc/fs/fs_writer.h"
#include "cpp/tools/binary.h"

namespace playground::cpp::misc::sst {

class TableBuilder {
public:
  void Add(const tools::Binary& key, const tools::Binary& value);

  void flush();

private:
private:
  fs::FsWriter* writer_;
};

}  // namespace playground::cpp::misc::sst
