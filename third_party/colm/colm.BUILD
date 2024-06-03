# Copyright (c) 2024 The Authors. All rights reserved.
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

cc_library(
    name = "aapl",
    hdrs = glob(["src/aapl/*.h"]),
    includes = ["src/aapl"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "config",
    hdrs = ["src/config.h"],
    includes = ["src"],
)

cc_library(
    name = "libfsm",
    srcs = [
        "src/libfsm/actexp.cc",
        "src/libfsm/actexp.h",
        "src/libfsm/actloop.cc",
        "src/libfsm/actloop.h",
        "src/libfsm/allocgen.cc",
        "src/libfsm/asm.cc",
        "src/libfsm/asm.h",
        "src/libfsm/binary.cc",
        "src/libfsm/binary.h",
        "src/libfsm/binbreak.cc",
        "src/libfsm/binbreak.h",
        "src/libfsm/bingoto.cc",
        "src/libfsm/bingoto.h",
        "src/libfsm/binvar.cc",
        "src/libfsm/binvar.h",
        "src/libfsm/codegen.cc",
        "src/libfsm/codegen.h",
        "src/libfsm/common.cc",
        "src/libfsm/dot.cc",
        "src/libfsm/flat.cc",
        "src/libfsm/flat.h",
        "src/libfsm/flatbreak.cc",
        "src/libfsm/flatbreak.h",
        "src/libfsm/flatgoto.cc",
        "src/libfsm/flatgoto.h",
        "src/libfsm/flatvar.cc",
        "src/libfsm/flatvar.h",
        "src/libfsm/fsmap.cc",
        "src/libfsm/fsmattach.cc",
        "src/libfsm/fsmbase.cc",
        "src/libfsm/fsmcond.cc",
        "src/libfsm/fsmgraph.cc",
        "src/libfsm/fsmmin.cc",
        "src/libfsm/fsmnfa.cc",
        "src/libfsm/fsmstate.cc",
        "src/libfsm/gendata.cc",
        "src/libfsm/goto.cc",
        "src/libfsm/goto.h",
        "src/libfsm/gotoexp.cc",
        "src/libfsm/gotoexp.h",
        "src/libfsm/gotoloop.cc",
        "src/libfsm/gotoloop.h",
        "src/libfsm/idbase.cc",
        "src/libfsm/idbase.h",
        "src/libfsm/ipgoto.cc",
        "src/libfsm/ipgoto.h",
        "src/libfsm/parsedata.h",
        "src/libfsm/redfsm.cc",
        "src/libfsm/switch.cc",
        "src/libfsm/switch.h",
        "src/libfsm/switchbreak.cc",
        "src/libfsm/switchbreak.h",
        "src/libfsm/switchgoto.cc",
        "src/libfsm/switchgoto.h",
        "src/libfsm/switchvar.cc",
        "src/libfsm/switchvar.h",
        "src/libfsm/tabbreak.cc",
        "src/libfsm/tabgoto.cc",
        "src/libfsm/tables.cc",
        "src/libfsm/tables.h",
        "src/libfsm/tabvar.cc",
    ],
    hdrs = [
        "src/libfsm/action.h",
        "src/libfsm/asm.h",
        "src/libfsm/common.h",
        "src/libfsm/dot.h",
        "src/libfsm/fsmgraph.h",
        "src/libfsm/gendata.h",
        "src/libfsm/ragel.h",
        "src/libfsm/redfsm.h",
    ],
    includes = ["src/libfsm"],
    visibility = ["//visibility:public"],
    deps = [
        ":aapl",
        ":config",
    ],
)

cc_library(
    name = "libcolm",
)

cc_library(
    name = "libprog",
)

cc_binary(
    name = "colm",
    srcs = [
    ],
)
