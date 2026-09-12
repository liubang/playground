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
# Created: 2026/09/12

"""Force-release wrapper for executables bundled into macOS apps.

The repo builds C++ as debug+ASan by default (the //cpp:asan bool flag
defaults to True; see //cpp:copts/configure_copts.bzl). A helper binary
shipped inside an .app must instead be -c opt without ASan no matter
which config the top-level build uses, otherwise the app's own debug
build would silently produce a dog-slow bundled engine. Applied as an
outgoing edge transition on the wrapped executable target.
"""

def _opt_transition_impl(settings, attr):
    _ = settings  # unused
    _ = attr  # unused
    return {
        "//command_line_option:compilation_mode": "opt",
        "//cpp:asan": False,
    }

_opt_transition = transition(
    implementation = _opt_transition_impl,
    inputs = [],
    outputs = [
        "//command_line_option:compilation_mode",
        "//cpp:asan",
    ],
)

def _opt_binary_impl(ctx):
    files = ctx.files.target
    if len(files) != 1:
        fail("opt_binary: expected exactly one executable output from %s, got %d" %
             (ctx.attr.target.label, len(files)))

    # Re-expose under output_name so bundle contents keep a stable name
    # regardless of the source target's name.
    out = ctx.actions.declare_file(ctx.attr.output_name)
    ctx.actions.symlink(output = out, target_file = files[0])
    return [DefaultInfo(files = depset([out]))]

opt_binary = rule(
    implementation = _opt_binary_impl,
    attrs = {
        "target": attr.label(
            cfg = _opt_transition,
            executable = True,
            mandatory = True,
        ),
        "output_name": attr.string(mandatory = True),
    },
    doc = "Re-exposes an executable built with -c opt and //cpp:asan=False.",
)
