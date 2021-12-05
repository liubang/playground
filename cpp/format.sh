#!/bin/bash

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
