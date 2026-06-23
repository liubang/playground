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
// Created: 2024/08/14 13:02

#pragma once

#include <atomic>
#include <cstddef>
#include <random>
#include <vector>

#include "cpp/pl/utility/utility.h"

namespace pl {

// ---------------------------------------------------------------------------
// SkipList — 并发安全的有序跳表 (Single Writer / Multiple Readers)
// ---------------------------------------------------------------------------
//
// 约束:
//   - Key 和 Value 必须 default-constructible (用于头节点哨兵)
//   - Comparator::compare(const Key&, const Key&) 必须定义严格弱序, 返回:
//       < 0  (a < b)
//         0  (a == b)
//       > 0  (a > b)
//   - insert() 单线程使用 (braft on_apply), 与读操作可并发
//   - remove() 单线程使用, 不得与任何操作并发 (API 保证由调用方负责)
//
// 线程模型:
//   - 写者独占: braft on_apply 单线程顺序调用 insert()
//   - 读者无锁: 依赖从 level 0 向上的节点发布顺序保证遍历安全
//   - 节点回收: MemStore 整个 swap 替换后才批量析构 (append-only 期间不删除)
//
template <typename Key, typename Value, typename Comparator, int MAX_HEIGHT = 16>
class SkipList final : public DisableCopyAndMove {
public:
    struct Node {
        Key key;
        Value value;
        std::vector<Node*> next;

        Node(const Key& k, const Value& v, int height)
            : key(k), value(v), next(height + 1, nullptr) {}
    };

    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    SkipList() : head_(new Node(Key{}, Value{}, MAX_HEIGHT)) {
        memory_usage_.store(estimate_node_memory(head_), std::memory_order_relaxed);
    }

    ~SkipList() {
        Node* cur = head_;
        while (cur != nullptr) {
            Node* nxt = cur->next[0];
            delete cur;
            cur = nxt;
        }
    }

    // -----------------------------------------------------------------------
    // 写入 (非线程安全 — 需外部同步)
    // -----------------------------------------------------------------------

    // 插入 key-value. 重复 key 按 FIFO 顺序排列 (新节点插入到相等 key 之后)
    void insert(const Key& key, const Value& value) {
        int height = random_height();

        // 查找每一层的前驱节点 (<= 保证 FIFO: 新节点在等值 key 之后)
        Node* cur = head_;
        for (int i = MAX_HEIGHT; i >= 0; --i) {
            while (cur->next[i] != nullptr && cmp_.compare(cur->next[i]->key, key) <= 0) {
                cur = cur->next[i];
            }
            if (i <= height) {
                prev_[i] = cur;
            }
        }

        // 构建节点 + 从下到上发布 (先 level 0, 保证读者从任意高层回落时能找到)
        auto* node = new Node(key, value, height);
        for (int i = 0; i <= height; ++i) {
            node->next[i] = prev_[i]->next[i];
            prev_[i]->next[i] = node;
        }

        size_.fetch_add(1, std::memory_order_relaxed);
        memory_usage_.fetch_add(estimate_node_memory(node), std::memory_order_relaxed);
    }

    // remove() — 非线程安全, 物理摘除节点.
    // 约束: 不得与任何读/写操作并发. MemStore 正常写入路径不使用此接口.
    bool remove(const Key& key) {
        Node* cur = head_;
        bool found = false;

        for (int i = MAX_HEIGHT; i >= 0; --i) {
            while (cur->next[i] != nullptr && cmp_.compare(cur->next[i]->key, key) < 0) {
                cur = cur->next[i];
            }
            prev_[i] = cur;
            if (cur->next[i] != nullptr && cmp_.compare(cur->next[i]->key, key) == 0) {
                found = true;
            }
        }

        if (!found) {
            return false;
        }

        Node* target = prev_[0]->next[0];
        int target_height = static_cast<int>(target->next.size()) - 1;

        for (int i = target_height; i >= 0; --i) {
            if (prev_[i]->next[i] == target) {
                prev_[i]->next[i] = target->next[i];
            }
        }

        size_.fetch_sub(1, std::memory_order_relaxed);
        memory_usage_.fetch_sub(estimate_node_memory(target), std::memory_order_relaxed);
        delete target;
        return true;
    }

    // -----------------------------------------------------------------------
    // 读取 (多线程安全 — wait-free, 但不能与 remove() 并发)
    // -----------------------------------------------------------------------

    // 返回 key >= search_key 的第一个节点, 未找到返回 nullptr
    [[nodiscard]] const Node* find_first_gte(const Key& search_key) const {
        Node* cur = head_;
        for (int i = MAX_HEIGHT; i >= 0; --i) {
            while (cur->next[i] != nullptr && cmp_.compare(cur->next[i]->key, search_key) < 0) {
                cur = cur->next[i];
            }
        }
        return cur->next[0];
    }

    // 返回 key > search_key 的第一个节点, 未找到返回 nullptr
    [[nodiscard]] const Node* find_first_gt(const Key& search_key) const {
        Node* cur = head_;
        for (int i = MAX_HEIGHT; i >= 0; --i) {
            while (cur->next[i] != nullptr && cmp_.compare(cur->next[i]->key, search_key) <= 0) {
                cur = cur->next[i];
            }
        }
        return cur->next[0];
    }

    [[nodiscard]] bool contains(const Key& key) const {
        const Node* node = find_first_gte(key);
        return node != nullptr && cmp_.compare(node->key, key) == 0;
    }

    // -----------------------------------------------------------------------
    // 容量
    // -----------------------------------------------------------------------

    [[nodiscard]] size_t size() const { return size_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] size_t memory_usage() const {
        return memory_usage_.load(std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // 迭代器 (只读, 前向, 沿 level-0 链表遍历)
    // -----------------------------------------------------------------------

    class ConstIterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const Node;
        using difference_type = std::ptrdiff_t;
        using pointer = const Node*;
        using reference = const Node&;

        ConstIterator() = default;
        explicit ConstIterator(Node* node) : current_(node) {}

        reference operator*() const { return *current_; }
        pointer operator->() const { return current_; }

        ConstIterator& operator++() {
            current_ = current_->next[0];
            return *this;
        }

        ConstIterator operator++(int) {
            ConstIterator tmp = *this;
            current_ = current_->next[0];
            return tmp;
        }

        bool operator==(const ConstIterator& other) const { return current_ == other.current_; }
        bool operator!=(const ConstIterator& other) const { return current_ != other.current_; }

    private:
        Node* current_{nullptr};
    };

    [[nodiscard]] ConstIterator begin() const { return ConstIterator(head_->next[0]); }
    [[nodiscard]] ConstIterator end() const { return ConstIterator(nullptr); }

    // 返回 key >= search_key 的起始迭代器
    [[nodiscard]] ConstIterator lower_bound(const Key& search_key) const {
        Node* cur = head_;
        for (int i = MAX_HEIGHT; i >= 0; --i) {
            while (cur->next[i] != nullptr && cmp_.compare(cur->next[i]->key, search_key) < 0) {
                cur = cur->next[i];
            }
        }
        return ConstIterator(cur->next[0]);
    }

    // -----------------------------------------------------------------------
    // 内部实现
    // -----------------------------------------------------------------------

private:
    // 几何分布: 从 level 0 开始, 每次以概率 1/2 升层
    int random_height() {
        // MemStore 插入单线程, static 即可安全
        static std::random_device rd;
        static std::mt19937 rng(rd());
        static std::uniform_int_distribution<int> dist(0, 1);

        int height = 0;
        while (height < MAX_HEIGHT && dist(rng) == 0) {
            ++height;
        }
        return height;
    }

    // 估算节点占用的堆内存
    static size_t estimate_node_memory(const Node* node) {
        size_t mem = sizeof(Node);
        mem += node->next.capacity() * sizeof(Node*);
        // 对 std::string 等堆分配类型, sizeof 仅统计内联部分.
        // 更精确的估算需要 MemoryTrait 特化, Phase 1 采用保守近似.
        mem += sizeof(Key);
        mem += sizeof(Value);
        return mem;
    }

    Node* head_; // 哨兵头节点 (构造后不变)
    Comparator cmp_;
    std::atomic<size_t> size_{0};
    std::atomic<size_t> memory_usage_{0};

    // insert/remove 复用缓冲区, 存储每层前驱节点
    Node* prev_[MAX_HEIGHT + 1];
};

} // namespace pl
