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

"""Expose Bazel's stable workspace status as an ordinary build artifact.

genrules cannot reference bazel-out/stable-status.txt directly (it is
minted by --workspace_status_command, not by any rule), but a custom
rule CAN: ctx.info_file is that file, and symlinking it into a target's
outputs declares the dependency edge — when the status changes (new
day, new HEAD), downstream genrules re-run.

Consumed by //go/pl/loom/cmd/loom-desktop:info_plist to render the
date+git-hash version into Info.plist, matching what go_binary x_defs
stamp into the binaries (tools/workspace_status.sh is the single
producer of the keys).
"""

def _stable_status_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".txt")
    ctx.actions.symlink(output = out, target_file = ctx.info_file)
    return [DefaultInfo(files = depset([out]))]

stable_status = rule(
    implementation = _stable_status_impl,
    doc = "Symlinks bazel-out/stable-status.txt into a first-class artifact.",
)
