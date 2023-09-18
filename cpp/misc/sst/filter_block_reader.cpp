//=====================================================================
//
// filter_block_reader.cpp -
//
// Created by liubang on 2023/05/30 14:52
// Last Modified: 2023/05/30 14:52
//
//=====================================================================

#include "cpp/misc/sst/filter_block_reader.h"

#include "cpp/misc/sst/encoding.h"

namespace pl {

FilterBlockReader::FilterBlockReader(const FilterPolicy* filter_policy, const pl::Binary& contents)
    : filter_policy_(filter_policy) {
    std::size_t n = contents.size();
    if (n < 5)
        return;
    base_lg_       = static_cast<std::size_t>(contents[n - 1]);
    auto last_word = decodeInt<uint32_t>(contents.data() + n - 5);
    if (last_word > n - 5)
        return;
    data_   = contents.data();
    offset_ = data_ + last_word;
    num_    = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::keyMayMatch(uint64_t block_offset, const Binary& key) {
    uint64_t idx = block_offset >> base_lg_;
    if (idx < num_) {
        auto start = decodeInt<uint32_t>(offset_ + idx * 4);
        auto limit = decodeInt<uint32_t>(offset_ + idx * 4 + 4);
        if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
            return filter_policy_->keyMayMatch(key, Binary(data_ + start, limit - start));
        }
        return false;
    }
    return true;
}

} // namespace pl
