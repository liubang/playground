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

#include <benchmark/benchmark.h>
#include <boost/crc.hpp>

#if defined(__linux__)
#include <crc32c/crc32c.h>
#include <isa-l/crc.h>
#include <isa-l/crc64.h>
#endif

class CRC32Benchmark : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State& state) override {
        block_size_ = static_cast<size_t>(state.range(0));
        block_data_ = std::string(block_size_, 'x');
        block_buffer_ = reinterpret_cast<const uint8_t*>(block_data_.data());
    }

protected:
    std::string block_data_;
    const uint8_t* block_buffer_;
    size_t block_size_;
};

BENCHMARK_DEFINE_F(CRC32Benchmark, boost_crc)(benchmark::State& state) {
    uint32_t crc = 0;
    for (auto _ : state) {
        boost::crc_32_type result;
        result.process_bytes(block_buffer_, block_size_);
        crc = result.checksum();
        benchmark::DoNotOptimize(crc);
    }
    state.SetBytesProcessed(state.iterations() * block_size_);
}
BENCHMARK_REGISTER_F(CRC32Benchmark, boost_crc)
    ->RangeMultiplier(16)
    ->Range(256, 16777216); // Block size.

#if defined(__linux__)
BENCHMARK_DEFINE_F(CRC32Benchmark, CRC32C_Public)(benchmark::State& state) {
    uint32_t crc = 0;
    for (auto _ : state)
        crc = crc32c::Extend(crc, block_buffer_, block_size_);
    state.SetBytesProcessed(state.iterations() * block_size_);
}

BENCHMARK_REGISTER_F(CRC32Benchmark, CRC32C_Public)
    ->RangeMultiplier(16)
    ->Range(256, 16777216); // Block size.

#if HAVE_SSE42 && (defined(_M_X64) || defined(__x86_64__))
BENCHMARK_DEFINE_F(CRC32CBenchmark, CRC32C_Sse42)(benchmark::State& state) {
    if (!crc32c::CanUseSse42()) {
        state.SkipWithError("SSE4.2 instructions not available or not enabled");
        return;
    }

    uint32_t crc = 0;
    for (auto _ : state)
        crc = crc32c::ExtendSse42(crc, block_buffer_, block_size_);
    state.SetBytesProcessed(state.iterations() * block_size_);
}

BENCHMARK_REGISTER_F(CRC32CBenchmark, CRC32C_Sse42)
    ->RangeMultiplier(16)
    ->Range(256, 16777216); // Block size.
#endif

BENCHMARK_DEFINE_F(CRC32Benchmark, isal_ieee)(benchmark::State& state) {
    uint32_t crc = 0;
    for (auto _ : state) {
        crc = ::crc32_ieee(0, (unsigned char*)block_data_.data(), block_size_);
        benchmark::DoNotOptimize(crc);
    }
    state.SetBytesProcessed(state.iterations() * block_size_);
}

BENCHMARK_REGISTER_F(CRC32Benchmark, isal_ieee)
    ->RangeMultiplier(16)
    ->Range(256, 16777216); // Block size.

BENCHMARK_DEFINE_F(CRC32Benchmark, isal_iscsi)(benchmark::State& state) {
    uint32_t crc = 0;
    for (auto _ : state) {
        crc = ::crc32_iscsi((unsigned char*)block_data_.data(), block_size_, 0);
        benchmark::DoNotOptimize(crc);
    }
    state.SetBytesProcessed(state.iterations() * block_size_);
}

BENCHMARK_REGISTER_F(CRC32Benchmark, isal_iscsi)
    ->RangeMultiplier(16)
    ->Range(256, 16777216); // Block size.
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////

#define BENCHMARK_CRC64_FUNC(func)                                              \
    BENCHMARK_DEFINE_F(CRC32Benchmark, func)(benchmark::State & state) {        \
        uint64_t crc = 0;                                                       \
        for (auto _ : state) {                                                  \
            crc = ::func(crc, (unsigned char*)block_data_.data(), block_size_); \
            benchmark::DoNotOptimize(crc);                                      \
        }                                                                       \
        state.SetBytesProcessed(state.iterations() * block_size_);              \
    }                                                                           \
    BENCHMARK_REGISTER_F(CRC32Benchmark, func)->RangeMultiplier(16)->Range(256, 16777216)

BENCHMARK_CRC64_FUNC(crc64_ecma_refl);
BENCHMARK_CRC64_FUNC(crc64_ecma_norm);
BENCHMARK_CRC64_FUNC(crc64_iso_refl);
BENCHMARK_CRC64_FUNC(crc64_iso_norm);
BENCHMARK_CRC64_FUNC(crc64_jones_refl);
BENCHMARK_CRC64_FUNC(crc64_jones_norm);
BENCHMARK_CRC64_FUNC(crc64_rocksoft_refl);
BENCHMARK_CRC64_FUNC(crc64_rocksoft_norm);
BENCHMARK_CRC64_FUNC(crc64_ecma_refl_by8);
BENCHMARK_CRC64_FUNC(crc64_ecma_norm_by8);
BENCHMARK_CRC64_FUNC(crc64_ecma_refl_base);
BENCHMARK_CRC64_FUNC(crc64_ecma_norm_base);
BENCHMARK_CRC64_FUNC(crc64_iso_refl_by8);
BENCHMARK_CRC64_FUNC(crc64_iso_norm_by8);
BENCHMARK_CRC64_FUNC(crc64_iso_refl_base);
BENCHMARK_CRC64_FUNC(crc64_iso_norm_base);
BENCHMARK_CRC64_FUNC(crc64_jones_refl_by8);
BENCHMARK_CRC64_FUNC(crc64_jones_norm_by8);
BENCHMARK_CRC64_FUNC(crc64_jones_refl_base);
BENCHMARK_CRC64_FUNC(crc64_jones_norm_base);
BENCHMARK_CRC64_FUNC(crc64_rocksoft_refl_by8);
BENCHMARK_CRC64_FUNC(crc64_rocksoft_refl_base);
BENCHMARK_CRC64_FUNC(crc64_rocksoft_norm_by8);
BENCHMARK_CRC64_FUNC(crc64_rocksoft_norm_base);
