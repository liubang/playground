#include "basecode/logger/buffer.h"
#include <string.h>

namespace basecode {
namespace logger {

Buffer::Buffer(size_t total)
    : total_(total), available_(total), cur_(0), data_(new char[total]) {
  set_cookie(cookie_start);
}

Buffer::~Buffer() {
  delete[] data_;
  set_cookie(cookie_end);
}

size_t Buffer::available() const { return available_; }

void Buffer::append(const char *data, size_t len) {
  memcpy(data_ + cur_, data, len);
  cur_ += len;
  available_ -= len;
}

const char *Buffer::data() const { return data_; }

size_t Buffer::length() const { return cur_; }

void Buffer::cookie_start() {}

void Buffer::cookie_end() {}

void Buffer::clear() {}

void Buffer::set_cookie(const CookieFunc &cookie) { cookie_ = cookie; }

} // namespace logger
} // namespace basecode
