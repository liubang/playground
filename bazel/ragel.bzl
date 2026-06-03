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

"""Bazel rule for generating source files with Ragel 7."""

def _ragel_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)

    args = ctx.actions.args()
    args.add("-o", out)
    args.add_all(ctx.attr.ragel_options)
    args.add(ctx.file.src)

    ctx.actions.run(
        inputs = [ctx.file.src] + ctx.files.data,
        outputs = [out],
        executable = ctx.executable._ragel_tool,
        arguments = [args],
        mnemonic = "Ragel",
        progress_message = "Ragel: generating %{output}",
    )

    return [DefaultInfo(files = depset([out]))]

ragel = rule(
    implementation = _ragel_impl,
    attrs = {
        "src": attr.label(
            allow_single_file = [".rl"],
            mandatory = True,
            doc = "The Ragel source file (.rl).",
        ),
        "data": attr.label_list(
            allow_files = [".rl"],
            doc = "Additional .rl files included by the source.",
        ),
        "out": attr.string(
            mandatory = True,
            doc = "Output filename (e.g. scanner_generated.cc).",
        ),
        "ragel_options": attr.string_list(
            default = [],
            doc = "Extra command-line options for ragel.",
        ),
        "_ragel_tool": attr.label(
            default = "@ragel//:ragel-c",
            executable = True,
            cfg = "exec",
            doc = "The ragel-c binary (combined frontend + C backend).",
        ),
    },
    doc = "Generate a source file from a Ragel (.rl) input using Ragel 7.",
)
