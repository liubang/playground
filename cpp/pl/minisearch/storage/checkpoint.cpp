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

#include "cpp/pl/minisearch/storage/checkpoint.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <json2pb/json_to_pb.h>
#include <json2pb/pb_to_json.h>
#include <sys/stat.h>
#include <vector>

#include "cpp/pl/minisearch/server/codec.h"

namespace pl::minisearch::storage {

namespace {

constexpr int64_t kKeepCheckpoints = 2;

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

bool read_file(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    *out = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return in.good();
}

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool make_dirs(const std::string& path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] == '/' && i > 0) {
            ::mkdir(current.c_str(), 0755);
        }
    }
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool remove_dir_recursive(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        return false;
    }
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string child = path + "/" + name;
        struct stat st;
        if (::stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_dir_recursive(child);
        } else {
            ::unlink(child.c_str());
        }
    }
    ::closedir(dir);
    return ::rmdir(path.c_str()) == 0;
}

void append_doc_frame(std::string* out, const proto::Document& doc) {
    const std::string bytes = doc.SerializeAsString();
    const uint32_t len = static_cast<uint32_t>(bytes.size());
    out->append(reinterpret_cast<const char*>(&len), sizeof(len));
    out->append(bytes);
}

} // namespace

CheckpointStore::CheckpointStore(std::string root) : root_(std::move(root)) {}

bool CheckpointStore::Save(const std::string& collection,
                           const proto::CollectionSpec& spec,
                           const std::string& index_type,
                           const core::Collection& docs,
                           const FaissIndex* index) {
    const std::string dir = root_ + "/" + collection;
    if (!make_dirs(dir)) {
        return false;
    }

    // 读取当前 seq。
    int64_t seq = 0;
    {
        std::string manifest_json;
        proto::CheckpointManifest manifest;
        if (read_file(dir + "/manifest.json", &manifest_json) &&
            json2pb::JsonToProtoMessage(manifest_json, &manifest, nullptr)) {
            seq = manifest.seq();
        }
        ++seq;
    }

    // 文档快照（快照读锁由 ForEachActive 持有，期间写者阻塞）。
    // 按 internal docid 排序写出：帧序确定，恢复侧与 diff/调试更友好。
    std::vector<core::Document> snapshot;
    docs.ForEachActive([&snapshot](const core::Document& doc) { snapshot.push_back(doc); });
    std::sort(
        snapshot.begin(), snapshot.end(), [](const core::Document& a, const core::Document& b) {
            return a.internal_docid < b.internal_docid;
        });
    std::string docs_bytes;
    for (const auto& doc : snapshot) {
        proto::Document msg;
        server::ToProtoDocument(doc, &msg, /*include_internal=*/true);
        append_doc_frame(&docs_bytes, msg);
    }
    const std::string docs_path = dir + "/checkpoint." + std::to_string(seq) + ".docs";
    if (!write_file(docs_path, docs_bytes)) {
        return false;
    }

    // 向量索引。
    std::string faiss_path;
    if (index != nullptr) {
        faiss_path = dir + "/checkpoint." + std::to_string(seq) + ".faiss";
        if (!index->save(faiss_path)) {
            return false;
        }
    }

    // manifest 原子写。
    proto::CheckpointManifest manifest;
    manifest.set_seq(seq);
    *manifest.mutable_spec() = spec;
    manifest.set_index_type(index_type);
    manifest.add_files("checkpoint." + std::to_string(seq) + ".docs");
    if (!faiss_path.empty()) {
        manifest.add_files("checkpoint." + std::to_string(seq) + ".faiss");
    }
    std::string manifest_json;
    json2pb::Pb2JsonOptions options;
    options.always_print_primitive_fields = true;
    json2pb::ProtoMessageToJson(manifest, &manifest_json, options);
    const std::string tmp = dir + "/manifest.json.tmp";
    if (!write_file(tmp, manifest_json) ||
        std::rename(tmp.c_str(), (dir + "/manifest.json").c_str()) != 0) {
        return false;
    }

    // 滚动保留：删除 seq-keep 及更早的数据文件。
    for (int64_t old = 1; old <= seq - kKeepCheckpoints; ++old) {
        ::unlink((dir + "/checkpoint." + std::to_string(old) + ".docs").c_str());
        ::unlink((dir + "/checkpoint." + std::to_string(old) + ".faiss").c_str());
    }
    return true;
}

std::optional<CheckpointStore::Loaded> CheckpointStore::Load(const std::string& collection) const {
    const std::string dir = root_ + "/" + collection;
    std::string manifest_json;
    proto::CheckpointManifest manifest;
    if (!read_file(dir + "/manifest.json", &manifest_json) ||
        !json2pb::JsonToProtoMessage(manifest_json, &manifest, nullptr)) {
        return std::nullopt;
    }

    const std::string seq_str = std::to_string(manifest.seq());
    std::string docs_bytes;
    if (!read_file(dir + "/checkpoint." + seq_str + ".docs", &docs_bytes)) {
        return std::nullopt;
    }

    Loaded loaded;
    loaded.spec = manifest.spec();
    loaded.index_type = manifest.index_type();
    size_t offset = 0;
    while (offset + sizeof(uint32_t) <= docs_bytes.size()) {
        uint32_t len = 0;
        std::memcpy(&len, docs_bytes.data() + offset, sizeof(len));
        offset += sizeof(len);
        if (offset + len > docs_bytes.size()) {
            return std::nullopt; // corrupt frame
        }
        proto::Document msg;
        if (!msg.ParseFromString(docs_bytes.substr(offset, len))) {
            return std::nullopt;
        }
        offset += len;
        core::Document doc;
        std::string error;
        if (!server::ToCoreDocument(msg, &doc, &error)) {
            return std::nullopt;
        }
        loaded.documents.push_back(std::move(doc));
    }

    const std::string faiss_path = dir + "/checkpoint." + seq_str + ".faiss";
    if (file_exists(faiss_path)) {
        loaded.faiss_path = faiss_path;
    }
    return loaded;
}

std::vector<std::string> CheckpointStore::ListCollections() const {
    std::vector<std::string> names;
    DIR* dir = ::opendir(root_.c_str());
    if (dir == nullptr) {
        return names;
    }
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (file_exists(root_ + "/" + name + "/manifest.json")) {
            names.push_back(name);
        }
    }
    ::closedir(dir);
    return names;
}

bool CheckpointStore::Drop(const std::string& collection) {
    return remove_dir_recursive(root_ + "/" + collection);
}

} // namespace pl::minisearch::storage
