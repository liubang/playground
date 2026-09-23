// Copyright (c) 2026 The Authors. All rights reserved.
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
// Created: 2026/09/23 23:05

// CRoaring 用法示例：bazel run //cpp/pl/croaring:demo

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <roaring/roaring.hh>
#include <roaring/roaring64.hh>
#include <roaring/roaring64map.hh>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using roaring::BulkContext;
using roaring::Roaring;
using roaring::Roaring64;
using roaring::Roaring64Map;

void title(const std::string& name) {
    std::cout << "\n=== " << name << " ===\n";
}

std::vector<uint32_t> toVector(const Roaring& bitmap) {
    std::vector<uint32_t> values(bitmap.cardinality());
    bitmap.toUint32Array(values.data());
    return values;
}

std::vector<uint64_t> toVector(const Roaring64Map& bitmap) {
    std::vector<uint64_t> values(bitmap.cardinality());
    bitmap.toUint64Array(values.data());
    return values;
}

void print(const std::string& name, const Roaring& bitmap) {
    std::cout << name << " (cardinality=" << bitmap.cardinality() << "): [";
    bool first = true;
    for (uint32_t value : bitmap) {
        std::cout << (first ? "" : ", ") << value;
        first = false;
    }
    std::cout << "]\n";
}

void constructionAndMutation() {
    title("构造与增删");

    Roaring empty;
    Roaring from_list = {1, 2, 3};
    const uint32_t data[] = {10, 20, 30};
    Roaring from_array(3, data);
    Roaring from_of = Roaring::bitmapOf(3, 4u, 5u, 6u);
    Roaring from_list2 = Roaring::bitmapOfList({7, 8, 9});

    print("empty", empty);
    print("from_list", from_list);
    print("from_array", from_array);
    print("bitmapOf", from_of);
    print("bitmapOfList", from_list2);

    Roaring bitmap;
    bitmap.add(42);
    std::cout << "addChecked(42) = " << bitmap.addChecked(42)
              << ", addChecked(43) = " << bitmap.addChecked(43) << "\n";

    const uint32_t batch[] = {100, 101, 102};
    bitmap.addMany(3, batch);

    // BulkContext 只对同一个位图有效，期间对该位图做其它修改会让它失效
    BulkContext bulk;
    for (uint32_t value = 200; value < 210; ++value) {
        bitmap.addBulk(bulk, value);
    }

    // addRange/removeRange 左闭右开，addRangeClosed/removeRangeClosed 闭区间
    bitmap.addRange(300, 305);
    bitmap.addRangeClosed(400, 403);
    std::cout << "cardinality = " << bitmap.cardinality() << "\n";

    std::cout << "contains(102) = " << bitmap.contains(102)
              << ", contains(103) = " << bitmap.contains(103)
              << ", containsRange(300, 304) = " << bitmap.containsRange(300, 304) << "\n";
    std::cout << "minimum = " << bitmap.minimum() << ", maximum = " << bitmap.maximum()
              << ", isEmpty = " << bitmap.isEmpty() << ", isFull = " << bitmap.isFull() << "\n";

    bitmap.remove(42);
    std::cout << "removeChecked(42) = " << bitmap.removeChecked(42) << "\n";
    bitmap.removeRange(200, 205);
    bitmap.removeRangeClosed(205, 209);
    std::cout << "after removes, cardinality = " << bitmap.cardinality() << "\n";

    Roaring masked = bitmap;
    masked.mask(100, 400);
    print("mask(100, 400)", masked);

    bitmap.clear();
    std::cout << "after clear, cardinality = " << bitmap.cardinality() << "\n";
}

void queries() {
    title("查询");

    Roaring bitmap;
    bitmap.addRange(0, 1000);
    bitmap.removeRange(0, 100);
    bitmap.addMany(
        9, std::vector<uint32_t>{1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000}.data());

    // rank(x)：<= x 的元素个数
    std::cout << "rank(99) = " << bitmap.rank(99) << ", rank(100) = " << bitmap.rank(100)
              << ", rank(999) = " << bitmap.rank(999) << "\n";

    const uint32_t probes[] = {500, 1000, 5000};
    uint64_t ranks[3] = {0, 0, 0};
    bitmap.rank_many(probes, probes + 3, ranks);
    std::cout << "rank_many{500, 1000, 5000} = {" << ranks[0] << ", " << ranks[1] << ", "
              << ranks[2] << "}\n";

    // select：第 rnk 小的元素，rnk 从 0 开始
    uint32_t value = 0;
    std::cout << "select(0) = " << (bitmap.select(0, &value) ? std::to_string(value) : "n/a")
              << ", select(900) = " << (bitmap.select(900, &value) ? std::to_string(value) : "n/a")
              << ", select(999999) = " << (bitmap.select(999999, &value) ? "ok" : "out of range")
              << "\n";

    // getIndex(x)：x 的下标，不在集合里返回 -1；rank(x) 对不存在的 x 也返回非负值
    std::cout << "getIndex(1000) = " << bitmap.getIndex(1000)
              << ", getIndex(1001) = " << bitmap.getIndex(1001) << "\n";

    std::vector<uint32_t> all = toVector(bitmap);
    std::cout << "toUint32Array: " << all.size() << " values, first = " << all.front()
              << ", last = " << all.back() << "\n";

    std::vector<uint32_t> page(5);
    bitmap.rangeUint32Array(page.data(), 898, 5);
    std::cout << "rangeUint32Array(offset=898, limit=5) = {";
    for (size_t i = 0; i < page.size(); ++i) {
        std::cout << (i == 0 ? "" : ", ") << page[i];
    }
    std::cout << "}\n";

    uint64_t sum = 0;
    for (uint32_t v : bitmap) {
        sum += v;
    }
    size_t count = 0;
    for (auto it = bitmap.begin(), end = bitmap.end(); it != end; ++it) {
        ++count;
    }
    std::cout << "iterate: count = " << count << ", sum = " << sum << "\n";

    // 跳跃定位到 >= v 的第一个元素，没有则返回 false
    auto seek = bitmap.begin();
    if (seek.move_equalorlarger(5000)) {
        std::cout << "move_equalorlarger(5000) -> *it = " << *seek << "\n";
    }
    auto missing = bitmap.begin();
    std::cout << "move_equalorlarger(9500) = " << missing.move_equalorlarger(9500) << "\n";

    BulkContext context;
    bool hit = bitmap.containsBulk(context, 3000) && bitmap.containsBulk(context, 4000);
    std::cout << "containsBulk(3000, 4000) = " << hit << "\n";

    std::cout << "toString() = " << Roaring{1, 2, 3}.toString() << "\n";
}

void setAlgebra() {
    title("集合运算");

    const Roaring a = {1, 2, 3, 4, 5};
    const Roaring b = {4, 5, 6, 7, 8};

    print("a & b", a & b);
    print("a | b", a | b);
    print("a - b", a - b);
    print("a ^ b", a ^ b);

    // *_cardinality 只算基数，不构造中间位图
    std::cout << "and = " << a.and_cardinality(b) << ", or = " << a.or_cardinality(b)
              << ", andnot = " << a.andnot_cardinality(b) << ", xor = " << a.xor_cardinality(b)
              << "\n";

    Roaring acc = a;
    acc |= b;
    print("acc |= b", acc);
    acc &= a;
    print("acc &= a", acc);

    std::cout << "a.intersect(b) = " << a.intersect(b)
              << ", a.isSubset(a | b) = " << a.isSubset(a | b)
              << ", a.isStrictSubset(a | b) = " << a.isStrictSubset(a | b) << "\n";
    std::cout << "jaccard_index(a, b) = " << a.jaccard_index(b) << "\n";

    const Roaring* inputs[] = {&a, &b};
    Roaring united = Roaring::fastunion(2, inputs);
    print("fastunion(a, b)", united);

    Roaring left = {1};
    Roaring right = {2};
    left.swap(right);
    std::cout << "after swap: left = " << left.toString() << ", right = " << right.toString()
              << ", left == right = " << (left == right) << "\n";
}

void compressionAndMemory() {
    title("内存布局与压缩");

    // 容器按数据自适应：稀疏走 array，稠密走 bitset（8KB/65536 个值），连续区间走 run
    Roaring sparse;
    for (uint32_t i = 0; i < 1000; ++i) {
        sparse.add(i * 1000);
    }
    std::cout << "sparse (step 1000): cardinality = " << sparse.cardinality()
              << ", bytes = " << sparse.getSizeInBytes(true) << "\n";

    Roaring dense_even;
    for (uint32_t i = 0; i < 100000; i += 2) {
        dense_even.add(i);
    }
    std::cout << "dense (step 2): cardinality = " << dense_even.cardinality()
              << ", bytes = " << dense_even.getSizeInBytes(true) << "\n";

    Roaring range;
    range.addRange(0, 1000000);
    std::cout << "addRange(0, 1000000): cardinality = " << range.cardinality()
              << ", bytes = " << range.getSizeInBytes(true) << "\n";

    // runOptimize 把 bitset/array 容器换成 run 容器，removeRunCompression 是反向操作
    Roaring compressed;
    for (uint32_t i = 0; i < 100000; ++i) {
        compressed.add(i);
    }
    const size_t before = compressed.getSizeInBytes(true);
    const bool optimized = compressed.runOptimize();
    std::cout << "dense (add one by one): " << before << " bytes, runOptimize() = " << optimized
              << " -> " << compressed.getSizeInBytes(true) << " bytes\n";
    std::cout << "removeRunCompression() = " << compressed.removeRunCompression() << " -> "
              << compressed.getSizeInBytes(true) << " bytes, shrinkToFit() freed "
              << compressed.shrinkToFit() << " bytes\n";

    Roaring bitmap = {1, 2, 3, 1000000};
    std::cout << "portable size = " << bitmap.getSizeInBytes(true)
              << ", native size = " << bitmap.getSizeInBytes(false) << "\n";
}

void serialization() {
    title("序列化");

    Roaring bitmap;
    bitmap.addRange(0, 100000);
    bitmap.runOptimize();

    // portable 格式与 Java/Go/Rust 实现一致，跨语言交换用它
    const size_t portable_size = bitmap.getSizeInBytes(true);
    std::vector<char> portable(portable_size);
    const size_t written = bitmap.write(portable.data(), true);
    Roaring restored = Roaring::read(portable.data(), true);
    std::cout << "portable: " << written << " bytes for " << bitmap.cardinality()
              << " values, round-trip equal = " << (restored == bitmap) << "\n";

    // 数据不可信时用 readSafe：带边界检查
    Roaring safe = Roaring::readSafe(portable.data(), portable.size());
    std::cout << "readSafe: equal = " << (safe == bitmap) << ", serializedSizeInBytesSafe = "
              << Roaring::serializedSizeInBytesSafe(portable.data(), portable.size()) << "\n";

    // native 格式用本机端序和内存布局，读写更快但不跨平台
    std::vector<char> native(bitmap.getSizeInBytes(false));
    bitmap.write(native.data(), false);
    std::cout << "native: " << native.size()
              << " bytes, equal = " << (Roaring::read(native.data(), false) == bitmap) << "\n";

    // frozen 是只读零拷贝视图，可直接落在 mmap 出来的内存上；
    // 缓冲区起始地址必须 32 字节对齐，长度必须精确等于 getFrozenSizeInBytes()
    const size_t frozen_size = bitmap.getFrozenSizeInBytes();
    void* raw = std::aligned_alloc(32, (frozen_size + 31) / 32 * 32);
    if (raw != nullptr) {
        bitmap.writeFrozen(static_cast<char*>(raw));
        {
            Roaring view = Roaring::frozenView(static_cast<const char*>(raw), frozen_size);
            std::cout << "frozen view: " << frozen_size
                      << " bytes, cardinality = " << view.cardinality()
                      << ", contains(99999) = " << view.contains(99999) << "\n";
        }
        std::free(raw);
    }

    Roaring portable_view = Roaring::portableDeserializeFrozen(portable.data());
    std::cout << "portableDeserializeFrozen: cardinality = " << portable_view.cardinality() << "\n";
}

void roaring64() {
    title("64 位位图");

    // Roaring64Map 内部是 map<uint32 高位, Roaring 低位>
    Roaring64Map map_ids;
    map_ids.add(1ULL);
    map_ids.add(1ULL << 32);
    map_ids.add((1ULL << 32) + 42);
    map_ids.addRangeClosed(100ULL, 103ULL);
    map_ids.add(UINT64_MAX);
    std::cout << "Roaring64Map: cardinality = " << map_ids.cardinality()
              << ", contains(2^32 + 42) = " << map_ids.contains((1ULL << 32) + 42)
              << ", minimum = " << map_ids.minimum() << ", maximum = " << map_ids.maximum() << "\n";

    Roaring64Map map_copy = Roaring64Map::bitmapOfList({5ULL, 6ULL});
    std::cout << "Roaring64Map ops: and = " << map_ids.and_cardinality(map_copy)
              << ", rank(1 << 32) = " << map_ids.rank(1ULL << 32)
              << ", getIndex(1) = " << map_ids.getIndex(1ULL) << "\n";

    const std::vector<uint64_t> map_values = toVector(map_ids);
    std::cout << "Roaring64Map toUint64Array: " << map_values.size() << " values, {";
    for (size_t i = 0; i < map_values.size(); ++i) {
        std::cout << (i == 0 ? "" : ", ") << map_values[i];
    }
    std::cout << "}\n";

    std::vector<char> buffer(map_ids.getSizeInBytes(true));
    map_ids.write(buffer.data(), true);
    Roaring64Map map_restored = Roaring64Map::readSafe(buffer.data(), buffer.size());
    std::cout << "Roaring64Map serialize: " << buffer.size()
              << " bytes, equal = " << (map_restored == map_ids) << "\n";

    // Roaring64 是基于 ART 的实现，比 Roaring64Map 更快更省内存，
    // 但两者序列化格式不同，不能互相读取
    Roaring64 art;
    art.add(1ULL);
    art.add((1ULL << 40) + 7);
    art.add(UINT64_MAX);
    art.runOptimize();
    std::cout << "Roaring64: cardinality = " << art.cardinality()
              << ", contains(2^40 + 7) = " << art.contains((1ULL << 40) + 7)
              << ", minimum = " << art.minimum() << ", maximum = " << art.maximum() << "\n";

    std::vector<uint64_t> values(art.cardinality());
    art.toArray(values.data());
    std::cout << "Roaring64 toArray: {";
    for (size_t i = 0; i < values.size(); ++i) {
        std::cout << (i == 0 ? "" : ", ") << values[i];
    }
    std::cout << "}\n";

    std::vector<char> art_buffer(art.getSizeInBytes());
    const size_t art_written = art.write(art_buffer.data());
    std::cout << "Roaring64 serialize: " << art_written << " bytes, cardinality after read = "
              << Roaring64::readSafe(art_buffer.data(), art_buffer.size()).cardinality() << "\n";
}

void invertedIndexAndTargeting() {
    title("倒排索引与人群圈选");

    const std::vector<std::pair<std::string, std::vector<uint32_t>>> docs = {
        {"/blog/roaring-internals", {1, 2, 3, 1000, 100000}},
        {"/blog/roaring-vs-bitset", {2, 3, 7, 1000, 200000}},
        {"/blog/sstable-bloom", {3, 4, 7, 8, 100000, 200000}},
        {"/blog/cpp-concurrency", {1, 4, 9}},
    };

    // 词 -> 文档 id 的倒排链
    std::unordered_map<std::string, Roaring> postings;
    for (size_t doc_id = 0; doc_id < docs.size(); ++doc_id) {
        for (uint32_t term : docs[doc_id].second) {
            postings[std::to_string(term)].add(static_cast<uint32_t>(doc_id));
        }
    }

    // 多词查询就是倒排链求交
    auto search = [&](const std::vector<std::string>& terms) {
        if (terms.empty()) {
            return Roaring{};
        }
        Roaring result = postings.at(terms.front());
        for (size_t i = 1; i < terms.size(); ++i) {
            result &= postings.at(terms[i]);
        }
        return result;
    };

    std::cout << "term 3           -> ";
    for (uint32_t doc_id : search({"3"})) {
        std::cout << docs[doc_id].first << " ";
    }
    std::cout << "\n";

    std::cout << "term 3 AND 7     -> ";
    for (uint32_t doc_id : search({"3", "7"})) {
        std::cout << docs[doc_id].first << " ";
    }
    std::cout << "\n";

    const Roaring cpp_users = {1, 2, 3};
    const Roaring ai_users = {2, 5};
    const Roaring newsletter_opt_out = {2};
    const Roaring target = (cpp_users | ai_users) - newsletter_opt_out;
    std::cout << "target users = {";
    for (size_t i = 0; i < target.cardinality(); ++i) {
        std::cout << (i == 0 ? "" : ", ") << toVector(target)[i];
    }
    std::cout << "}\n";

    std::cout << "posting sizes: term 100000 = " << postings["100000"].getSizeInBytes(true)
              << " bytes, term 200000 = " << postings["200000"].getSizeInBytes(true) << " bytes\n";
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << std::boolalpha;

    constructionAndMutation();
    queries();
    setAlgebra();
    compressionAndMemory();
    serialization();
    roaring64();
    invertedIndexAndTargeting();
    return 0;
}
