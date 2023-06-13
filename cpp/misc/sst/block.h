//=====================================================================
//
// block.h -
//
// Created by liubang on 2023/06/01 19:45
// Last Modified: 2023/06/01 19:45
//
//=====================================================================
#pragma once

#include "cpp/misc/sst/comparator.h"
#include "cpp/misc/sst/iterator.h"
#include "cpp/misc/sst/sstable_format.h"

namespace pl::misc::sst {

class Block {
 public:
  explicit Block(const BlockContents& content);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  ~Block();

  Iterator* iterator(const Comparator* comparator);

 private:
  class BlockIterator;

 private:
  const char* data_;
  std::size_t size_;
  std::size_t num_restarts_;
  uint32_t restart_offset_;
  bool owned_;
};

}  // namespace pl::misc::sst
