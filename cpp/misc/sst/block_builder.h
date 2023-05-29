//=====================================================================
//
// block_builder.h -
//
// Created by liubang on 2023/05/29 19:36
// Last Modified: 2023/05/29 19:36
//
//=====================================================================

#pragma once

#include "cpp/tools/binary.h"

#include <string>

namespace playground::cpp::misc::sst {

class BlockBuilder {
public:
  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  void add(const tools::Binary& key, const tools::Binary& val);

  tools::Binary finish();

  [[nodiscard]] std::size_t size() const;

  [[nodiscard]] bool empty() const { return buffer_.empty(); }

  void reset();

private:
  std::string buffer_;
  bool finished_;
  std::string last_key_;
};

}  // namespace playground::cpp::misc::sst
