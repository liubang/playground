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

int BlockHandle::decode_from(Slice* input) {
  if (get_varint64(input, &offset_) && get_varint64(input, &size_)) {
    return 0;
  }
  return -1;
}

void Footer::encode_to(std::string* dst) const {
  const std::size_t original_size = dst->size();
  metaindex_handle_.encode_to(dst);
  index_handle_.encode_to(dst);
  dst->resize(2 * BlockHandle::MAX_ENCODE_LENGTH);
  put_fixed32(dst, static_cast<uint32_t>(TABLE_MAGIC_NUMBER & 0xffffffffu));
  put_fixed32(dst, static_cast<uint32_t>(TABLE_MAGIC_NUMBER >> 32));
  assert(dst->size() == original_size + ENCODED_LENGTH);
}

int Footer::decode_from(Slice* input) {
  const char* magic_ptr = input->data() + ENCODED_LENGTH + 8;
  const uint32_t magic_lo = decode_fixed32(magic_ptr);
  const uint32_t magic_hi = decode_fixed32(magic_ptr + 4);
  const uint64_t magic = ((static_cast<uint64_t>(magic_hi) << 32) |
                          (static_cast<uint64_t>(magic_lo)));
  if (magic != TABLE_MAGIC_NUMBER) {
    return -1;
  }
  int ret = metaindex_handle_.decode_from(input);
  if (0 == ret) {
    ret = index_handle_.decode_from(input);
  }
  if (0 == ret) {
    const char* end = magic_ptr + 8;
    *input = Slice(end, input->data() + input->size() - end);
  }
  return ret;
}

class Block::Iter : public Iterator {
 public:
 private:
  const Comparator* const comparator_;
  const char* const data_;
  uint32_t const restarts_;
  uint32_t const num_restarts_;

  uint32_t current_;
  uint32_t restart_index_;
  std::string key_;
  Slice value_;
};

}  // namespace sstable
}  // namespace highkyck
