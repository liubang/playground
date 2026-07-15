#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/bootstrap-common.sh
source "$SCRIPT_DIR/../scripts/bootstrap-common.sh"
bootstrap_init "$SCRIPT_DIR" "$@"

bootstrap_check_dependencies
bootstrap_handle_reset

if [ ! -f .env ]; then
    cp .env.example .env
    chmod 600 .env
    log_ok ".env 已从 .env.example 创建"
else
    log_skip ".env 已存在，不覆盖"
fi

docker compose config --quiet
log_ok "Compose 配置校验通过"

BUILDER_IMAGE="${BAZEL_BUILDER_IMAGE:-liubang/bazel-builder:8.7.0}"
if [ "$START_SERVICES" = true ]; then
    if ! docker image inspect "$BUILDER_IMAGE" >/dev/null 2>&1; then
        echo "==> 首次构建通用 Bazel builder 镜像..."
        docker build \
            --build-arg BAZEL_VERSION=8.7.0 \
            -f ../bazel-builder/Dockerfile \
            -t "$BUILDER_IMAGE" \
            ../bazel-builder
    else
        log_skip "复用 builder 镜像: $BUILDER_IMAGE"
    fi
    echo "==> 构建并启动 MiniDFS 集群..."
    BUILDER_IMAGE="$BUILDER_IMAGE" docker compose up -d --build mysql namenode datanode1 datanode2 datanode3
    show_compose_status
    printf '\n==> 常用命令:\n'
    echo "  ./tests/e2e.sh test"
    echo "  docker compose run --rm cli --namenode=namenode:9000 fsinfo"
    echo "  docker compose logs -f namenode datanode1 datanode2 datanode3"
else
    print_no_start
fi
