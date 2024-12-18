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

#pragma once

// noinline
#ifdef _MSC_VER
#define PL_NOINLINE __declspec(noinline)
#elif defined(__HIP_PLATFORM_HCC__)
#define PL_NOINLINE __attribute__((noinline))
#elif defined(__GNUC__)
#define PL_NOINLINE __attribute__((__noinline__))
#else
#define PL_NOINLINE
#endif

// always inline
#ifdef _MSC_VER
#define PL_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__)
#define PL_ALWAYS_INLINE inline __attribute__((__always_inline__))
#else
#define PL_ALWAYS_INLINE inline
#endif

#if defined(__aarch64__)
//  __builtin_prefetch(..., 1) turns into a prefetch into prfm pldl3keep. On
// arm64 we want this as close to the core as possible to turn it into a
// L1 prefetech unless locality == 0 in which case it will be turned into a
// non-temporal prefetch
#define PL_PREFETCH(addr, rw, locality) __builtin_prefetch(addr, rw, locality >= 1 ? 3 : locality)
#else
#define PLPREFETCH(addr, rw, locality) __builtin_prefetch(addr, rw, locality)
#endif
