# vim: ft=bzl

licenses(["notice"])  # ISC License

genrule(
    name = "gen_sh",
    outs = [
        "gen.sh",
    ],
    cmd = """
cat > $@ <<"EOF"
#! /bin/sh
sed -e 's/@VERSION@/1.0.15/g' \
    -e 's/@SODIUM_LIBRARY_VERSION_MAJOR@/10/g' \
    -e 's/@SODIUM_LIBRARY_VERSION_MINOR@/0/g' \
    -e 's/@SODIUM_LIBRARY_MINIMAL_DEF@//g' 
EOF""",
)

genrule(
    name = "version_h",
    srcs = ["src/libsodium/include/sodium/version.h.in"],
    outs = ["src/libsodium/include/sodium/version.h"],
    cmd = "$(location :gen_sh) < $(<) > $(@)",
    tools = [":gen_sh"],
)

cc_library(
    name = "libsodium",
    srcs = glob(
        [
            "src/**/*.c",
            "src/**/*.h",
        ],
        exclude = ["src/libsodium/crypto_pwhash/argon2/argon2.c"],
    ),
    hdrs = [":version_h"] + glob(["src/**/*.h"]),
    copts = ["-DHAVE_PTHREAD", "-w"],
    includes = [
        "src/libsodium/crypto_core/curve25519/ref10",
        "src/libsodium/crypto_generichash/blake2b/ref",
        "src/libsodium/crypto_onetimeauth/poly1305",
        "src/libsodium/crypto_onetimeauth/poly1305/donna",
        "src/libsodium/crypto_onetimeauth/poly1305/sse2",
        "src/libsodium/crypto_pwhash/argon2",
        "src/libsodium/crypto_pwhash/scryptsalsa208sha256",
        "src/libsodium/crypto_scalarmult/curve25519",
        "src/libsodium/crypto_scalarmult/curve25519/donna_c64",
        "src/libsodium/crypto_scalarmult/curve25519/ref10",
        "src/libsodium/crypto_scalarmult/curve25519/sandy2x",
        "src/libsodium/crypto_shorthash/siphash24/ref",
        "src/libsodium/crypto_sign/ed25519/ref10",
        "src/libsodium/crypto_stream/chacha20",
        "src/libsodium/crypto_stream/chacha20/dolbeau",
        "src/libsodium/crypto_stream/chacha20/ref",
        "src/libsodium/crypto_stream/salsa20",
        "src/libsodium/crypto_stream/salsa20/ref",
        "src/libsodium/crypto_stream/salsa20/xmm6",
        "src/libsodium/crypto_stream/salsa20/xmm6int",
        "src/libsodium/include",
        "src/libsodium/include/sodium",
        "src/libsodium/include/sodium/private",
    ],
    linkopts = [
        "-pthread",
    ],
    deps = [
        "@argon2//:argon2",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
