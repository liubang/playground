#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>

namespace highkyck {
namespace sstable {

class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char* d, std::size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
  Slice(const char* s) : data_(s), size_(strlen(s)) {}
  Slice(const Slice&) = default;
  Slice& operator=(const Slice&) = default;

  const char* data() const { return data_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  char operator[](std::size_t n) const {
    assert(n < size());
    return data_[n];
  }

  void clear() {
    data_ = "";
    size_ = 0;
  }

  void remove_prefix(std::size_t n) {
    assert(n <= size());
    data_ += n;
    size_ -= n;
  }

  std::string to_string() const { return std::string(data_, size_); }

  int compare(const Slice& b) const;

  bool start_with(const Slice& x) const {
    return ((size_ >= x.size_) && (memcmp(data_, x.data_, x.size_) == 0));
  }

 private:
  const char* data_;
  std::size_t size_;
};

inline bool operator==(const Slice& x, const Slice& y) {
  return ((x.size() == y.size()) &&
          (memcmp(x.data(), y.data(), x.size()) == 0));
}

inline bool operator!=(const Slice& x, const Slice& y) {
  return !(x == y);
}

inline bool operator>(const Slice& x, const Slice& y) {
  return x.compare(y) > 0;
}

inline bool operator<(const Slice& x, const Slice& y) {
  return x.compare(y) < 0;
}

inline int Slice::compare(const Slice& b) const {
  const std::size_t min_len = std::min(size_, b.size());
  int r = memcmp(data_, b.data_, min_len);
  if (r == 0) {
    if (size_ < b.size()) {
      // *this < b
      r = -1;
    } else if (size_ > b.size()) {
      // *this > b
      r = 1;
    }
  }
  return r;
}

}  // namespace sstable
}  // namespace highkyck
