//=====================================================================
//
// table_builder.h -
//
// Created by liubang on 2023/05/26 16:34
// Last Modified: 2023/05/26 16:34
//
//=====================================================================

#pragma once

#include "cpp/misc/fs/fs.h"
#include "cpp/misc/sst/block_builder.h"
#include "cpp/tools/binary.h"
#include "cpp/tools/status.h"

namespace playground::cpp::misc::sst {

class TableBuilder {
public:
  TableBuilder(fs::FsWriter* writer);
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;
  ~TableBuilder();

  void add(const tools::Binary& key, const tools::Binary& value);

  void flush();

  tools::Status finish();

  uint64_t entriesCount();

  uint64_t fileSize();

private:
private:
  fs::FsWriter* writer_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  int64_t num_entries_;
};

}  // namespace playground::cpp::misc::sst
