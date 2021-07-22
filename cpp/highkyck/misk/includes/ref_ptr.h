#pragma once

#include <atomic>
#include <type_traits>
#include <cassert>

namespace highkyck {

/**
 * shared_ptr的一个不完美的实现
 */
class Referenceable {
 public:
  Referenceable() : ref_(0) {}

  virtual ~Referenceable() = default;

  void AddRef() {
    (void)ref_.fetch_add(1);
  }

  int64_t GetRef() {
    return ref_.load();
  }

  void Release() {
    int64_t n = ref_.fetch_sub(1);
    assert(n >= 0);
    if (n == 0) {
      delete this;
    }
  }

 private:
  std::atomic<int64_t> ref_;
};

template <class T>
class RefPtr {
  static_assert(
      std::is_base_of<Referenceable, T>::value,
      "T is not derived from Referenceable");

 public:
  RefPtr() : ptr_(nullptr) {}

  RefPtr(T* obj) : ptr_(obj) {
    if (obj) {
      ptr_->AddRef();
    }
  }

  RefPtr(const RefPtr& ref_ptr) {
    if (ref_ptr.ptr_) {
      ref_ptr.ptr_->AddRef();
    }
    ptr_ = ref_ptr.ptr_;
  }

  ~RefPtr() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

  T* operator->() {
    return ptr_;
  }

  RefPtr& operator=(const RefPtr& ref_ptr) {
    if (ptr_) {
      ptr_->Release();
    }
    if (ref_ptr.ptr_) {
      ref_ptr.ptr_->AddRef();
    }
    ptr_ = ref_ptr.ptr_;
    return *this;
  }

  RefPtr& operator=(T* obj) {
    if (ptr_) {
      ptr_->Release();
    }
    if (obj) {
      obj->AddRef();
    }
    ptr_ = obj;
    return *this;
  }

  T* get() {
    return ptr_;
  }

 private:
  T* ptr_;
};

} // namespace highkyck
