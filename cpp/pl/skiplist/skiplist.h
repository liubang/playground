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

#include "cpp/pl/utility/utility.h"

#include <iostream>
#include <random>
#include <vector>

namespace pl {

template <typename Key> struct SkipListNode {
    Key key;
    std::vector<SkipListNode<Key>*> next;

    SkipListNode(const Key& key, int height) : key(key), next(height + 1, nullptr) {}
};

template <typename Key, typename Comparator, int MAX_HEIGHT = 31>
class SkipList final : public DisableCopyAndMove {
public:
    SkipList() : head_(new SkipListNode<Key>(0, MAX_HEIGHT)) {}

    ~SkipList() {
        SkipListNode<Key>* cur = head_;
        while (cur != nullptr) {
            auto* next = cur->next[0];
            delete cur;
            cur = next;
        }
    }

    void insert(const Key& key) {
        int height = random_height();
        ::printf("height: %d\n", height);
        SkipListNode<Key>* node = new SkipListNode<Key>(key, height);
        for (int i = height; i >= 0; --i) {
            SkipListNode<Key>* pre = head_;
            SkipListNode<Key>* cur = head_->next[i];
            while (cur != nullptr && comparator_.compare(cur, node) < 0) {
                pre = cur;
                cur = cur->next[i];
            }
            node->next[i] = pre->next[i];
            pre->next[i] = node;
        }
    }

    void remove(const Key& key) {
        // TODO
    }

    void display() {
        for (int i = MAX_HEIGHT; i >= 0; --i) {
            SkipListNode<Key>* cur = head_->next[i];
            std::cout << '[' << i << "]\t: ";
            while (cur != nullptr) {
                std::cout << (*cur->key) << ",";
                cur = cur->next[i];
            }
            std::cout << "nullptr\n";
        }
    }

private:
    // TODO
    int random_height() {
        std::random_device dev;
        std::mt19937 rng(dev());
        std::uniform_int_distribution<std::mt19937::result_type> dist(0, MAX_HEIGHT);
        return dist(rng);
    }

private:
    SkipListNode<Key>* head_{nullptr};
    Comparator comparator_;
};

} // namespace pl
