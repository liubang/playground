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

#define PL_CONCATENATE_IMPL(s1, s2) s1##s2
#define PL_CONCATENATE(s1, s2)      PL_CONCATENATE_IMPL(s1, s2)

#ifdef __COUNTER__
#define PL_ANONYMOUS_VARIABLE(str) PL_CONCATENATE(str, __COUNTER__)
#else
#define PL_ANONYMOUS_VARIABLE(str) PL_CONCATENATE(str, __LINE__)
#endif
