// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#include "cpp/pl/sst/block.h"
#include "cpp/pl/sst/cell.h"
#include "cpp/pl/sst/encoding.h"

#include <cassert>
#include <utility>

namespace pl {

Block::Block(const BlockContents& content)
    : data_(content.data.data()), size_(content.data.size()), owned_(content.heap_allocated) {
    num_restarts_ = decodeInt<uint32_t>(data_ + size_ - 4);
    std::size_t max_num_restarts = (size_ - 4) / 4;
    if (num_restarts_ > max_num_restarts) {
        // invalid block
        size_ = 0;
    } else {
        // 计算restart的起始地址偏移
        restart_offset_ = static_cast<uint32_t>(size_ - (1 + num_restarts_) * 4);
    }
}

Block::Block(Block&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      num_restarts_(std::exchange(other.num_restarts_, 0)),
      restart_offset_(std::exchange(other.restart_offset_, 0)),
      owned_(std::exchange(other.owned_, false)) {}

Block& Block::operator=(Block&& other) noexcept {
    if (this != &other) {
        cleanup();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        num_restarts_ = std::exchange(other.num_restarts_, 0);
        restart_offset_ = std::exchange(other.restart_offset_, 0);
        owned_ = std::exchange(other.owned_, false);
    }
    return *this;
}

void Block::cleanup() noexcept {
    if (owned_ && (data_ != nullptr)) {
        delete[] data_;
    }
    data_ = nullptr;
    size_ = 0;
    num_restarts_ = 0;
    restart_offset_ = 0;
    owned_ = false;
}

Block::~Block() { cleanup(); }

class Block::BlockIterator : public Iterator {
public:
    BlockIterator(ComparatorRef comparator,
                  BlockRef block,
                  const char* data,
                  uint32_t restarts,
                  uint32_t num_restarts)
        : comparator_(std::move(comparator)),
          block_(std::move(block)),
          data_(data),
          restarts_(restarts),
          num_restarts_(num_restarts),
          current_(restarts_),
          current_restart_(num_restarts) {
        // 预分配 key 缓冲区，减少重分配
        cell_key_.reserve(256);
    }

    [[nodiscard]] bool valid() const override { return current_ < restarts_; }

    [[nodiscard]] CellRef cell() const override {
        if (!valid()) {
            return nullptr;
        }
        return cell_;
    }

    [[nodiscard]] Status status() const override { return status_; }

    // return the first key that >= target
    void seek(std::string_view target) override {
        uint32_t left = 0;
        if (!binarySearchIndex(target, &left)) {
            return;
        }
        seekToRestartPoint(left);
        while (parseNextCell()) {
            if (compare(cell_->rowkey(), target) >= 0) {
                return;
            }
        }
    }

    void first() override {
        seekToRestartPoint(0);
        parseNextCell();
    }

    void last() override {
        seekToRestartPoint(num_restarts_ - 1);
        for (;;) {
            if (!parseNextCell() || nextEntryOffset() >= restarts_) {
                break;
            }
        }
    }

    void prev() override {
        assert(valid());
        const uint32_t old = current_;
        while (getRestartOffset(current_restart_) >= old) {
            if (current_restart_ == 0) {
                current_ = restarts_;
                current_restart_ = num_restarts_;
                return;
            }
            --current_restart_;
        }
        seekToRestartPoint(current_restart_);
        for (;;) {
            if (parseNextCell() && nextEntryOffset() < old) {
                break;
            }
        }
    }

    void next() override {
        assert(valid());
        parseNextCell();
    }

    ~BlockIterator() override = default;

private:
    bool binarySearchIndex(std::string_view target, uint32_t* idx) {
        uint32_t left = 0;
        uint32_t right = num_restarts_ - 1;
        uint32_t offset = 0;
        while (left < right) {
            const auto mid = (left + right + 1) / 2;
            offset = getRestartOffset(mid);
            EntryInfo entry;
            const char* key_ptr = parseEntryAt(offset, &entry);
            if (key_ptr == nullptr || (entry.shared != 0) ||
                (entry.rowkey_size >= entry.non_shared)) {
                status_ = Status(StatusCode::kDataCorruption, "invalid entry in block");
                current_ = restarts_;
                current_restart_ = num_restarts_;
                cell_key_.clear();
                val_ = {};
                return false;
            }

            std::string_view mid_key(key_ptr, entry.rowkey_size);
            int cmp = compare(mid_key, target);

            if (cmp == 0) {
                left = mid;
                break;
            }
            if (cmp < 0) {
                left = mid;
            } else {
                right = mid - 1;
            }
        }

        *idx = left;
        return true;
    }

    uint32_t getRestartOffset(uint32_t idx) {
        assert(idx < num_restarts_);
        return decodeInt<uint32_t>(data_ + restarts_ + (idx * sizeof(uint32_t)));
    }

    void seekToRestartPoint(uint32_t idx) {
        cell_key_.clear();
        current_restart_ = idx;
        uint32_t offset = getRestartOffset(idx);
        val_ = std::string_view(data_ + offset, 0);
    }

    [[nodiscard]] inline uint32_t nextEntryOffset() const {
        return static_cast<uint32_t>((val_.data() + val_.size()) - data_);
    }

    bool parseNextCell() {
        current_ = nextEntryOffset();
        const char* p = data_ + current_;
        const char* limit = data_ + restarts_;
        if (p >= limit) {
            current_ = restarts_;
            current_restart_ = num_restarts_;
            status_ = Status(StatusCode::kKVStoreNotFound);
            return false;
        }
        EntryInfo entry;
        p = parseEntryAt(current_, &entry);
        // 第一次Seek的时候，key 为空，那么shared必定为0
        if (p == nullptr || cell_key_.size() < entry.shared) {
            status_ = Status(StatusCode::kDataCorruption, "invalid block");
            return false;
        }
        cell_key_.resize(entry.shared);
        cell_key_.append(p, entry.non_shared);
        val_ = std::string_view(p + entry.non_shared, entry.value_size);
        cell_ = std::make_shared<Cell>(cell_key_, entry.rowkey_size, val_);
        while (current_restart_ + 1 < num_restarts_ &&
               getRestartOffset(current_restart_) < current_) {
            ++current_restart_;
        }

        return true;
    }

    // Entry 信息结构
    struct EntryInfo {
        uint32_t shared;
        uint32_t non_shared;
        uint32_t rowkey_size;
        uint32_t value_size;
        const char* key_data;
        const char* value_data;
    };

    const char* parseEntryAt(uint32_t offset, EntryInfo* info) {
        const char* p = data_ + offset;
        const char* limit = data_ + restarts_;
        constexpr std::size_t s = sizeof(uint32_t);
        if (static_cast<uint32_t>(limit - p) < (s * 4)) {
            return nullptr;
        }
        info->shared = pl::decodeInt<uint32_t>(p);
        info->non_shared = pl::decodeInt<uint32_t>(p + s);
        info->rowkey_size = pl::decodeInt<uint32_t>(p + s * 2);
        info->value_size = pl::decodeInt<uint32_t>(p + s * 3);
        p += s * 4;

        const size_t data_size = info->non_shared + info->value_size;
        if (static_cast<uint32_t>(limit - p) < data_size) {
            return nullptr;
        }

        return p;
    }

    [[nodiscard]] inline int compare(std::string_view a, std::string_view b) const {
        return comparator_->compare(a, b);
    }

private:
    const ComparatorRef comparator_{nullptr}; // 主要是seek的时候做二分查找的
    const BlockRef block_{nullptr};           // 维护block的生命周期
    const char* data_{nullptr};               // data block content
    const uint32_t restarts_;                 // restart的起始位置
    const uint32_t num_restarts_;             // restart的个数
    uint32_t current_{0};                     // 当前游标的偏移
    uint32_t current_restart_{0};             // 当前是第几个restart
    std::string cell_key_;                    // 当前游标处的cellkey
    CellRef cell_{nullptr};                   // 当前的cell
    std::string_view val_;                    // 当前游标处的value
    Status status_{StatusCode::kOK};
};

IteratorPtr Block::iterator(const ComparatorRef& comparator) {
    return std::make_unique<BlockIterator>(comparator, shared_from_this(), data_, restart_offset_,
                                           num_restarts_);
}

} // namespace pl
