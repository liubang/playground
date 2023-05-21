#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 13:17
# Last Modified: 2023/05/21 13:17
#
#======================================================================

load("@bazel_gazelle//:def.bzl", "DEFAULT_LANGUAGES", "gazelle", "gazelle_binary")

# gazelle:java_maven_install_file maven_install.json
gazelle(
    name = "gazelle",
    gazelle = ":gazelle_bin",
)

gazelle_binary(
    name = "gazelle_bin",
    languages = DEFAULT_LANGUAGES + [
        "@contrib_rules_jvm//java/gazelle",
    ],
)
