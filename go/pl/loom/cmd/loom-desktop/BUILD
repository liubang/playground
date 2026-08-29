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
# Created: 2026/08/08

# Loom Desktop (docs/DESKTOP_DESIGN.md §6): Wails-based macOS desktop app.
# The whole package is tagged `manual`: it pulls in CGO + system webview
# frameworks, so it stays out of the default //go/... build matrix and gets
# its own macOS-only CI job.

load("@rules_go//go:def.bzl", "go_binary", "go_library", "go_test")
load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_files", "strip_prefix")
load("@rules_pkg//pkg:zip.bzl", "pkg_zip")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("//tools:version_status.bzl", "stable_status")

go_library(
    name = "loom_desktop_lib",
    srcs = [
        "dialogs_darwin.go",
        "frameworks_darwin.go",
        "main.go",
        "windowbg_darwin.go",
        "windowbg_darwin.m",
        "windowstate.go",
    ],
    cgo = True,
    # Upstream gap: wails v2.13's darwin frontend references UTType without
    # declaring UniformTypeIdentifiers in its cgo LDFLAGS. rules_go collects
    # link flags from rule clinkopts (gazelle normally backfills them from
    # #cgo directives); the in-source directive alone is not propagated, so
    # declare the framework here as well (frameworks_darwin.go covers plain
    # `go build`).
    clinkopts = select({
        "@platforms//os:macos": [
            "-framework",
            "UniformTypeIdentifiers",
            "-framework",
            "WebKit",
        ],
        "//conditions:default": [],
    }),
    importpath = "github.com/liubang/playground/go/pl/loom/cmd/loom-desktop",
    # `manual` on the library too, not just the binary/test: wildcard builds
    # like `bazel coverage //go/...` still build non-test targets matched by
    # the pattern, and this one drags in wails' CGO webview (gtk on Linux),
    # which breaks the Linux CI coverage job before any test can run.
    tags = ["manual"],
    visibility = ["//visibility:private"],
    deps = [
        "//go/pl/loom/internal/app",
        "//go/pl/loom/internal/config:config_lib",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/logging",
        "//go/pl/loom/internal/runtimeevent",
        "//go/pl/loom/internal/server",
        "//go/pl/loom/internal/session:session_lib",
        "//go/pl/loom/internal/version",
        "@com_github_wailsapp_wails_v2//:wails",
        "@com_github_wailsapp_wails_v2//pkg/options",
        "@com_github_wailsapp_wails_v2//pkg/options/assetserver",
        "@com_github_wailsapp_wails_v2//pkg/options/mac",
        "@com_github_wailsapp_wails_v2//pkg/runtime",
    ],
)

go_binary(
    name = "loom-desktop",
    embed = [":loom_desktop_lib"],
    # NOTE: building requires --config=desktop (see .bazelrc): wails v2 needs
    # the `production` build tag, which is applied via the global rules_go
    # tags setting + a gazelle_override in MODULE.bazel (per-target gotags
    # does not reach external repo compiles).
    tags = ["manual"],
    visibility = ["//visibility:public"],
    # Version stamping: same {STABLE_LOOM_VERSION} as cmd/loom (substituted
    # under --stamp; the desktop config sets it).
    x_defs = {
        "github.com/liubang/playground/go/pl/loom/internal/version.Version": "{STABLE_LOOM_VERSION}",
    },
)

go_test(
    name = "loom_desktop_test",
    srcs = ["main_test.go"],
    embed = [":loom_desktop_lib"],
    tags = ["manual"],
    deps = [
        "//go/pl/loom/internal/config:config_lib",
        "//go/pl/loom/internal/runtimeevent",
    ],
)

# --- .app bundle (docs/DESKTOP_DESIGN.md §6.2) ---

# go_binary's declared output is loom-desktop_/loom-desktop; renames flattens
# it to the bundle's executable name.
pkg_files(
    name = "app_binary",
    srcs = [":loom-desktop"],
    attributes = pkg_attributes(mode = "0755"),
    prefix = "Contents/MacOS",
    renames = {":loom-desktop": "loom-desktop"},
    tags = ["manual"],
)

# Renders Info.plist from the template, injecting @VERSION@/@RELEASE@ from
# the STABLE workspace status keys — the same keys the go_binary x_defs
# stamp into the binaries (tools/workspace_status.sh) — so bundle metadata
# can never drift from the Go binaries.
stable_status(name = "loom_stable_status")

genrule(
    name = "info_plist",
    srcs = [
        "macos/Info.plist.tmpl",
        ":loom_stable_status",
    ],
    outs = ["Info.plist"],
    cmd = """
VERSION=$$(awk '$$1 == "STABLE_LOOM_VERSION" {print $$2}' $(location :loom_stable_status)); \\
RELEASE=$$(awk '$$1 == "STABLE_LOOM_DATE" {print $$2}' $(location :loom_stable_status)); \\
[ -n "$$VERSION" ] || { echo "STABLE_LOOM_VERSION missing from workspace status" >&2; exit 1; }; \\
sed -e "s/@VERSION@/$$VERSION/g" -e "s/@RELEASE@/$$RELEASE/g" $(location macos/Info.plist.tmpl) > $@
""",
    tags = ["manual"],
)

pkg_files(
    name = "app_metadata",
    srcs = [":info_plist"],
    prefix = "Contents",
    renames = {":info_plist": "Info.plist"},
    tags = ["manual"],
)

# Bundle icon (referenced by CFBundleIconFile in Info.plist). Regenerate
# with macos/generate_icon.sh when the artwork changes.
pkg_files(
    name = "app_resources",
    srcs = ["macos/AppIcon.icns"],
    prefix = "Contents/Resources",
    strip_prefix = strip_prefix.from_pkg("macos"),
    tags = ["manual"],
)

# Unzipping the output yields Loom.app directly; codesign it afterwards
# (bazel run :package_app), which auto-discovers/creates a stable local
# identity for firewall/TCC grants across rebuilds.
pkg_zip(
    name = "loom_desktop_app",
    srcs = [
        ":app_binary",
        ":app_metadata",
        ":app_resources",
    ],
    package_dir = "Loom.app",
    tags = ["manual"],
)

# Unpacks the bundle zip into dist/Loom.app and codesigns it with the
# auto-discovered local identity (ad-hoc as last resort).
# Usage: bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:package_app
sh_binary(
    name = "package_app",
    srcs = ["package_app.sh"],
    data = [
        ":loom_desktop_app",
        ":make-signing-cert",
    ],
    tags = ["manual"],
)

# Generates the local self-signed codesigning identity used by package_app
# (idempotent; prompts for the keychain password the first time).
# Usage: bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:make-signing-cert
sh_binary(
    name = "make-signing-cert",
    srcs = ["make-signing-cert.sh"],
    tags = ["manual"],
)

# Release artifacts in dist/: per-arch macOS DMGs (arm64 + x86_64) and
# Linux CLI .debs (amd64 + arm64).
# Usage: bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:package_release
sh_binary(
    name = "package_release",
    srcs = ["package_release.sh"],
    data = [":loom_desktop_app"],
    tags = ["manual"],
)
