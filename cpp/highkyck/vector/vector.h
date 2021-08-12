#pragma once

#include <new>
#include <utility>
#include <stddef.h>

namespace highkyck {

template<typename ValueT> class vector
{
public:
  using value_type = ValueT;
  using reference = ValueT&;
  using const_reference = const ValueT&;
  using iterator = ValueT*;
  using const_iterator = const ValueT*;
  using size_type = ::size_t;
  using difference_type = ::ptrdiff_t;

private:
  ValueT* m_data_;
  ::size_t m_size_;
  ::size_t m_capacity_;

public:
  constexpr vector() noexcept
      : m_data_()
      , m_size_()
      , m_capacity_()
  {}

  vector(const vector& rhs)
  {
    this->m_data_ = static_cast<ValueT*>(::operator new(rhs.m_capacity_ * sizeof(ValueT)));
    this->m_size_ = 0;
    this->m_capacity_ = rhs.m_capacity_;
    try {
      for (::size_t k = 0; k < rhs.m_size_; ++k) {
        ::new (&this->m_data_[k]) ValueT(rhs.m_data_[k]);
        this->m_size_ += 1;
      }
    }
    catch (...) {
      for (::size_t k = 0; k < this->m_size_; ++k) { this->m_data_[k].~ValueT(); }
      if (this->m_data_) { ::operator delete(this->m_data_); }
      throw;
    }
  }

  vector(vector&& rhs) noexcept
  {
    this->m_data_ = rhs.m_data_;
    this->m_size_ = rhs.m_size_;
    this->m_capacity_ = rhs.m_capacity_;
    rhs.m_data_ = nullptr;
    rhs.m_size = 0;
    rhs.m_capacity_ = 0;
  }

  ~vector()
  {
    for (::size_t k = 0; k < this->m_size_; ++k) { this->m_data_[k].~ValueT(); }
    if (this->m_data_) { ::operator delete[](this->m_data_); }
  }

public:
  iterator begin() noexcept { return this->m_data_; }

  const_iterator begin() const noexcept { return this->m_data_; }

  iterator end() noexcept { return this->m_data_ + this->m_size_; }

  const_iterator end() const noexcept { return this->m_data_ + this->m_size_; }

  value_type* data() noexcept { return this->m_data_; }

  const value_type* data() const noexcept { return this->m_data_; }

  size_type size() const noexcept { return this->m_size_; }

  size_type capacity() const noexcept { return this->m_capacity_; }

  bool empty() const noexcept { return this->m_size_ == 0; }

  void clear() noexcept
  {
    for (::size_t k = 0; k < this->m_size_; ++k) { this->m_data_[k].~ValueT(); }
    this->m_size_ = 0;
  }

  void pop_back() noexcept
  {
    assert(!this->empty());
    ::size_t k = this->m_data_ - 1;
    this->m_data_[k].~ValueT();
    this->m_size_ = k;
  }

  void push_back(const ValueT& value) { this->emplace_back(value); }

  void push_back(const ValueT&& value) { this->emplace_back(value); }

  template<typename... ArgsT> reference emplace_back(ArgsT&&... args)
  {
    if (this->m_size_ < this->m_capacity_) {
      ::size_t k = this->m_size_;
      ::new (&this->m_data_[k]) ValueT(::std::forward<ArgsT>(args)...);
      this->m_size_ += 1;
      return this->m_data_[k];
    }

    ::size_t new_capacity = this->m_capacity_ + 1;
    new_capacity |= this->m_capacity_ / 2;

    auto new_data = static_cast<ValueT*>(::operator new(new_capacity * sizeof(ValueT)));
    ::size_t new_size = 0;

    try {
      for (::size_t k = 0; k < this->m_size_; ++k) {
        ::new (&new_data[k]) ValueT(::std::move(this->m_data_[k]));
        new_size++;
      }
      ::new (&new_data[new_size]) ValueT(::std::forward<ArgsT>(args)...);
      new_size++;
    }
    catch (...) {
      for (::size_t k = 0; k < new_size; ++k) { new_data[k].~ValueT(); }
      ::operator delete(new_data);
      throw;
    }

    for (::size_t k = 0; k < this->m_size_; ++k) { this->m_data_[k].~ValueT(); }
    if (this->m_data_) { ::operator delete(this->m_data_); }

    this->m_data_ = new_data;
    this->m_size_ = new_size;
    this->m_capacity_ = new_capacity;
    return new_data[new_size - 1];
  }
};

}   // namespace highkyck
