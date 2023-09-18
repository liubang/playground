//=====================================================================
//
// iterator.cpp -
//
// Created by liubang on 2023/06/02 12:00
// Last Modified: 2023/06/02 12:00
//
//=====================================================================

#include "cpp/misc/sst/iterator.h"

namespace pl {

Iterator::Iterator() = default;

Iterator::~Iterator() {
    if (cleanup_funcs_.empty()) {
        return;
    }
    while (!cleanup_funcs_.empty()) {
        const auto func = cleanup_funcs_.front();
        cleanup_funcs_.pop_front();
        func();
    }
}

void Iterator::registerCleanup(const CleanupFunc& func) {
    cleanup_funcs_.push_back(func);
}

} // namespace pl
