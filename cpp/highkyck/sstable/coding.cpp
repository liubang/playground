#include "coding.h"

namespace highkyck {
namespace sstable {

void put_fixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  encode_fixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void put_fixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  encode_fixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

char* encode_varint32(char* dst, uint32_t value) {
  static constexpr int B = 128;
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  if (value < (1 << 7)) {
    *(ptr++) = value;
  } else if (value < (1 << 14)) {
    *(ptr++) = value | B;
    *(ptr++) = value >> 7;
  } else if (value < (1 << 21)) {
    *(ptr++) = value | B;
    *(ptr++) = (value >> 7) | B;
    *(ptr++) = value >> 14;
  } else if (value < (1 << 28)) {
    *(ptr++) = value | B;
    *(ptr++) = (value >> 7) | B;
    *(ptr++) = (value >> 14) | B;
    *(ptr++) = value >> 21;
  } else {
    *(ptr++) = value | B;
    *(ptr++) = (value >> 7) | B;
    *(ptr++) = (value >> 14) | B;
    *(ptr++) = (value >> 21) | B;
    *(ptr++) = value >> 28;
  }
  return reinterpret_cast<char*>(ptr);
}

void put_varint32(std::string* dst, uint32_t value) {
  char buf[5];
  char* ptr = encode_varint32(buf, value);
  dst->append(buf, ptr - buf);
}

char* encode_varint64(char* dst, uint64_t value) {
  static constexpr int B = 128;
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  while (v >= B) {
    *(ptr++) = value | B;
    value >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(value);
  return reinterpret_cast<char*>(ptr);
}

void put_varint64(std::string* dst, uint64_t value) {
  char buf[10];
  char* ptr = encode_varint64(buf, value);
  dst->append(buf, ptr - buf);
}

const char* get_varint32_ptr_fallback(const char* p,
                                      const char* limit,
                                      uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) {
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

bool get_varint32(Slice* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = get_varint32_ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  } else {
    *input = Slice(q, limit - q);
    return true;
  }
}

const char* get_varint64_ptr(const char* p,
                             const char* limit,
                             uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) {
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

bool get_varint64(Slice* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = get_varint64_ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  } else {
    *input = Slice(q, limit - q);
    return true;
  }
}

}  // namespace sstable
}  // namespace highkyck
