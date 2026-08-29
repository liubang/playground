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
// Created: 2026/08/29 22:15

#pragma once

// Test-only helper: writes minimal GGUF v3 files into memory so loader tests
// and the end-to-end golden pipeline stay fully hermetic (no checked-in
// binaries, no external tooling at test time).

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace pl::mllm::testdata {

enum class GgufType : uint32_t {
    kF32 = 0,
    kF16 = 1,
    kQ4_0 = 2,
    kQ8_0 = 8,
};

enum class GgufMetaType : uint32_t {
    kU8 = 0,
    kI8 = 1,
    kU16 = 2,
    kI16 = 3,
    kU32 = 4,
    kI32 = 5,
    kF32 = 6,
    kBool = 7,
    kString = 8,
    kArray = 9,
    kU64 = 10,
    kI64 = 11,
    kF64 = 12,
};

struct GgufTensorSpec {
    std::string name;
    std::vector<uint64_t> dims; // ggml order: dims[0] contiguous
    GgufType type;
    std::vector<uint8_t> bytes; // raw, already aligned to the file alignment
};

class GgufWriter {
public:
    explicit GgufWriter(std::string architecture) : architecture_(std::move(architecture)) {}

    void meta(std::string key, GgufMetaType type, std::vector<uint8_t> encoded_value) {
        metas_.push_back({std::move(key), type, std::move(encoded_value)});
    }

    void meta_string(std::string key, std::string value) {
        meta(std::move(key), GgufMetaType::kString, encode_string(value));
    }
    void meta_u32(std::string key, uint32_t value) {
        meta(std::move(key), GgufMetaType::kU32, encode_pod(value));
    }
    void meta_f32(std::string key, float value) {
        meta(std::move(key), GgufMetaType::kF32, encode_pod(value));
    }
    void meta_bool(std::string key, bool value) {
        meta(std::move(key), GgufMetaType::kBool, encode_pod(static_cast<uint8_t>(value ? 1 : 0)));
    }

    void meta_str_array(std::string key, const std::vector<std::string>& values) {
        std::vector<uint8_t> body;
        append_pod(body, static_cast<uint32_t>(GgufMetaType::kString));
        append_pod(body, static_cast<uint64_t>(values.size()));
        for (const auto& v : values) {
            append_pod(body, static_cast<uint64_t>(v.size()));
            append(body, reinterpret_cast<const uint8_t*>(v.data()), v.size());
        }
        meta(std::move(key), GgufMetaType::kArray, std::move(body));
    }

    void meta_f32_array(std::string key, const std::vector<float>& values) {
        std::vector<uint8_t> body;
        append_pod(body, static_cast<uint32_t>(GgufMetaType::kF32));
        append_pod(body, static_cast<uint64_t>(values.size()));
        for (float v : values) {
            append_pod(body, v);
        }
        meta(std::move(key), GgufMetaType::kArray, std::move(body));
    }

    void meta_i32_array(std::string key, const std::vector<int32_t>& values) {
        std::vector<uint8_t> body;
        append_pod(body, static_cast<uint32_t>(GgufMetaType::kI32));
        append_pod(body, static_cast<uint64_t>(values.size()));
        for (int32_t v : values) {
            append_pod(body, v);
        }
        meta(std::move(key), GgufMetaType::kArray, std::move(body));
    }

    void tensor(GgufTensorSpec spec) { tensors_.push_back(std::move(spec)); }

    void write(const std::string& path, uint32_t alignment = 32) const {
        std::ofstream out(path, std::ios::binary);
        write_to(out, alignment);
    }

    std::vector<uint8_t> build(uint32_t alignment = 32) const {
        struct MemBuf : std::streambuf {
            std::vector<uint8_t> buf;
            std::streamsize xsputn(const char* s, std::streamsize n) override {
                buf.insert(buf.end(), s, s + n);
                return n;
            }
            pos_type seekoff(off_type off,
                             std::ios_base::seekdir,
                             std::ios_base::openmode) override {
                return static_cast<pos_type>(buf.size());
            }
        };
        MemBuf buf;
        std::ostream os(&buf);
        write_to(os, alignment);
        return std::move(buf.buf);
    }

private:
    static void append(std::vector<uint8_t>& dst, const uint8_t* src, size_t n) {
        dst.insert(dst.end(), src, src + n);
    }
    static void owrite(std::ostream& out, const uint8_t* src, size_t n) {
        out.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(n));
    }
    template <typename T> static void append_pod(std::vector<uint8_t>& dst, T value) {
        append(dst, reinterpret_cast<const uint8_t*>(&value), sizeof(T));
    }
    template <typename T> static void owrite_pod(std::ostream& out, T value) {
        owrite(out, reinterpret_cast<const uint8_t*>(&value), sizeof(T));
    }
    template <typename T> static std::vector<uint8_t> encode_pod(T value) {
        std::vector<uint8_t> out;
        append_pod(out, value);
        return out;
    }
    static std::vector<uint8_t> encode_string(const std::string& s) {
        std::vector<uint8_t> out;
        append_pod(out, static_cast<uint64_t>(s.size()));
        append(out, reinterpret_cast<const uint8_t*>(s.data()), s.size());
        return out;
    }

    void write_to(std::ostream& out, uint32_t alignment) const {
        const uint64_t total_meta = static_cast<uint64_t>(metas_.size()) + 2; // arch + alignment
        owrite_pod(out, static_cast<uint32_t>(0x46554747));                   // magic
        owrite_pod(out, static_cast<uint32_t>(3));                            // version
        owrite_pod(out, static_cast<uint64_t>(tensors_.size()));
        owrite_pod(out, total_meta);

        // default arch + alignment
        {
            const std::string a = architecture_;
            const std::string arch_key = "general.architecture";
            owrite_pod(out, static_cast<uint64_t>(arch_key.size()));
            owrite(out, reinterpret_cast<const uint8_t*>(arch_key.data()), arch_key.size());
            owrite_pod(out, static_cast<uint32_t>(GgufMetaType::kString));
            owrite_pod(out, static_cast<uint64_t>(a.size()));
            owrite(out, reinterpret_cast<const uint8_t*>(a.data()), a.size());

            const std::string align_key = "general.alignment";
            owrite_pod(out, static_cast<uint64_t>(align_key.size()));
            owrite(out, reinterpret_cast<const uint8_t*>(align_key.data()), align_key.size());
            owrite_pod(out, static_cast<uint32_t>(GgufMetaType::kU32));
            owrite_pod(out, alignment);
        }
        for (const auto& m : metas_) {
            owrite_pod(out, static_cast<uint64_t>(m.key.size()));
            owrite(out, reinterpret_cast<const uint8_t*>(m.key.data()), m.key.size());
            owrite_pod(out, static_cast<uint32_t>(m.type));
            owrite(out, m.value.data(), m.value.size());
        }

        // tensor directory
        uint64_t data_offset = 0;
        for (const auto& t : tensors_) {
            owrite_pod(out, static_cast<uint64_t>(t.name.size()));
            owrite(out, reinterpret_cast<const uint8_t*>(t.name.data()), t.name.size());
            owrite_pod(out, static_cast<uint32_t>(t.dims.size()));
            for (uint64_t d : t.dims) {
                owrite_pod(out, d);
            }
            owrite_pod(out, static_cast<uint32_t>(t.type));
            owrite_pod(out, data_offset); // relative offset, pre-aligned
            data_offset += t.bytes.size();
        }

        // align data section
        const auto pos = static_cast<uint64_t>(out.tellp());
        const uint64_t aligned = (pos + alignment - 1) / alignment * alignment;
        for (uint64_t i = pos; i < aligned; ++i) {
            char z = 0;
            out.write(&z, 1);
        }
        for (const auto& t : tensors_) {
            owrite(out, t.bytes.data(), t.bytes.size());
        }
    }

    struct Meta {
        std::string key;
        GgufMetaType type;
        std::vector<uint8_t> value;
    };

    std::string architecture_;
    std::vector<Meta> metas_;
    std::vector<GgufTensorSpec> tensors_;
};

} // namespace pl::mllm::testdata
