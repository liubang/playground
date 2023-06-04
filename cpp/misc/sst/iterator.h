//=====================================================================
//
// iterator.h -
//
// Created by liubang on 2023/06/01 19:49
// Last Modified: 2023/06/01 19:49
//
//=====================================================================

#pragma once

#include "cpp/tools/binary.h"
#include "cpp/tools/status.h"

#include <functional>
#include <list>

namespace playground::cpp::misc::sst {

class Iterator {
public:
  Iterator();
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual ~Iterator();

  virtual void first() = 0;
  virtual void last() = 0;
  virtual void next() = 0;
  virtual void prev() = 0;
  virtual void seek(const tools::Binary& target) = 0;

  [[nodiscard]] virtual tools::Status status() const = 0;
  [[nodiscard]] virtual bool valid() const = 0;
  [[nodiscard]] virtual tools::Binary key() const = 0;
  [[nodiscard]] virtual tools::Binary val() const = 0;

  using CleanupFunc = std::function<void()>;
  void registerCleanup(const CleanupFunc& function);

private:
  std::list<CleanupFunc> cleanup_funcs_;
};

}  // namespace playground::cpp::misc::sst
