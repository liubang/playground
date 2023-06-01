//=====================================================================
//
// block.cpp -
//
// Created by liubang on 2023/06/01 19:45
// Last Modified: 2023/06/01 19:45
//
//=====================================================================
#include "cpp/misc/sst/block.h"
#include "cpp/misc/sst/encoding.h"

namespace playground::cpp::misc::sst {

Block::Block(const BlockContents& content)
    : data_(content.data.data()), size_(content.data.size()), owned_(content.heap_allocated) {
  num_restarts_ = decodeInt<uint32_t>(data_ + size_ - 4);
  std::size_t max_num_restarts = (size_ - 4) / 4;
  if (num_restarts_ > max_num_restarts) {
    // invalid block
    size_ = 0;
  } else {
    // 计算restart的起始地址偏移
    restart_offset_ = size_ - (1 + num_restarts_) * 4;
  }
}

Block::~Block() {
  if (owned_) {
    delete[] data_;
  }
}

class Block::BlockIterator : public Iterator {
public:
  BlockIterator(const Comparator* comparator, const char* data, uint32_t restarts,
                uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(0),
        current_end_(0),
        current_restart_(0) {}

  [[nodiscard]] bool valid() const override { return current_ < restarts_; }
  [[nodiscard]] tools::Binary key() const override { return key_; }
  [[nodiscard]] tools::Binary val() const override { return val_; }

  void prev() override {}
  void next() override {}

public:
  inline void decodeKeyVal() {
    if (current_ == current_end_) {
      if (valid()) {
        current_restart_++;
        if (current_restart_ == num_restarts_) {
          current_end_ = restarts_;
        } else {
          auto curr = decodeInt<uint32_t>(data_ + ((current_restart_ - 1) * 4));
          auto next = decodeInt<uint32_t>(data_ + (current_restart_ * 4));
          current_end_ = next - curr;
        }
      }
    }
    assert(current_ + 12 < current_end_);
    auto shared = playground::cpp::misc::sst::decodeInt<uint32_t>(data_ + current_);
    auto non_shared = playground::cpp::misc::sst::decodeInt<uint32_t>(data_ + current_ + 4);
    auto value_size = playground::cpp::misc::sst::decodeInt<uint32_t>(data_ + current_ + 8);

    assert(current_ + 12 + non_shared + value_size <= current_end_);
    assert(key_.size() >= shared);

    std::string key(key_.data(), shared);
    key.append(data_ + current_ + 12, non_shared);
    key_ = key;
    val_.reset(data_ + current_ + 12 + non_shared, value_size);
    current_ += 12 + non_shared + value_size;
  }

private:
  const Comparator* comparator_;  // 主要是seek的时候做二分查找的
  const char* data_;              // data block content
  uint32_t const restarts_;       // restart的起始位置
  uint32_t const num_restarts_;   // restart的个数
  uint32_t current_;              // 当前游标的位置
  uint32_t current_end_;          // 当前block的结束位置
  uint32_t current_restart_;      // 当前是第几个restart
  std::string key_;               // 当前游标处的key
  tools::Binary val_;             // 当前游标处的value
};

}  // namespace playground::cpp::misc::sst
