#include "format.h"

#include "coding.h"

namespace highkyck {
namespace sstable {

void BlockHandle::encode_to(std::string* dst) const {
  assert(offset_ != ~static_cast<uint64_t>(0));
  assert(size_ != ~static_cast<uint64_t>(0));
  put_varint64(dst, offset_);
  put_varint64(dst, size_);
}

int BlockHandle::decode_from(Slice* input) {}

}  // namespace sstable
}  // namespace highkyck
