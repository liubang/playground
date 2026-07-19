# syntax=docker/dockerfile:1.7
ARG BUILDER_IMAGE=liubang/bazel-builder:8.7.0
FROM ${BUILDER_IMAGE} AS build
WORKDIR /src
COPY . .
RUN --mount=type=cache,id=minivessel-bazel-repository,target=/root/.cache/bazel/repository_cache,sharing=locked \
    --mount=type=cache,id=minivessel-bazel-output,target=/root/.cache/bazel/output,sharing=locked \
    bazel --batch --output_user_root=/root/.cache/bazel/output build --config=release --jobs=2 \
      --local_resources=memory=3072 --repository_cache=/root/.cache/bazel/repository_cache \
      //cpp/pl/minidfs/namenode:namenode //cpp/pl/minidfs/datanode:datanode \
      //cpp/pl/minidfs/client:minidfs //cpp/pl/minivessel/e2e:shared_log_server \
      //cpp/pl/minivessel/e2e:counter_replica //cpp/pl/minivessel/e2e:control \
    && bin="$(bazel --batch --output_user_root=/root/.cache/bazel/output info --config=release bazel-bin)" \
    && install -D -m0755 "$bin/cpp/pl/minidfs/namenode/namenode" /out/minidfs-namenode \
    && install -D -m0755 "$bin/cpp/pl/minidfs/datanode/datanode" /out/minidfs-datanode \
    && install -D -m0755 "$bin/cpp/pl/minidfs/client/minidfs" /out/minidfs \
    && install -D -m0755 "$bin/cpp/pl/minivessel/e2e/shared_log_server" /out/minivessel-sharedlog \
    && install -D -m0755 "$bin/cpp/pl/minivessel/e2e/counter_replica" /out/minivessel-replica \
    && install -D -m0755 "$bin/cpp/pl/minivessel/e2e/control" /out/minivessel-control
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/ /usr/local/bin/
RUN mkdir -p /data/minidfs
EXPOSE 9000 9100 9200 9300
CMD ["minivessel-control", "--help"]
