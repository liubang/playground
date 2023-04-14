# Copyright (c) 2020 The Authors. All rights reserved.
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
# Created: 2020/07/20 00:32

#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/04/14 13:40
# Last Modified: 2023/04/14 13:40
#
#======================================================================
package(
    default_visibility = ["//visibility:public"],
    features = ["fully_static_link"],
)

load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

refresh_compile_commands(
    name = "refresh_compile_commands",
    targets = {
        "//highkyck/misk/...": "",
        "//highkyck/utils/...": "",
        "//highkyck/meta/...": "",
        "//highkyck/trace/...": "",
        "//highkyck/brain_fuck/...": "",
        "//test/cpp14/...": "",
        "//test/cpp17/...": "",
        "//test/cpp20/...": "",
        "//test/meta/...": "",
        "//test/misk/...": "",
    },
)
