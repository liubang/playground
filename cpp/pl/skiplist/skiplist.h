// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#include <memory>

namespace pl {

template <typename Key> struct SkipListNode {
    Key key;
    std::vector<SkipListNode<Key>*> next;
};

template <typename Key, typename Comparator> class SkipList {
public:
    SkipList(int max_height) : max_height_(max_height) {}

    // disable copy and move
    SkipList(const SkipList&) = delete;
    SkipList(SkipList&&) = delete;
    SkipList& operator=(const SkipList&) = delete;
    SkipList& operator=(SkipList&&) = delete;

    ~SkipList() {
        SkipListNode<Key>* cur = head_;
        while (head_ != nullptr) {
            auto* next = head_->next[0];
            delete cur;
            cur = next;
        }
    }

    void add(const Key& key) {
        // 1. 获取一个随机高度
        // 2. 查找该高度下所有位于key之前的节点
        // 3. 创建一个新的节点
        // 4. 依次将该节点插入到2中获取的节点之后
    }

private:
    int max_height_{0};
    SkipListNode<Key>* head_{nullptr};
    Comparator comparator_;
};

} // namespace pl
