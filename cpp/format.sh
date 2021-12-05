#!/bin/bash

# Copyright (c) 2021 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Authors: liubang (it.liubang@gmail.com)
# Created: 2021/12/05 15:08

set -x

clang-format -i algo/**/*.h
clang-format -i algo/**/*.cc
clang-format -i algo/**/*.cpp
clang-format -i basecode/**/*.h
clang-format -i basecode/**/*.cc
clang-format -i basecode/**/*.cpp
clang-format -i highkyck/**/*.h
clang-format -i highkyck/**/*.cc
clang-format -i highkyck/**/*.cpp
clang-format -i leetcode/**/*.h
clang-format -i leetcode/**/*.cc
clang-format -i leetcode/**/*.cpp
clang-format -i test/**/*.h
clang-format -i test/**/*.cc
clang-format -i test/**/*.cpp

buildifier  ./**/*BUILD*
