#pragma once

#include <memory>
#include <cstring>

class Buffer {
  static const int INIT_SIZE = 1024;

 public:
  char* Ptr() {
    return m_buffer_ ? m_buffer_.get() : m_raw_buffer_;
  }

  int Size() const {
    return m_size_;
  }

  void ReAllocate() {
    char* new_buffer = new char[Size() * 2];
    memcpy(new_buffer, Ptr(), Size());
    m_buffer_.reset(new_buffer);
    m_size_ *= 2;
  }

 private:
  char m_raw_buffer_[INIT_SIZE]{};
  std::unique_ptr<char[]> m_buffer_;
  int m_size_ = INIT_SIZE;
};
