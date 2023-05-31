//=====================================================================
//
// sstable_builder.cpp -
//
// Created by liubang on 2023/05/31 19:39
// Last Modified: 2023/05/31 19:39
//
//=====================================================================
#include "cpp/misc/sst/sstable_builder.h"

namespace playground::cpp::misc::sst {

void SSTableBuilder::add(const tools::Binary& key, const tools::Binary& value) {
  if (!ok()) {
    return;
  }

  if (pending_index_entry_) {
    assert(data_block_.empty());
    index_block_.add(last_key_, key);
    pending_index_entry_ = false;
  }

  if (num_entries_ > 0) {
    assert(comparator_->compare(key, last_key_) > 0);
  }

  if (filter_block_ != nullptr) {
    filter_block_->addKey(key);
  }

  last_key_.assign(key.data(), key.size());
  data_block_.add(key, value);
  num_entries_++;

  const size_t s = data_block_.size();
  if (s >= block_size_) {
    flush();
  }
}

void SSTableBuilder::flush() {
  if (!ok() || data_block_.empty()) {
    return;
  }

}

}  // namespace playground::cpp::misc::sst
