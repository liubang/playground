//=====================================================================
//
// sstable_format.cpp -
//
// Created by liubang on 2023/05/31 23:24
// Last Modified: 2023/05/31 23:24
//
//=====================================================================

#include "cpp/misc/sst/sstable_format.h"
#include "cpp/misc/sst/encoding.h"

#include <memory>

namespace playground::cpp::misc::sst {

void BlockHandle::encodeTo(std::string* dst) const {
  assert(offset_ != ~static_cast<uint64_t>(0));
  assert(size_ != ~static_cast<uint64_t>(0));
  encodeInt(dst, offset_);
  encodeInt(dst, size_);
}

tools::Status BlockHandle::decodeFrom(const tools::Binary& input) {
  if (input.size() < 16) {
    return tools::Status::NewCorruption("bad block handle");
  }
  offset_ = decodeInt<uint64_t>(input.data());
  size_ = decodeInt<uint64_t>(input.data() + 8);
  return tools::Status::NewOk();
}

/*
 * footer format
 * +-----------------+-------------+--------------+-------------------+
 * | metaindex (16B) | index (16B) | padding (8B) | magic number (8B) |
 * +-----------------+-------------+--------------+-------------------+
 * | <------------------  40B ------------------> |
 *
 */
void Footer::encodeTo(std::string* dst) const {
  const std::size_t s = dst->size();
  metaindex_handle_.encodeTo(dst);
  index_handle_.encodeTo(dst);
  dst->resize(2 * BlockHandle::kMaxEncodedLength);
  encodeInt(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
  encodeInt(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));
  assert(dst->size() == s + kEncodedLength);
}

tools::Status Footer::decodeFrom(const tools::Binary& input) {
  if (input.size() < kEncodedLength) {
    return tools::Status::NewCorruption("invalid sstable format");
  }
  const char* magic_ptr = input.data() + kEncodedLength - 8;
  const auto magic_lo = decodeInt<uint32_t>(magic_ptr);
  const auto magic_hi = decodeInt<uint32_t>(magic_ptr + 4);
  const uint64_t magic =
      ((static_cast<uint64_t>(magic_hi) << 32 | (static_cast<uint64_t>(magic_lo))));
  if (magic != kTableMagicNumber) {
    return tools::Status::NewCorruption("invalid magic number");
  }
  tools::Status result = metaindex_handle_.decodeFrom(input);
  if (result.isOk()) {
    // TODO(liubang): 优化没必要的拷贝
    result = index_handle_.decodeFrom(tools::Binary(input.data() + 16, input.size() - 16));
  }
  return result;
}

tools::Status BlockReader::readBlock(fs::FsReader* reader, const BlockHandle& handle,
                                     BlockContents* result) {
  // read block trailer
  auto s = static_cast<std::size_t>(handle.size());
  std::unique_ptr<char[]> buf = std::make_unique<char[]>(s + kBlockTrailerSize);

  tools::Binary content;
  auto status = reader->read(handle.offset(), s + kBlockTrailerSize, &content, buf.get());
  if (!status.isOk()) {
    return status;
  }
  // invalid content
  if (content.size() != s + kBlockTrailerSize) {
    return tools::Status::NewCorruption("invalid block");
  }
}

}  // namespace playground::cpp::misc::sst
