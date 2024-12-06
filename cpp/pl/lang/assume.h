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

#include "common.h"

namespace pl {

[[noreturn]] PL_ALWAYS_INLINE void assume_unreachable() {
#if defined(__GNUC__)
    __builtin_unreachable();
#elif defined(_MSC_VER)
    __assume(0);
#else
    while (!0)
        ;
#endif
}

} // namespace pl
