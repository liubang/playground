# syntax=docker/dockerfile:1.7
FROM ubuntu:24.04

ARG TARGETARCH
ARG BAZEL_VERSION=8.7.0
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf \
    automake \
    build-essential \
    ca-certificates \
    clang \
    curl \
    git \
    libtool \
    lld \
    nasm \
    pkg-config \
    python3 \
    unzip \
    zip \
    && rm -rf /var/lib/apt/lists/*

RUN case "${TARGETARCH}" in \
        amd64) bazel_arch="x86_64" ;; \
        arm64) bazel_arch="arm64" ;; \
        *) echo "Unsupported TARGETARCH: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && curl --fail --location --retry 5 --connect-timeout 15 --max-time 1200 \
        "https://releases.bazel.build/${BAZEL_VERSION}/release/bazel-${BAZEL_VERSION}-linux-${bazel_arch}" \
        -o /usr/local/bin/bazel \
    && chmod +x /usr/local/bin/bazel \
    && bazel --version

WORKDIR /workspace
