# Copyright (c) 2026 The Authors. All rights reserved.
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
# Created: 2026/06/04 12:01

cc_library(
    name = "data_type",
    hdrs = ["data_type.h"],
    visibility = ["//cpp/pl/sstv2:__subpackages__"],
)

cc_library(
    name = "constants",
    hdrs = ["constants.h"],
    visibility = ["//cpp/pl/sstv2:__subpackages__"],
)

cc_library(
    name = "flag",
    hdrs = ["flag.h"],
    visibility = ["//cpp/pl/sstv2:__subpackages__"],
    deps = [":data_type"],
)

cc_library(
    name = "variant",
    srcs = ["variant.cpp"],
    hdrs = ["variant.h"],
    visibility = ["//cpp/pl/sstv2:__subpackages__"],
    deps = [
        ":data_type",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "schema",
    srcs = ["schema.cpp"],
    hdrs = ["schema.h"],
    visibility = ["//cpp/pl/sstv2:__subpackages__"],
    deps = [
        ":data_type",
        ":flag",
    ],
)

cc_library(
    name = "internal_row",
    hdrs = ["internal_row.h"],
    visibility = ["//cpp/pl/sstv2:__subpackages__"],
    deps = [
        ":schema",
        ":variant",
    ],
)
