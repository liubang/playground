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

#include "cpp/pl/minisearch/auth/console_auth.h"

#include <butil/logging.h>
#include <butil/third_party/rapidjson/document.h>
#include <butil/third_party/rapidjson/stringbuffer.h>
#include <butil/third_party/rapidjson/writer.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace rj = BUTIL_RAPIDJSON_NAMESPACE;

namespace pl::minisearch::auth {

namespace {

std::string to_hex(const unsigned char* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string random_token(const char* prefix) {
    static const char kBase62[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned char raw[32];
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        return {};
    }
    std::string out = prefix;
    out.reserve(out.size() + 43);
    for (size_t i = 0; i < sizeof(raw); ++i) {
        out.push_back(kBase62[raw[i] % 62]);
    }
    return out;
}

std::string random_salt() {
    unsigned char raw[16];
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        return {};
    }
    return to_hex(raw, sizeof(raw));
}

int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

} // namespace

std::string ConsoleAuth::HashPassword(const std::string& salt, const std::string& password) {
    const std::string input = salt + ":" + password;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    return to_hex(digest, sizeof(digest));
}

ConsoleAuth::ConsoleAuth(Options opts) : opts_(std::move(opts)) {
    if (!opts_.path.empty()) {
        std::ifstream in(opts_.path);
        if (in.is_open()) {
            const std::string content((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            rj::Document doc;
            if (!doc.Parse(content.c_str()).HasParseError() && doc.IsObject() &&
                doc.HasMember("users") && doc["users"].IsArray()) {
                const rj::Value& users = doc["users"];
                for (rj::SizeType i = 0; i < users.Size(); ++i) {
                    const rj::Value& item = users[i];
                    if (!item.IsObject() || !item.HasMember("user") || !item.HasMember("salt") ||
                        !item.HasMember("hash")) {
                        continue;
                    }
                    UserEntry entry;
                    entry.salt = item["salt"].GetString();
                    entry.hash = item["hash"].GetString();
                    users_[item["user"].GetString()] = std::move(entry);
                }
            }
        }
    }
    // bootstrap：store 为空且配置了初始账号时创建超级管理员。
    if (users_.empty() && !opts_.bootstrap_user.empty() && !opts_.bootstrap_password.empty()) {
        UserEntry entry;
        entry.salt = random_salt();
        entry.hash = HashPassword(entry.salt, opts_.bootstrap_password);
        users_[opts_.bootstrap_user] = std::move(entry);
        PersistLocked();
        LOG(WARNING) << "console user '" << opts_.bootstrap_user
                     << "' bootstrapped from config; change the password via "
                        "POST /api/v2/auth/password";
    }
}

std::optional<std::string> ConsoleAuth::Login(const std::string& user,
                                              const std::string& password) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = users_.find(user);
    if (it == users_.end() || HashPassword(it->second.salt, password) != it->second.hash) {
        return std::nullopt;
    }
    // 顺手清扫过期 session，避免 map 无界增长
    const int64_t now = now_seconds();
    for (auto sit = sessions_.begin(); sit != sessions_.end();) {
        if (sit->second.expires_at <= now) {
            sit = sessions_.erase(sit);
        } else {
            ++sit;
        }
    }
    std::string token = random_token("mss_");
    if (token.empty()) {
        return std::nullopt;
    }
    sessions_[token] = Session{user, now + opts_.token_ttl_seconds};
    return token;
}

std::optional<Principal> ConsoleAuth::Authenticate(const std::string& token) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = sessions_.find(token);
    if (it == sessions_.end() || it->second.expires_at <= now_seconds()) {
        return std::nullopt;
    }
    Principal principal;
    principal.tenant = "default";
    principal.role = Role::kAdmin;
    principal.key_id = "session:" + it->second.user;
    return principal;
}

void ConsoleAuth::Logout(const std::string& token) {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_.erase(token);
}

bool ConsoleAuth::ChangePassword(const std::string& user,
                                 const std::string& old_password,
                                 const std::string& new_password) {
    if (new_password.size() < 6) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = users_.find(user);
    if (it == users_.end() || HashPassword(it->second.salt, old_password) != it->second.hash) {
        return false;
    }
    UserEntry entry;
    entry.salt = random_salt();
    entry.hash = HashPassword(entry.salt, new_password);
    it->second = std::move(entry);
    // 该用户的全部 session 失效
    for (auto sit = sessions_.begin(); sit != sessions_.end();) {
        if (sit->second.user == user) {
            sit = sessions_.erase(sit);
        } else {
            ++sit;
        }
    }
    PersistLocked();
    return true;
}

void ConsoleAuth::PersistLocked() {
    if (opts_.path.empty()) {
        return;
    }
    rj::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    rj::Value users(rj::kArrayType);
    for (const auto& [name, entry] : users_) {
        rj::Value item(rj::kObjectType);
        item.AddMember("user", rj::Value(name.c_str(), alloc), alloc);
        item.AddMember("salt", rj::Value(entry.salt.c_str(), alloc), alloc);
        item.AddMember("hash", rj::Value(entry.hash.c_str(), alloc), alloc);
        users.PushBack(item, alloc);
    }
    doc.AddMember("users", users, alloc);
    rj::StringBuffer buf;
    rj::Writer<rj::StringBuffer> writer(buf);
    doc.Accept(writer);

    const std::string tmp = opts_.path + ".tmp";
    if (!write_file(tmp, buf.GetString()) || std::rename(tmp.c_str(), opts_.path.c_str()) != 0) {
        LOG(WARNING) << "failed to persist console users to " << opts_.path;
    }
}

} // namespace pl::minisearch::auth
