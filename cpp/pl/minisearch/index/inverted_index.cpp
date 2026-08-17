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
// Created: 2026/08/17

#include "cpp/pl/minisearch/index/inverted_index.h"

#include <fstream>

namespace pl::minisearch::index {

namespace {

void put_u32(std::string* out, uint32_t v) {
    out->append(reinterpret_cast<const char*>(&v), sizeof(v));
}
void put_i64(std::string* out, int64_t v) {
    out->append(reinterpret_cast<const char*>(&v), sizeof(v));
}

struct Cursor {
    const std::string& data;
    size_t off = 0;
    bool take_u32(uint32_t* v) {
        if (off + sizeof(uint32_t) > data.size()) {
            return false;
        }
        std::memcpy(v, data.data() + off, sizeof(*v));
        off += sizeof(uint32_t);
        return true;
    }
    bool take_i64(int64_t* v) {
        if (off + sizeof(int64_t) > data.size()) {
            return false;
        }
        std::memcpy(v, data.data() + off, sizeof(*v));
        off += sizeof(int64_t);
        return true;
    }
    bool take_string(std::string* v, uint32_t len) {
        if (off + len > data.size()) {
            return false;
        }
        v->assign(data.data() + off, len);
        off += len;
        return true;
    }
};

} // namespace

void InvertedIndex::Add(int64_t docid, const std::vector<analysis::Token>& tokens) {
    std::unordered_map<std::string, uint32_t> term_freq;
    for (const auto& token : tokens) {
        ++term_freq[token.term];
    }
    std::unique_lock<std::shared_mutex> lock(mu_);
    for (const auto& [term, tf] : term_freq) {
        postings_[term].push_back({docid, tf});
    }
    doc_lengths_[docid] = static_cast<uint32_t>(tokens.size());
    total_length_ += static_cast<int64_t>(tokens.size());
}

std::vector<InvertedIndex::Posting> InvertedIndex::Find(const std::string& term) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    auto it = postings_.find(term);
    return it == postings_.end() ? std::vector<Posting>() : it->second;
}

int64_t InvertedIndex::DocCount() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return static_cast<int64_t>(doc_lengths_.size());
}

uint32_t InvertedIndex::DocLength(int64_t docid) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    auto it = doc_lengths_.find(docid);
    return it == doc_lengths_.end() ? 0 : it->second;
}

double InvertedIndex::AvgDocLength() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (doc_lengths_.empty()) {
        return 0.0;
    }
    return static_cast<double>(total_length_) / static_cast<double>(doc_lengths_.size());
}

bool InvertedIndex::Save(const std::string& path) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::string out;
    put_i64(&out, total_length_);
    put_u32(&out, static_cast<uint32_t>(doc_lengths_.size()));
    for (const auto& [docid, len] : doc_lengths_) {
        put_i64(&out, docid);
        put_u32(&out, len);
    }
    put_u32(&out, static_cast<uint32_t>(postings_.size()));
    for (const auto& [term, list] : postings_) {
        put_u32(&out, static_cast<uint32_t>(term.size()));
        out += term;
        put_u32(&out, static_cast<uint32_t>(list.size()));
        for (const auto& posting : list) {
            put_i64(&out, posting.docid);
            put_u32(&out, posting.tf);
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return file.good();
}

bool InvertedIndex::Load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    const std::string data((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    Cursor cur{data};
    std::unique_lock<std::shared_mutex> lock(mu_);
    postings_.clear();
    doc_lengths_.clear();
    total_length_ = 0;
    if (!cur.take_i64(&total_length_)) {
        return false;
    }
    uint32_t lengths_count = 0;
    if (!cur.take_u32(&lengths_count)) {
        return false;
    }
    for (uint32_t i = 0; i < lengths_count; ++i) {
        int64_t docid = 0;
        uint32_t len = 0;
        if (!cur.take_i64(&docid) || !cur.take_u32(&len)) {
            return false;
        }
        doc_lengths_[docid] = len;
    }
    uint32_t postings_count = 0;
    if (!cur.take_u32(&postings_count)) {
        return false;
    }
    for (uint32_t i = 0; i < postings_count; ++i) {
        uint32_t term_len = 0;
        if (!cur.take_u32(&term_len)) {
            return false;
        }
        std::string term;
        if (!cur.take_string(&term, term_len)) {
            return false;
        }
        uint32_t list_len = 0;
        if (!cur.take_u32(&list_len)) {
            return false;
        }
        std::vector<Posting>& list = postings_[term];
        list.reserve(list_len);
        for (uint32_t j = 0; j < list_len; ++j) {
            int64_t docid = 0;
            uint32_t tf = 0;
            if (!cur.take_i64(&docid) || !cur.take_u32(&tf)) {
                return false;
            }
            list.push_back({docid, tf});
        }
    }
    return true;
}

} // namespace pl::minisearch::index
