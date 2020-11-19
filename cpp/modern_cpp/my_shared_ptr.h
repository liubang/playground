#pragma once
#include <atomic>
#include <iostream>

template <typename T>
class my_shared_ptr {
 private:
  T* ptr_;
  std::atomic<int32_t> refcount_{1};

 public:
  my_shared_ptr(T* ptr) : ptr_(ptr) {}
  my_shared_ptr(my_shared_ptr& ptr) {
    this->ptr_ = ptr.getPtr();
    ptr.addRef();
  }
  my_shared_ptr& operator=(my_shared_ptr& ptr) {
    this->ptr_ = ptr.getPtr();
    ptr.addRef();
  }
  my_shared_ptr& operator=(my_shared_ptr&& ptr) = delete;
  my_shared_ptr& operator*() {
    return *ptr_;
  }
  int32_t count() {
    return refcount_.load();
  }
  T* getPtr() {
    return ptr_;
  }
  void addRef() {
    refcount_.fetch_add(1);
  }
  ~my_shared_ptr() {
    if (refcount_.fetch_sub(1) <= 1) {
      std::cout << "free" << std::endl;
      delete ptr_;
    }
  }
};
