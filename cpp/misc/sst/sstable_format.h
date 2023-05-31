//=====================================================================
//
// sstable_format.h -
//
// Created by liubang on 2023/05/31 20:56
// Last Modified: 2023/05/31 20:56
//
//=====================================================================

#include "cpp/tools/binary.h"
#include "cpp/tools/status.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace playground::cpp::misc::sst {

class BlockHandle {
public:
  BlockHandle() : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

  [[nodiscard]] const uint64_t offset() const { return offset_; }
  [[nodiscard]] const uint64_t size() const { return size_; }

  void setOffset(uint64_t offset) { offset_ = offset; }
  void setSize(uint64_t size) { size_ = size; }

  void encodeTo(std::string* dst) const;
  [[nodiscard]] tools::Status decodeFrom(const tools::Binary& input) const;

private:
  uint64_t offset_;
  uint64_t size_;
};

class Footer {};

}  // namespace playground::cpp::misc::sst
