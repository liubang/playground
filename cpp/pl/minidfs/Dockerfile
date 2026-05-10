# MiniDFS Multi-stage Dockerfile
# Build: docker build --target <namenode|datanode|cli> -t minidfs-<target> .
# Context should be the playground root (4 levels up from this file).

# ============================================================================
# Stage 1: Build environment
# ============================================================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    lld \
    git \
    curl \
    ca-certificates \
    python3 \
    zip \
    unzip \
    autoconf \
    automake \
    libtool \
    nasm \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Install Bazelisk as bazel
RUN curl -fSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
    -o /usr/local/bin/bazel && chmod +x /usr/local/bin/bazel

WORKDIR /src
COPY . .

# Build NameNode and DataNode binaries
RUN bazel build -c opt \
    //cpp/pl/minidfs/master:namenode \
    //cpp/pl/minidfs/client:datanode \
    //cpp/pl/minidfs/client:minidfs

# ============================================================================
# Stage 2: NameNode runtime
# ============================================================================
FROM ubuntu:24.04 AS namenode

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/bazel-bin/cpp/pl/minidfs/master/namenode /usr/local/bin/minidfs-namenode

EXPOSE 8000

ENTRYPOINT ["/usr/local/bin/minidfs-namenode"]

# ============================================================================
# Stage 3: DataNode runtime
# ============================================================================
FROM ubuntu:24.04 AS datanode

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/bazel-bin/cpp/pl/minidfs/client/datanode /usr/local/bin/minidfs-datanode

RUN mkdir -p /data/minidfs

EXPOSE 9000 9001

ENTRYPOINT ["/usr/local/bin/minidfs-datanode"]

# ============================================================================
# Stage 4: CLI tool
# ============================================================================
FROM ubuntu:24.04 AS cli

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/bazel-bin/cpp/pl/minidfs/client/minidfs /usr/local/bin/minidfs

ENTRYPOINT ["/usr/local/bin/minidfs"]
