//=====================================================================
//
// table.cpp -
//
// Created by liubang on 2023/06/04 00:46
// Last Modified: 2023/06/04 00:46
//
//=====================================================================

#include "cpp/misc/sst/table.h"

#include <memory>

namespace playground::cpp::misc::sst {

Table::Table(const Options& options, fs::FsReader* reader, const BlockHandle& metaindex_handle,
             Block* index_block)
    : options_(options),
      reader_(reader),
      metaindex_Handle_(metaindex_handle),
      index_block_(index_block) {}

Table::~Table() {
  if (filter_ != nullptr) delete filter_;
  if (filter_data_ != nullptr) delete[] filter_data_;
  if (index_block_ != nullptr) delete index_block_;
}

tools::Status Table::open(const Options& options, fs::FsReader* reader, uint64_t size,
                          Table** table) {
  *table = nullptr;
  if (size < Footer::kEncodedLength) {
    return tools::Status::NewCorruption("file is too short to be an sstable");
  }
  char footer_content[Footer::kEncodedLength];
  tools::Binary footer_input;
  auto s = reader->read(size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input,
                        footer_content);
  if (!s.isOk()) return s;
  Footer footer;
  s = footer.decodeFrom(footer_input);
  if (!s.isOk()) return s;

  // parse index block
  BlockContents index_block_contents;
  s = BlockReader::readBlock(reader, footer.indexHandle(), &index_block_contents);
  if (!s.isOk()) return s;

  auto* index_block = new Block(index_block_contents);

  *table = new Table(options, reader, footer.metaindexHandle(), index_block);
  (*table)->readMeta(footer);
  return s;
}

void Table::readMeta(const Footer& footer) {
  if (options_.filter_policy == nullptr) {
    return;
  }

  BlockContents contents;
  if (BlockReader::readBlock(reader_, footer.metaindexHandle(), &contents).isOk()) {
    return;
  }

  auto meta = std::make_unique<Block>(contents);
  auto* iter = meta->iterator(options_.comparator);

  std::string key = "filter.";
  key.append(options_.filter_policy->name());
  iter->seek(key);
  if (iter->valid() && iter->key() == tools::Binary(key)) {
    readFilter(iter->val());
  }
  delete iter;
}

void Table::readFilter(const tools::Binary& filter_handle_value) {
  BlockHandle filter_handle;
  if (!filter_handle.decodeFrom(filter_handle_value).isOk()) {
    return;
  }
  BlockContents block;
  if (!BlockReader::readBlock(reader_, filter_handle, &block).isOk()) {
    return;
  }
  filter_ = new FilterBlockReader(options_.filter_policy, block.data);
}

tools::Status Table::get(const tools::Binary& key, tools::Binary* value) {
  tools::Status s;
  auto* iiter = index_block_->iterator(options_.comparator);
  iiter->seek(key);
  if (iiter->valid()) {
    tools::Binary handle_value = iiter->val();
    BlockHandle handle;
    if (filter_ != nullptr && handle.decodeFrom(handle_value).isOk() &&
        !filter_->keyMayMatch(handle.offset(), key)) {
      // not found
    } else {
      // key may found
      auto* iter = blockReader(iiter->val());
      if (nullptr == iter) {
        delete iiter;
        return tools::Status::NewNotFound();
      }

      iter->seek(key);
      if (iter->valid()) {
        *value = iter->val();
      }
      s = iter->status();
      delete iter;
    }
  }

  delete iiter;
  return s;
}

Iterator* Table::blockReader(const tools::Binary& index_value) {
  Block* block = nullptr;
  BlockHandle handle;
  auto s = handle.decodeFrom(index_value);
  if (s.isOk()) {
    BlockContents contents;
    auto s = BlockReader::readBlock(reader_, handle, &contents);
    if (s.isOk()) [[maybe_unused]]
      auto* block = new Block(contents);
  }

  Iterator* iter = nullptr;
  if (nullptr != block) {
    iter = block->iterator(options_.comparator);
    iter->registerCleanup([&block]() { delete block; });
  }
  return iter;
}

}  // namespace playground::cpp::misc::sst
