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

#include "cpp/pl/mllm/loader/gguf.h"

#include <cstring>
#include <utility>

namespace pl::mllm {
namespace {

inline constexpr uint32_t kGgufMagic = 0x46554747; // "GGUF" little-endian
inline constexpr uint32_t kDefaultAlignment = 32;
inline constexpr uint64_t kMaxSaneDim = (1ull << 40);

// Bounds-checked little-endian cursor over the mmap.
class Cursor {
public:
    Cursor(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    [[nodiscard]] size_t pos() const noexcept { return pos_; }
    [[nodiscard]] bool seek(size_t pos) noexcept {
        if (pos > size_)
            return false;
        pos_ = pos;
        return true;
    }
    [[nodiscard]] bool read_bytes(void* dst, size_t n) noexcept {
        if (n > size_ - pos_)
            return false;
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    template <typename T> [[nodiscard]] bool read_pod(T& out) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        return read_bytes(&out, sizeof(T));
    }
    [[nodiscard]] bool read_string(std::string& out) {
        uint64_t len = 0;
        if (!read_pod(len) || len > size_ - pos_)
            return false;
        out.assign(reinterpret_cast<const char*>(data_ + pos_), static_cast<size_t>(len));
        pos_ += static_cast<size_t>(len);
        return true;
    }
    [[nodiscard]] const uint8_t* raw() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

size_t align_up(size_t n, size_t a) noexcept {
    return (n + a - 1) / a * a;
}

[[nodiscard]] DType to_dtype(uint32_t type) noexcept {
    switch (type) {
        case 0:
            return DType::kF32;
        case 1:
            return DType::kF16;
        case 2:
            return DType::kQ4_0;
        case 8:
            return DType::kQ8_0;
        default:
            return DType::kF32; // sentinel: caller rejects before this
    }
    return DType::kF32;
}

[[nodiscard]] bool read_scalar(Cursor& c, uint32_t type, MetadataValue& out) {
    switch (type) {
        case 0: {
            uint8_t v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        case 1: {
            int8_t v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        case 2: {
            uint16_t v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        case 3: {
            int16_t v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        case 4: {
            uint32_t v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        case 5: {
            int32_t v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        case 6: {
            float v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        case 7: {
            uint8_t v;
            if (!c.read_pod(v))
                return false;
            out = (v != 0);
            return true;
        }
        case 8: {
            std::string v;
            if (!c.read_string(v))
                return false;
            out = std::move(v);
            return true;
        }
        case 10: {
            uint64_t v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        case 11: {
            int64_t v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        case 12: {
            double v;
            if (!c.read_pod(v))
                return false;
            out = v;
            return true;
        }
        default:
            return false;
    }
}

[[nodiscard]] bool read_array(Cursor& c, MetadataValue& out) {
    uint32_t elem_type = 0;
    uint64_t len = 0;
    if (!c.read_pod(elem_type) || !c.read_pod(len))
        return false;
    if (len > (c.size() - c.pos()))
        return false; // crude overflow guard

    if (elem_type == 8) {
        std::vector<std::string> v;
        v.reserve(static_cast<size_t>(len));
        for (uint64_t i = 0; i < len; ++i) {
            std::string s;
            if (!c.read_string(s))
                return false;
            v.push_back(std::move(s));
        }
        out = std::move(v);
        return true;
    }

    switch (elem_type) {
        case 0: {
            std::vector<uint8_t> v;
            v.reserve(static_cast<size_t>(len));
            uint8_t x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 1: {
            std::vector<int32_t> v;
            v.reserve(static_cast<size_t>(len));
            int8_t x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 3: {
            std::vector<int32_t> v;
            v.reserve(static_cast<size_t>(len));
            int16_t x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 5: {
            std::vector<int32_t> v;
            v.reserve(static_cast<size_t>(len));
            int32_t x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 2: {
            std::vector<uint32_t> v;
            v.reserve(static_cast<size_t>(len));
            uint16_t x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 4: {
            std::vector<uint32_t> v;
            v.reserve(static_cast<size_t>(len));
            uint32_t x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 6: {
            std::vector<float> v;
            v.reserve(static_cast<size_t>(len));
            float x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 12: {
            std::vector<double> v;
            v.reserve(static_cast<size_t>(len));
            double x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 7: {
            std::vector<uint8_t> v;
            v.reserve(static_cast<size_t>(len));
            uint8_t x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 10: {
            std::vector<uint64_t> v;
            v.reserve(static_cast<size_t>(len));
            uint64_t x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        case 11: {
            std::vector<int64_t> v;
            v.reserve(static_cast<size_t>(len));
            int64_t x;
            for (uint64_t i = 0; i < len; ++i) {
                if (!c.read_pod(x))
                    return false;
                v.push_back(x);
            }
            out = std::move(v);
            return true;
        }
        default:
            return false;
    }
}

} // namespace

Result<std::shared_ptr<GGUFFile>> GGUFFile::Open(std::string path) {
    auto mapped = MappedFile::Open(path);
    if (!mapped.ok()) {
        return mapped.status();
    }

    Cursor cursor(mapped.value().bytes().data(), mapped.value().size());
    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t tensor_count = 0;
    uint64_t meta_count = 0;
    if (!cursor.read_pod(magic) || magic != kGgufMagic) {
        return Status::Error(ErrorCode::kInvalidFormat, "bad GGUF magic");
    }
    if (!cursor.read_pod(version) || (version != 3 && version != 2)) {
        return Status::Error(ErrorCode::kInvalidFormat, "unsupported GGUF version");
    }
    if (!cursor.read_pod(tensor_count) || !cursor.read_pod(meta_count)) {
        return Status::Error(ErrorCode::kInvalidFormat, "truncated GGUF header");
    }
    if (tensor_count > kMaxSaneDim || meta_count > kMaxSaneDim) {
        return Status::Error(ErrorCode::kInvalidFormat, "implausible GGUF counts");
    }

    auto file = std::shared_ptr<GGUFFile>(new GGUFFile());
    file->mapped_ = std::move(mapped).value();

    // Metadata.
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(meta_count));
    for (uint64_t i = 0; i < meta_count; ++i) {
        std::string key;
        uint32_t type = 0;
        if (!cursor.read_string(key) || !cursor.read_pod(type)) {
            return Status::Error(ErrorCode::kInvalidFormat, "truncated metadata entry");
        }
        MetadataValue value;
        const bool ok = (type == 9) ? read_array(cursor, value) : read_scalar(cursor, type, value);
        if (!ok) {
            return Status::Error(ErrorCode::kInvalidFormat, "bad metadata value: " + key);
        }
        keys.push_back(std::move(key));
        file->metadata_.emplace(keys.back(), std::move(value));
    }

    const auto arch_it = file->metadata_.find("general.architecture");
    if (arch_it == file->metadata_.end() || !std::holds_alternative<std::string>(arch_it->second)) {
        return Status::Error(ErrorCode::kInvalidFormat, "missing general.architecture");
    }
    file->architecture_ = std::get<std::string>(arch_it->second);

    // Alignment.
    uint32_t alignment = kDefaultAlignment;
    const auto align_it = file->metadata_.find("general.alignment");
    if (align_it != file->metadata_.end()) {
        const auto* a = std::get_if<uint32_t>(&align_it->second);
        if (a != nullptr && *a != 0 && (*a & (*a - 1)) == 0) {
            alignment = *a;
        }
    }

    // Tensor directory.
    for (uint64_t i = 0; i < tensor_count; ++i) {
        std::string name;
        uint32_t n_dims = 0;
        if (!cursor.read_string(name) || !cursor.read_pod(n_dims)) {
            return Status::Error(ErrorCode::kInvalidFormat, "truncated tensor info");
        }
        if (n_dims == 0 || n_dims > static_cast<uint32_t>(Shape::kMaxRank)) {
            return Status::Error(ErrorCode::kInvalidFormat, "unsupported tensor rank: " + name);
        }
        std::array<int64_t, Shape::kMaxRank> ggml_dims{};
        uint64_t prod = 1;
        bool overflow = false;
        for (uint32_t d = 0; d < n_dims; ++d) {
            uint64_t dim = 0;
            if (!cursor.read_pod(dim)) {
                return Status::Error(ErrorCode::kInvalidFormat, "truncated tensor dim");
            }
            if (dim > kMaxSaneDim) {
                return Status::Error(ErrorCode::kInvalidFormat, "implausible tensor dim: " + name);
            }
            ggml_dims[d] = static_cast<int64_t>(dim);
            if (dim != 0 && prod > (UINT64_MAX / dim)) {
                overflow = true;
            }
            prod *= dim;
        }
        uint32_t type = 0;
        uint64_t offset = 0;
        if (!cursor.read_pod(type) || !cursor.read_pod(offset)) {
            return Status::Error(ErrorCode::kInvalidFormat, "truncated tensor tail: " + name);
        }
        if (type != 0 && type != 1 && type != 2 && type != 8) {
            return Status::Error(ErrorCode::kUnsupported,
                                 "unsupported tensor dtype id " + std::to_string(type) + " for " +
                                     name);
        }
        if (overflow) {
            return Status::Error(ErrorCode::kInvalidFormat, "tensor numel overflow: " + name);
        }

        const DType dtype = to_dtype(type);
        const int64_t numel = static_cast<int64_t>(prod);
        const size_t bytes = dtype_nbytes(dtype, numel);
        if (bytes == 0) {
            return Status::Error(ErrorCode::kInvalidFormat, "bad tensor byte size: " + name);
        }

        // Reverse ggml dim order -> row-major shape.
        std::span<const int64_t> src(ggml_dims.data(), static_cast<size_t>(n_dims));
        std::vector<int64_t> rm(src.rbegin(), src.rend());
        Shape shape(rm);

        TensorInfo info;
        info.name = std::move(name);
        info.dtype = dtype;
        info.shape = shape;
        info.byte_size = bytes;
        // Raw offset is relative to the start of the data section; the absolute
        // file offset is resolved once below, after the whole tensor directory
        // has been consumed.
        info.file_offset = static_cast<size_t>(offset);

        file->name_to_index_.emplace(file->tensors_.size() == 0 ? std::string_view{}
                                                                // placeholder; fixed below
                                                                : std::string_view{},
                                     file->tensors_.size());
        (void)file;
        file->tensors_.push_back(std::move(info));
    }

    // GGUF tensor offsets are relative to the data section, which begins after
    // ALL metadata entries and the complete tensor directory, aligned to the
    // file alignment. Compute that base once (not per-tensor inside the loop —
    // the cursor position there is still mid-directory) and resolve offsets.
    const size_t data_base = align_up(cursor.pos(), static_cast<size_t>(alignment));
    for (auto& ti : file->tensors_) {
        if (ti.file_offset > file->mapped_.size() ||
            file->mapped_.size() - ti.file_offset < ti.byte_size) {
            return Status::Error(ErrorCode::kInvalidFormat, "tensor data out of range: " + ti.name);
        }
        ti.file_offset += data_base;
    }

    // name_to_index_ points into tensors_ string storage (stable).
    for (size_t i = 0; i < file->tensors_.size(); ++i) {
        file->name_to_index_[file->tensors_[i].name] = i;
    }

    return file;
}

// Silence unused helper warnings if any.
namespace {
[[maybe_unused]] const void* kUnused = reinterpret_cast<void*>(&read_array);
} // namespace

bool GGUFFile::has_tensor(std::string_view name) const noexcept {
    return name_to_index_.find(name) != name_to_index_.end();
}

Result<TensorInfo> GGUFFile::tensor_info(std::string_view name) const {
    const auto it = name_to_index_.find(name);
    if (it == name_to_index_.end()) {
        return Status::Error(ErrorCode::kNotFound, "tensor not found: " + std::string(name));
    }
    return tensors_[it->second];
}

Result<TensorView> GGUFFile::tensor(std::string_view name) const {
    const auto it = name_to_index_.find(name);
    if (it == name_to_index_.end()) {
        return Status::Error(ErrorCode::kNotFound, "tensor not found: " + std::string(name));
    }
    const auto& info = tensors_[it->second];
    auto* ptr = const_cast<uint8_t*>(mapped_.bytes().data() + info.file_offset);
    return TensorView(ptr, info.dtype, info.shape);
}

const MetadataValue* GGUFFile::metadata(std::string_view key) const noexcept {
    const auto it = metadata_.find(std::string(key));
    return it == metadata_.end() ? nullptr : &it->second;
}

Result<std::string> GGUFFile::string_meta(std::string_view key) const {
    const auto* v = metadata(key);
    if (v == nullptr)
        return Status::Error(ErrorCode::kNotFound, std::string(key));
    if (const auto* s = std::get_if<std::string>(v))
        return *s;
    return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " not a string");
}

Result<uint32_t> GGUFFile::u32_meta(std::string_view key) const {
    const auto* v = metadata(key);
    if (v == nullptr)
        return Status::Error(ErrorCode::kNotFound, std::string(key));
    return std::visit(
        [key](auto&& val) -> Result<uint32_t> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
                          std::is_same_v<T, uint32_t>) {
                return static_cast<uint32_t>(val);
            } else if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> ||
                                 std::is_same_v<T, int32_t>) {
                if (val < 0)
                    return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " negative");
                return static_cast<uint32_t>(val);
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                if (val > UINT32_MAX)
                    return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " overflow");
                return static_cast<uint32_t>(val);
            } else if constexpr (std::is_same_v<T, int64_t>) {
                if (val < 0 || val > INT32_MAX)
                    return Status::Error(ErrorCode::kInvalidFormat,
                                         std::string(key) + " out of range");
                return static_cast<uint32_t>(val);
            } else {
                return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " not u32");
            }
        },
        *v);
}

Result<int32_t> GGUFFile::i32_meta(std::string_view key) const {
    const auto* v = metadata(key);
    if (v == nullptr)
        return Status::Error(ErrorCode::kNotFound, std::string(key));
    return std::visit(
        [key](auto&& val) -> Result<int32_t> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t> ||
                          std::is_same_v<T, uint16_t> || std::is_same_v<T, int16_t> ||
                          std::is_same_v<T, int32_t>) {
                return static_cast<int32_t>(val);
            } else if constexpr (std::is_same_v<T, uint32_t>) {
                if (val > static_cast<uint32_t>(INT32_MAX))
                    return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " overflow");
                return static_cast<int32_t>(val);
            } else {
                return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " not i32");
            }
        },
        *v);
}

Result<float> GGUFFile::f32_meta(std::string_view key) const {
    const auto* v = metadata(key);
    if (v == nullptr)
        return Status::Error(ErrorCode::kNotFound, std::string(key));
    if (const auto* f = std::get_if<float>(v))
        return *f;
    if (const auto* d = std::get_if<double>(v))
        return static_cast<float>(*d);
    return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " not float");
}

Result<bool> GGUFFile::bool_meta(std::string_view key) const {
    const auto* v = metadata(key);
    if (v == nullptr)
        return Status::Error(ErrorCode::kNotFound, std::string(key));
    if (const auto* b = std::get_if<bool>(v))
        return *b;
    if (const auto* u = std::get_if<uint8_t>(v))
        return *u != 0;
    return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " not bool");
}

Result<std::span<const std::string>> GGUFFile::str_array_meta(std::string_view key) const {
    const auto* v = metadata(key);
    if (v == nullptr)
        return Status::Error(ErrorCode::kNotFound, std::string(key));
    if (const auto* a = std::get_if<std::vector<std::string>>(v)) {
        return std::span<const std::string>(*a);
    }
    return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " not string array");
}

Result<std::span<const float>> GGUFFile::f32_array_meta(std::string_view key) const {
    const auto* v = metadata(key);
    if (v == nullptr)
        return Status::Error(ErrorCode::kNotFound, std::string(key));
    if (const auto* a = std::get_if<std::vector<float>>(v)) {
        return std::span<const float>(*a);
    }
    return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " not float array");
}

Result<std::span<const int32_t>> GGUFFile::i32_array_meta(std::string_view key) const {
    const auto* v = metadata(key);
    if (v == nullptr)
        return Status::Error(ErrorCode::kNotFound, std::string(key));
    if (const auto* a = std::get_if<std::vector<int32_t>>(v)) {
        return std::span<const int32_t>(*a);
    }
    return Status::Error(ErrorCode::kInvalidFormat, std::string(key) + " not int32 array");
}

Result<ModelConfig> GGUFFile::model_config() const {
    ModelConfig cfg;
    cfg.architecture = architecture_;

    const std::string p = architecture_ + ".";
    auto get_u32 = [&](std::string_view key) -> Result<uint32_t> {
        return u32_meta(p + std::string(key));
    };

    auto ctx = get_u32("context_length");
    auto emb = get_u32("embedding_length");
    auto ffn = get_u32("feed_forward_length");
    auto blocks = get_u32("block_count");
    auto heads = get_u32("attention.head_count");
    if (!ctx.ok() || !emb.ok() || !ffn.ok() || !blocks.ok() || !heads.ok()) {
        return Status::Error(ErrorCode::kInvalidFormat, "missing model metadata");
    }
    cfg.context_length = static_cast<int32_t>(ctx.value());
    cfg.hidden_size = static_cast<int32_t>(emb.value());
    cfg.intermediate_size = static_cast<int32_t>(ffn.value());
    cfg.num_layers = static_cast<int32_t>(blocks.value());
    cfg.num_attention_heads = static_cast<int32_t>(heads.value());

    auto kv = get_u32("attention.head_count_kv");
    cfg.num_kv_heads = kv.ok() ? static_cast<int32_t>(kv.value()) : cfg.num_attention_heads;

    auto eps = f32_meta(p + "attention.layer_norm_rms_epsilon");
    cfg.rms_norm_eps = eps.ok() ? eps.value() : 1e-5f;

    auto rope = f32_meta(p + "rope.freq_base");
    cfg.rope_freq_base = rope.ok() ? rope.value() : 10000.0f;

    auto head_dim = get_u32("attention.head_dim");
    cfg.head_dim = head_dim.ok() ? static_cast<int32_t>(head_dim.value()) : 0;

    // vocab size from token embedding tensor row count.
    if (has_tensor("token_embd.weight")) {
        const auto ti = tensor_info("token_embd.weight");
        if (ti.ok()) {
            cfg.vocab_size = static_cast<int32_t>(ti.value().shape.dim(0));
        }
    }

    if (auto st = cfg.Validate(); !st.ok()) {
        return st;
    }
    return cfg;
}

} // namespace pl::mllm
