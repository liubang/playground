#pragma once

#include <cassert>
#include <functional>
#include <list>

#include "slice.h"

namespace highkyck {
namespace sstable {

  class Iterator
  {
  public:
    Iterator();

    virtual ~Iterator();

    Iterator(const Iterator &) = delete;
    Iterator &operator=(const Iterator &) = delete;

    virtual bool valid() const = 0;
    virtual void seek_to_first() = 0;
    virtual void seek_to_last() = 0;
    virtual void seek(const Slice &target) = 0;
    virtual void next() = 0;
    virtual void prev() = 0;
    virtual Slice key() const = 0;
    virtual Slice value() const = 0;

    using CleanupFunction = std::function<void(void *, void *)>;
    void RegisterCleanup(CleanupFunction function, void *arg1, void *arg2);

  private:
    struct CleanupNode
    {
      bool is_empty() const { return function == nullptr; }

      void Run()
      {
        assert(function != nullptr);
        (function)(arg1, arg2);
      }

      CleanupFunction function;
      void *arg1;
      void *arg2;
    };

    std::list<CleanupNode *> cleanup_list_;
  };

}// namespace sstable
}// namespace highkyck
