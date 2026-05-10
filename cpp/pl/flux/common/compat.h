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
// Created: 2026/05/04 19:55

#pragma once

#if __cplusplus >= 202302L
#include <utility>
#define PL_FLUX_UNREACHABLE() std::unreachable()
#elif defined(__GNUC__) || defined(__clang__)
#define PL_FLUX_UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
#define PL_FLUX_UNREACHABLE() __assume(0)
#else
#include <cstdlib>
#define PL_FLUX_UNREACHABLE() std::abort()
#endif
