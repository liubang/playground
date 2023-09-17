//=====================================================================
//
// bloom_filter_policy.cpp -
//
// Created by liubang on 2023/05/30 15:45
// Last Modified: 2023/05/30 15:45
//
//=====================================================================

#include "cpp/misc/bloom/bloom_filter.h"
#include "cpp/misc/sst/filter_policy.h"

namespace pl {

/**
 * @class BloomFilterPolicy
 * @brief this class is a proxy of bloom::BloomFilter
 */
class BloomFilterPolicy : public FilterPolicy {
public:
    BloomFilterPolicy(std::size_t bits_per_key) : bits_per_key_(bits_per_key) {}

    ~BloomFilterPolicy() override = default;

    void createFilter(Binary *keys, std::size_t n, std::string *dst) const override {
        BloomFilter bloom_filter(bits_per_key_);
        bloom_filter.create(keys, n, dst);
    }

    [[nodiscard]] const char *name() const override { return "BloomFilterPolicy"; }

    [[nodiscard]] bool keyMayMatch(const Binary &key, const Binary &filter) const override {
        BloomFilter bloom_filter(bits_per_key_);
        return bloom_filter.contains(key, filter);
    }

private:
    std::size_t bits_per_key_;
};

FilterPolicy *newBloomFilterPolicy(uint64_t bits_per_key) {
    return new BloomFilterPolicy(bits_per_key);
}

} // namespace pl
