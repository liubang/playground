//=====================================================================
//
// table.h -
//
// Created by liubang on 2023/06/04 00:36
// Last Modified: 2023/06/04 00:36
//
//=====================================================================

#include "cpp/misc/fs/fs.h"
#include "cpp/misc/sst/block.h"
#include "cpp/misc/sst/filter_block_reader.h"
#include "cpp/misc/sst/options.h"
#include "cpp/misc/sst/sstable_format.h"
#include "cpp/tools/status.h"

namespace playground::cpp::misc::sst {

class Table {
public:
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  ~Table();

  static tools::Status open(const Options* options, fs::FsReader* reader, uint64_t size,
                            Table** table);

  tools::Status get(const tools::Binary& key, tools::Binary* value);

private:
  Table(const Options* options, fs::FsReader* reader, Block* index_block);

  void readMeta(const Footer& footer);
  void readFilter(const tools::Binary& filter_handle_content);
  Iterator* blockReader(const tools::Binary& index_value);

private:
  const Options* options_;
  fs::FsReader* reader_;
  tools::Status status_;
  FilterBlockReader* filter_{nullptr};
  const char* filter_data_;
  // BlockHandle metaindex_Handle_;
  Block* index_block_;
};

}  // namespace playground::cpp::misc::sst
