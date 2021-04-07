#pragma once

#include <stddef.h>
#include <exception>

template <class T>
class shard_ptr {
 public:
  shard_ptr(T* t) : t_(t), ref_count_(new size_t) {
    if (t_) {
      *ref_count_ = 1;
    } else {
      *ref_count_ = 0;
    }
  }

  shard_ptr(const shard_ptr& ptr) {
    if (this != &ptr) {
      t_ = ptr.t_;
      ref_count_ = ptr.ref_count_;
      (*ref_count_)++;
    }
  }

  shard_ptr& operator=(const shard_ptr& ptr) {
    if (t_ == ptr.t_) {
      return *this;
    }
    release();
    t_ = ptr.t_;
    ref_count_ = ptr.ref_count_;
    (*ref_count_)++;
    return *this;
  }

  T& operator*() {
    if (t_) {
      return *t_;
    }
    throw std::bad_exception();
  }

  T* operator->() {
    if (t_) {
      return t_;
    }
    throw std::bad_exception();
  }

  int getCount() {
    return *ref_count_;
  }

  ~shard_ptr() {
    release();
  }

 private:
  void release() {
    if (t_) {
      (*ref_count_)--;
      if (*ref_count_ == 0) {
        delete t_;
        delete ref_count_;
      }
    }
  }

 private:
  T* t_;
  size_t* ref_count_;
};
