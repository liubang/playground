//=====================================================================
//
// block_builder.h -
//
// Created by liubang on 2023/05/29 19:36
// Last Modified: 2023/05/29 19:36
//
//=====================================================================
#pragma once

#include <string>
#include <vector>

#include "cpp/misc/sst/comparator.h"
#include "cpp/misc/sst/options.h"
#include "cpp/tools/binary.h"

namespace pl {

/**
 * data block and index block builder
 */
class BlockBuilder {
public:
    // BlockBuilder(const Comparator* comparator, int block_restart_interval);
    BlockBuilder(const Options* options);

    BlockBuilder(const BlockBuilder&) = delete;

    BlockBuilder& operator=(const BlockBuilder&) = delete;

    void add(const Binary& key, const Binary& val);

    Binary finish();

    [[nodiscard]] bool empty() const { return buffer_.empty(); }

    [[nodiscard]] std::size_t sizeEstimate() const;

    void reset();

private:
    const Comparator* comparator_;
    bool finished_{false};
    int counter_{0};
    std::string buffer_;
    std::string last_key_;
    int block_restart_interval_{0};
    std::vector<uint32_t> restarts_;
};

} // namespace pl
