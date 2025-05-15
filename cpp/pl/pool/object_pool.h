// Copyright (c) 2025 The Authors. All rights reserved.
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

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace pl {

template <typename T, size_t NumTLSItem = 64, size_t NumGlobalItem = 64 * 64> class ObjectPool {
    using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;
    using Bucket = std::vector<std::unique_ptr<Storage>>;
    using Buckets = std::vector<Bucket>;

    struct Deleter {
        constexpr Deleter() = default;
        void operator()(T* item) {
            item->~T();
            tls().put(std::unique_ptr<Storage>(reinterpret_cast<Storage*>(item)));
        }
    };

public:
    using Ptr = std::unique_ptr<T, Deleter>;

    static Ptr get() { return Ptr(new (tls().get().release()) T); }

    template <typename... Args> static Ptr get(Args&&... args) {
        return (new (tls().get().release()) T(std::forward<Args>(args)...));
    }

private:
    class ThreadLocalStorage {
    public:
        ThreadLocalStorage(ObjectPool& parent) : parent_(parent) {}

        std::unique_ptr<Storage> get() {
            if (!second_.empty()) {
                auto item = std::move(second_.back());
                second_.pop_back();
                return item;
            }

            if (first_.empty() && !parent_.getBucket(first_)) {
                first_.resize(kTLSMaxNum);
                for (auto& item : first_) {
                    item.reset(new Storage);
                }
            }

            auto item = std::move(first_.back());
            first_.pop_back();
            return item;
        }

        void put(std::unique_ptr<Storage> obj) {
            if (first_.size() < kTLSMaxNum) {
                first_.push_back(std::move(obj));
                return;
            }

            second_.push_back(std::move(obj));
            if (second_.size() >= kTLSMaxNum) {
                parent_.putBucket(std::move(second_));
                second_.clear();
                second_.reserve(kTLSMaxNum);
            }
        }

    private:
        ObjectPool& parent_;
        Bucket first_;
        Bucket second_;
    };

    static auto& tls() {
        static ObjectPool pool;
        thread_local ThreadLocalStorage tls{pool};
        return tls;
    }

    bool getBucket(Bucket& bucket) {
        auto lock = std::unique_lock(mutex_);
        if (global_.empty()) {
            return false;
        }
        bucket = std::move(global_.back());
        global_.pop_back();
        return true;
    }

    void putBucket(Bucket bucket) {
        auto lock = std::unique_lock(mutex_);
        if (global_.size() < kGlobalMaxNum) {
            global_.push_back(std::move(bucket));
        }
    }

private:
    static constexpr auto kTLSMaxNum = std::max(1ul, NumTLSItem);
    static constexpr auto kGlobalMaxNum = std::max(1ul, NumGlobalItem / NumTLSItem);

    std::mutex mutex_;
    Buckets global_;
};

} // namespace pl
