#pragma once

#include "iterator.h"

namespace highkyck {
namespace sstable {

  Iterator::Iterator() {}

  Iterator::~Iterator()
  {
    while (!cleanup_list_.empty()) {
      CleanupNode *node = cleanup_list_.front();
      cleanup_list_.pop_front();
      node->Run();
      delete node;
    }
  }

  void Iterator::RegisterCleanup(CleanupFunction function, void *arg1, void *arg2)
  {
    assert(function != nullptr);
    CleanupNode *node = new CleanupNode;
    node->function = function;
    node->arg1 = arg1;
    node->arg2 = arg2;
    cleanup_list_.push_back(node);
  }

}// namespace sstable
}// namespace highkyck
