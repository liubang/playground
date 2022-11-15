#pragma once

#include <cstdint>
#include <string>

#include "iterator.h"
#include "slice.h"

namespace highkyck {
namespace sstable {

struct BlockContent {
  Slice data;           // actual contents of data
  bool cacheable;       // true iff data can be cached
  bool heap_allocated;  // true iff caller should delete[] data.data()
};

class Comparator;

class Block {
 public:
  explicit Block(const BlockContent& contents);
  ~Block();

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  std::size_t size() const { return size_; }
  Iterator* new_iterator(const Comparator* comparator);

 private:
  uint32_t num_restarts() const;

 private:
  class Iter;

  const char* data_;
  std::size_t size_;
  uint32_t restart_offset_;
  bool owned_;  // true iff Block owned data[]
};

class BlockHandle {
 public:
  BlockHandle()
      : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

  inline uint64_t offset() const { return offset_; }
  inline uint64_t size() const { return size_; }

  void set_offset(uint64_t offset) { offset_ = offset; }
  void set_size(uint64_t size) { size_ = size; }

  void encode_to(std::string* dst) const;
  int decode_from(Slice* input);

  enum { MAX_ENCODE_LENGTH = 10 + 10 };

 private:
  uint64_t offset_;
  uint64_t size_;
};

class Footer {
 public:
  Footer() = default;

  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  const BlockHandle& index_handle() const { return index_handle_; }

  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void encode_to(std::string* dst) const;
  int decode_from(Slice* input);

  enum { ENCODED_LENGTH = 2 * BlockHandle::MAX_ENCODE_LENGTH + 8 };

 private:
  BlockHandle metaindex_handle_;  // for metadata index block
  BlockHandle index_handle_;      // for index block
};

// echo http://code.google.com/p/leveldb/ | sha1sum
// and taking the leading 64 bits.
constexpr uint64_t TABLE_MAGIC_NUMBER = 0xdb4775248b80fb57ull;

constexpr std::size_t BLOCK_TRAILER_SIZE = 5;

}  // namespace sstable
}  // namespace highkyck
