#======================================================================
#
# WORKSPACE -
#
# Created by liubang on 2023/05/21 00:30
# Last Modified: 2023/05/21 00:30
#
#======================================================================

workspace(name = "playground")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

################ for cpp ################
http_archive(
    name = "hedron_compile_commands",
    strip_prefix = "bazel-compile-commands-extractor-3dddf205a1f5cde20faf2444c1757abe0564ff4c",
    url = "https://github.com/hedronvision/bazel-compile-commands-extractor/archive/3dddf205a1f5cde20faf2444c1757abe0564ff4c.tar.gz",
)

load("@hedron_compile_commands//:workspace_setup.bzl", "hedron_compile_commands_setup")

hedron_compile_commands_setup()

load("//third_party:repos.bzl", "external_repositories")

external_repositories()

load("@rules_perl//perl:deps.bzl", "perl_register_toolchains", "perl_rules_dependencies")

perl_rules_dependencies()

perl_register_toolchains()

load("@com_github_nelhage_rules_boost//:boost/boost.bzl", "boost_deps")

boost_deps()

load("@rules_ragel//ragel:ragel.bzl", "ragel_register_toolchains")

ragel_register_toolchains("7.0.0.11")

################ for golang ################
load("@io_bazel_rules_go//go:deps.bzl", "go_register_toolchains", "go_rules_dependencies")
load("@bazel_gazelle//:deps.bzl", "gazelle_dependencies")

# gazelle:repo bazel_gazelle

load("//:go_deps.bzl", "go_repositories")

# gazelle:repository_macro go_deps.bzl%go_repositories
go_repositories()

go_rules_dependencies()

go_register_toolchains(version = "1.19.5")

gazelle_dependencies()

################ for java ################
load("@contrib_rules_jvm//:repositories.bzl", "contrib_rules_jvm_deps", "contrib_rules_jvm_gazelle_deps")

contrib_rules_jvm_deps()

contrib_rules_jvm_gazelle_deps()

load("@contrib_rules_jvm//:setup.bzl", "contrib_rules_jvm_setup")

contrib_rules_jvm_setup()

load("@contrib_rules_jvm//:gazelle_setup.bzl", "contrib_rules_jvm_gazelle_setup")

contrib_rules_jvm_gazelle_setup()
