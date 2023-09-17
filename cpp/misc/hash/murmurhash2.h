//=====================================================================
//
// murmurhash2.h -
//
// Created by liubang on 2023/05/21 22:46
// Last Modified: 2023/05/21 22:46
//
//=====================================================================
#pragma once

#include <cctype>
#include <cstdint>
#include <type_traits>

namespace pl {

template <typename T,
          T m,
          T r,
          T x,
          T y,
          std::enable_if_t<std::disjunction<std::is_same<T, uint32_t>,
                                            std::is_same<T, uint64_t>>::value> * = nullptr>
class CMurmurHash final {
public:
    CMurmurHash() = default;

    ~CMurmurHash() = default;

    void begin(const T seed = 0) {
        size_ = 0;
        tail_ = 0;
        count_ = 0;
        hash_ = seed;
    }

    void __attribute__((no_sanitize("alignment")))
    add(const void *const data, const T length, const bool lower_case) {
        const auto *data1 = static_cast<const uint8_t *>(data);
        T data_size = length;

        size_ += data_size;
        process_tail(data1, data_size, lower_case);

        auto data2 = reinterpret_cast<const T *>(data1);
        auto stop = data2 + (data_size >> _shift);

        if (lower_case) {
            while (data2 != stop) {
                T k = *data2++;
                k = std::tolower(k);
                mmix(hash_, k);
            }
        } else {
            while (data2 != stop) {
                T k = *data2++;
                mmix(hash_, k);
            }
        }

        data1 = reinterpret_cast<const uint8_t *>(data2);
        data_size &= _mask;

        process_tail(data1, data_size, lower_case);
    }

    T end() {
        mmix(hash_, tail_);
        mmix(hash_, size_);
        hash_ ^= hash_ >> x;
        hash_ *= m;
        hash_ ^= hash_ >> y;
        return hash_;
    }

private:
    void mmix(uint32_t &h, uint32_t &k) const {
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
    }

    void mmix(uint64_t &h, uint64_t &k) const {
        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
    }

    void process_tail(const uint8_t *&data, T &length, const bool lower_case) {
        while (length > 0 && (length < sizeof(T) || count_ > 0)) {
            uint8_t value = *data++;
            if (lower_case && (value >= 'A' && value <= 'Z')) {
                value += 'a' - 'A';
            }
            tail_ |= static_cast<T>(value) << (count_ << 3);
            count_++;
            length--;
            if (count_ == sizeof(T)) {
                mmix(hash_, tail_);
                tail_ = 0;
                count_ = 0;
            }
        }
    }

private:
    static constexpr T _mask = sizeof(T) - 1;
    static constexpr T _shift = (sizeof(T) == 4 ? 2 : 3);
    T size_ = 0;
    T tail_ = 0;
    T count_ = 0;
    T hash_ = 0;
};

using CMurmurHash32 = CMurmurHash<uint32_t, 0x5bd1e995, 24, 13, 15>;
using CMurmurHash64 = CMurmurHash<uint64_t, UINT64_C(0xc6a4a7935bd1e995), 47, 47, 47>;

} // namespace pl
