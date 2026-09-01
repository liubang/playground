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

# End-to-end parity test against llama.cpp on real GGUF checkpoints.
# Tagged "manual": requires llama-cli in PATH and models in /tmp/mllm_models;
# excluded from the default test suite and CI on purpose. Run explicitly:
#   bazel test //cpp/pl/mllm/e2e:e2e_llama_parity --config=release --test_output=all
sh_test(
    name = "e2e_llama_parity",
    srcs = ["parity_vs_llamacpp.sh"],
    args = ["$(rootpath //cpp/pl/mllm/cli:mllm_cli)"],
    data = ["//cpp/pl/mllm/cli:mllm_cli"],
    tags = ["manual"],
)
