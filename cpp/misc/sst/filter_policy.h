//=====================================================================
//
// filter.h -
//
// Created by liubang on 2023/05/29 19:53
// Last Modified: 2023/05/29 19:53
//
//=====================================================================

#pragma once

#include <string>

#include "cpp/tools/binary.h"

namespace pl {

class FilterPolicy {
public:
    virtual ~FilterPolicy();

    /**
     * @brief description]
     *
     * @param keys parameter]
     * @param n parameter]
     * @param dst parameter]
     */
    virtual void createFilter(Binary* keys, std::size_t n, std::string* dst) const = 0;

    /**
     * @brief description]
     *
     * @return return]
     */
    [[nodiscard]] virtual const char* name() const = 0;

    /**
     * @brief description]
     *
     * @param key parameter]
     * @param filter parameter]
     * @return return]
     */
    [[nodiscard]] virtual bool keyMayMatch(const Binary& key, const Binary& filter) const = 0;
};

FilterPolicy* newBloomFilterPolicy(uint64_t bits_per_key);

} // namespace pl
