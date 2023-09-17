//=====================================================================
//
// table.h -
//
// Created by liubang on 2023/06/04 00:36
// Last Modified: 2023/06/04 00:36
//
//=====================================================================
#pragma once

#include "cpp/misc/fs/fs.h"
#include "cpp/misc/sst/block.h"
#include "cpp/misc/sst/filter_block_reader.h"
#include "cpp/misc/sst/options.h"
#include "cpp/misc/sst/sstable_format.h"
#include "cpp/tools/status.h"

namespace pl {

class Table {
public:
    Table(const Table &) = delete;
    Table &operator=(const Table &) = delete;

    ~Table();

    static Status open(const Options *options, FsReader *reader, uint64_t size, Table **table);

    Status get(const Binary &key, Binary *value);

private:
    Table(const Options *options, FsReader *reader, Block *index_block);

    void readMeta(const Footer &footer);
    void readFilter(const Binary &filter_handle_content);
    Iterator *blockReader(const Binary &index_value);

private:
    const Options *options_;
    FsReader *reader_;
    Status status_;
    FilterBlockReader *filter_{nullptr};
    const char *filter_data_;
    // BlockHandle metaindex_Handle_;
    Block *index_block_;
};

} // namespace pl
