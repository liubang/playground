//=====================================================================
//
// sstable_builder.h -
//
// Created by liubang on 2023/05/31 19:39
// Last Modified: 2023/05/31 19:39
//
//=====================================================================
#pragma once

#include "cpp/misc/fs/fs.h"
#include "cpp/misc/sst/block_builder.h"
#include "cpp/misc/sst/filter_block_builder.h"
#include "cpp/misc/sst/options.h"
#include "cpp/misc/sst/sstable_format.h"
#include "cpp/tools/binary.h"
#include "cpp/tools/status.h"

namespace pl {

class SSTableBuilder {
 public:
  SSTableBuilder(const Options* options, FsWriter* writer);
  SSTableBuilder(const SSTableBuilder&) = delete;
  SSTableBuilder& operator=(const SSTableBuilder&) = delete;
  ~SSTableBuilder();

  void add(const Binary& key, const Binary& value);

  void flush();

  Status finish();

  uint64_t entriesCount();

  uint64_t fileSize();

  [[nodiscard]] Status status() const { return status_; }

  [[nodiscard]] bool ok() const { return status().isOk(); }

 private:
  void writeBlock(BlockBuilder* block, BlockHandle* handle);
  void writeBlockRaw(const Binary& content, CompressionType type,
                     BlockHandle* handle);

 private:
  const Options* options_;
  FsWriter* writer_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  BlockHandle pending_handler_;
  FilterBlockBuilder* filter_block_;
  std::string last_key_;
  int64_t num_entries_{0};
  uint64_t offset_{0};
  Status status_;
  bool pending_index_entry_{false};
  bool closed_{false};
};

}  // namespace pl
