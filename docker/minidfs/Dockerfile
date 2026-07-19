# syntax=docker/dockerfile:1.7
ARG BUILDER_IMAGE=liubang/bazel-builder:8.7.0
FROM ${BUILDER_IMAGE} AS build

WORKDIR /src
COPY . .
RUN --mount=type=cache,id=minidfs-bazel-repository,target=/root/.cache/bazel/repository_cache,sharing=locked \
    --mount=type=cache,id=minidfs-bazel-output,target=/root/.cache/bazel/output,sharing=locked \
    bazel --batch --output_user_root=/root/.cache/bazel/output build \
        --config=release \
        --jobs=2 \
        --local_resources=memory=2048 \
        --repository_cache=/root/.cache/bazel/repository_cache \
        --distdir=/src/docker/minidfs \
        //cpp/pl/minidfs/namenode:namenode \
        //cpp/pl/minidfs/namenode:format \
        //cpp/pl/minidfs/datanode:datanode \
        //cpp/pl/minidfs/client:minidfs \
        //cpp/pl/minidfs/e2e:dfs_client_e2e \
        //cpp/pl/minivessel:minidfs_e2e \
    && bazel_bin="$(bazel --batch --output_user_root=/root/.cache/bazel/output info --config=release bazel-bin)" \
    && install -D -m 0755 "$bazel_bin/cpp/pl/minidfs/namenode/namenode" /out/minidfs-namenode \
    && install -D -m 0755 "$bazel_bin/cpp/pl/minidfs/namenode/format" /out/minidfs-format \
    && install -D -m 0755 "$bazel_bin/cpp/pl/minidfs/datanode/datanode" /out/minidfs-datanode \
    && install -D -m 0755 "$bazel_bin/cpp/pl/minidfs/client/minidfs" /out/minidfs \
    && install -D -m 0755 "$bazel_bin/cpp/pl/minidfs/e2e/dfs_client_e2e" /out/minidfs-api-e2e \
    && install -D -m 0755 "$bazel_bin/cpp/pl/minivessel/minidfs_e2e" /out/minivessel-minidfs-e2e

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/ /usr/local/bin/
RUN mkdir -p /data/minidfs
EXPOSE 9000 9100
CMD ["minidfs", "--help"]
