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

workspace(name = "playground")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

################ for cpp ################
load("//third_party:repos.bzl", "external_repositories")

external_repositories()

load("@rules_perl//perl:deps.bzl", "perl_register_toolchains", "perl_rules_dependencies")

perl_rules_dependencies()

perl_register_toolchains()

load("@com_github_nelhage_rules_boost//:boost/boost.bzl", "boost_deps")

boost_deps()

load("@rules_ragel//ragel:ragel.bzl", "ragel_register_toolchains")

ragel_register_toolchains("7.0.0.11")

################ for java ################
# load("@contrib_rules_jvm//:repositories.bzl", "contrib_rules_jvm_deps", "contrib_rules_jvm_gazelle_deps")
#
# contrib_rules_jvm_deps()
#
# contrib_rules_jvm_gazelle_deps()
#
# load("@contrib_rules_jvm//:setup.bzl", "contrib_rules_jvm_setup")
#
# contrib_rules_jvm_setup()
#
# load("@contrib_rules_jvm//:gazelle_setup.bzl", "contrib_rules_jvm_gazelle_setup")
#
# contrib_rules_jvm_gazelle_setup()

################ for rust ################
# https://github.com/bazelbuild/rules_rust/releases
# http_archive(
#     name = "rules_rust",
#     sha256 = "36ab8f9facae745c9c9c1b33d225623d976e78f2cc3f729b7973d8c20934ab95",
#     urls = ["https://github.com/bazelbuild/rules_rust/releases/download/0.31.0/rules_rust-v0.31.0.tar.gz"],
# )
#
# load("@rules_rust//rust:repositories.bzl", "rules_rust_dependencies", "rust_register_toolchains")
#
# rules_rust_dependencies()
#
# rust_register_toolchains()
#
# load("@rules_rust//crate_universe:repositories.bzl", "crate_universe_dependencies")
#
# crate_universe_dependencies(bootstrap = True)
#
# load("@rules_rust//crate_universe:defs.bzl", "crates_repository")
#
# crates_repository(
#     name = "crate_index",
#     cargo_lockfile = "//rust:Cargo.lock",
#     manifests = [
#         "//rust:Cargo.toml",
#         "//rust/base:Cargo.toml",
#         "//rust/helloworld:Cargo.toml",
#         "//rust/web:Cargo.toml",
#     ],
#     rust_version = "1.73.0",
# )
#
# load(
#     "@crate_index//:defs.bzl",
#     crate_index = "crate_repositories",
# )
#
# crate_index()
