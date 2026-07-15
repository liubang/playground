#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MINIDFS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$MINIDFS_DIR/../.." && pwd)"
TIMEOUT_SECONDS="${E2E_TIMEOUT_SECONDS:-300}"
BUILDER_IMAGE="${BAZEL_BUILDER_IMAGE:-liubang/bazel-builder:8.7.0}"
COMPOSE=(docker compose --project-directory "$MINIDFS_DIR" -f "$MINIDFS_DIR/docker-compose.yml")
WORK_DIR=""

log() { printf '\n==> %s\n' "$*"; }
fail() { printf '\n[FAIL] %s\n' "$*" >&2; exit 1; }
compose() { "${COMPOSE[@]}" "$@"; }
require_cmd() { command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"; }

on_error() {
    local status=$?
    printf '\n[FAIL] MiniDFS E2E failed (exit %d). Containers and volumes were preserved.\n' "$status" >&2
    compose ps >&2 || true
    printf '[INFO] Inspect logs with: cd %s && docker compose logs --tail=200\n' "$MINIDFS_DIR" >&2
    exit "$status"
}
trap on_error ERR
trap '[[ -z "$WORK_DIR" ]] || rm -rf "$WORK_DIR"' EXIT

check_dependencies() {
    require_cmd docker
    require_cmd bazel
    require_cmd cmp
    require_cmd mktemp
    docker compose version >/dev/null
    docker info >/dev/null
}

service_healthy() {
    local service="$1" container status
    container="$(compose ps -q "$service")"
    [[ -n "$container" ]] || return 1
    status="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$container")"
    [[ "$status" == healthy ]]
}

wait_for() {
    local description="$1"
    shift
    local deadline=$((SECONDS + TIMEOUT_SECONDS))
    log "Waiting for $description"
    until "$@"; do
        ((SECONDS < deadline)) || fail "timed out waiting for $description"
        sleep 2
    done
}

cli() {
    local mounts=()
    if [[ -n "$WORK_DIR" ]]; then
        mounts=(-v "$WORK_DIR:/work")
    fi
    compose run --rm -T "${mounts[@]}" cli --namenode=namenode:9000 "$@"
}

three_datanodes_ready() {
    local output
    output="$(compose exec -T namenode minidfs --namenode=127.0.0.1:9000 datanodes 2>/dev/null)" || return 1
    for node in datanode1 datanode2 datanode3; do
        grep -q "$node" <<<"$output" || return 1
    done
}

build_builder_image() {
    if docker image inspect "$BUILDER_IMAGE" >/dev/null 2>&1; then
        log "Reusing shared Bazel builder image $BUILDER_IMAGE"
        return
    fi
    log "Building shared Bazel builder image $BUILDER_IMAGE"
    docker build \
        --build-arg BAZEL_VERSION=8.7.0 \
        -f "$MINIDFS_DIR/../bazel-builder/Dockerfile" \
        -t "$BUILDER_IMAGE" \
        "$MINIDFS_DIR/../bazel-builder"
}

build_and_test() {
    log "Running all MiniDFS Bazel tests"
    (cd "$REPO_ROOT" && bazel test //cpp/pl/minidfs/... --test_output=errors)
    log "Building the standalone DfsClient API E2E binary"
    (cd "$REPO_ROOT" && bazel build //cpp/pl/minidfs/e2e:dfs_client_e2e)
    build_builder_image
    log "Building MiniDFS Linux runtime image with persistent Bazel caches"
    BUILDER_IMAGE="$BUILDER_IMAGE" compose build namenode
}

start_cluster() {
    "$MINIDFS_DIR/bootstrap.sh" --no-start
    log "Resetting MiniDFS data for a fresh cluster"
    compose down -v --remove-orphans
    log "Starting a fresh MiniDFS cluster"
    compose up -d mysql namenode datanode1 datanode2 datanode3
    local service
    for service in mysql namenode datanode1 datanode2 datanode3; do
        wait_for "$service to become healthy" service_healthy "$service"
    done
    # DataNode health follows registration, but the NameNode admin view can lag briefly.
    wait_for "three registered DataNodes" three_datanodes_ready
}

make_test_file() {
    local path="$1" size="$2"
    dd if=/dev/zero of="$path" bs=1 count="$size" status=none
    if ((size > 0)); then
        printf '\x5a' | dd of="$path" bs=1 seek=$((size - 1)) conv=notrunc status=none
    fi
}

assert_before() {
    local output="$1" first="$2" second="$3" first_line second_line
    first_line="$(grep -n "$first" <<<"$output" | head -1 | cut -d: -f1)"
    second_line="$(grep -n "$second" <<<"$output" | head -1 | cut -d: -f1)"
    [[ -n "$first_line" && -n "$second_line" && "$first_line" -lt "$second_line" ]]
}

run_api_tests() {
    log "Testing the DfsClient API through the standalone cc_binary"
    compose run --rm -T --no-deps --entrypoint minidfs-api-e2e cli \
        --namenode=namenode:9000 \
        --work_dir=/tmp \
        --dfs_root=/api-e2e \
        --block_size=1048576 \
        --replication=3
}

run_tests() {
    WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidfs-e2e.XXXXXX")"
    local block_size=1048576
    local -a size_cases=(
        "empty:0"
        "one-byte:1"
        "one-kib:1024"
        "block-minus-one:$((block_size - 1))"
        "exact-block:$block_size"
        "block-plus-one:$((block_size + 1))"
        "multi-block:$((2 * block_size + 33))"
    )
    local case_spec case_name case_size
    for case_spec in "${size_cases[@]}"; do
        case_name="${case_spec%%:*}"
        case_size="${case_spec##*:}"
        make_test_file "$WORK_DIR/$case_name.bin" "$case_size"
    done
    printf 'appended payload\n' >"$WORK_DIR/append.txt"
    printf 'replacement\n' >"$WORK_DIR/replacement.txt"

    log "Checking cluster status and every admin command"
    local fsinfo datanodes datanode_id datanode_detail
    fsinfo="$(cli fsinfo)"
    grep -Eq 'Live DataNodes[^0-9]*3' <<<"$fsinfo"
    datanodes="$(cli datanodes)"
    for node in datanode1 datanode2 datanode3; do
        grep -q "$node" <<<"$datanodes"
    done
    cli -- datanodes --all | grep -q 'datanode1'
    datanode_id="$(awk '$2 ~ /^datanode[123]$/ {print $1; exit}' <<<"$datanodes")"
    [[ "$datanode_id" =~ ^[0-9]+$ ]]
    datanode_detail="$(cli datanode "$datanode_id")"
    grep -q "DataNode ID" <<<"$datanode_detail"
    grep -Eq 'datanode[123]' <<<"$datanode_detail"

    log "Testing nested namespace, stat, mv, and every ls mode"
    if cli stat /e2e >/dev/null 2>&1; then
        cli -- rm -r /e2e
    fi
    cli mkdir /e2e
    cli mkdir /e2e/level1/level2/level3
    cli stat /e2e/level1/level2/level3 | grep -q 'directory'
    cli mv /e2e/level1/level2/level3 /e2e/level1/level2/data
    cli ls /e2e/level1/level2 | grep -q '/e2e/level1/level2/data'
    cli ls | grep -q '/e2e'
    cli -- ls -d /e2e/level1 | grep -q '/e2e/level1'
    cli -- ls -R /e2e | grep -q '/e2e/level1/level2/data'
    cli -- ls -h /e2e/level1/level2 | grep -q '/e2e/level1/level2/data'
    cli -- ls -t /e2e/level1/level2 >/dev/null
    cli -- ls -r /e2e/level1/level2 >/dev/null
    cli -- ls -hRt /e2e >/dev/null

    log "Testing put/get integrity across file-size boundaries"
    for case_spec in "${size_cases[@]}"; do
        case_name="${case_spec%%:*}"
        case_size="${case_spec##*:}"
        cli --block_size="$block_size" --replication=3 \
            put "/work/$case_name.bin" "/e2e/level1/level2/data/$case_name.bin"
        cli get "/e2e/level1/level2/data/$case_name.bin" "/work/$case_name.download.bin"
        cmp "$WORK_DIR/$case_name.bin" "$WORK_DIR/$case_name.download.bin"
        [[ "$(wc -c <"$WORK_DIR/$case_name.download.bin" | tr -d ' ')" == "$case_size" ]]
    done

    local size_sorted reverse_size_sorted
    size_sorted="$(cli -- ls -S /e2e/level1/level2/data)"
    assert_before "$size_sorted" 'multi-block.bin' 'one-byte.bin'
    reverse_size_sorted="$(cli -- ls -Sr /e2e/level1/level2/data)"
    assert_before "$reverse_size_sorted" 'one-byte.bin' 'multi-block.bin'
    cli -- ls -h /e2e/level1/level2/data | grep -Eq '1\.0K|1\.0M|2\.0M'

    log "Testing inode, blocks, and block diagnostic commands"
    local target_path=/e2e/level1/level2/data/multi-block.bin
    local inode_by_path inode_id inode_by_id blocks_by_path blocks_by_id block_id block_detail
    inode_by_path="$(cli inode "$target_path")"
    inode_id="$(awk '$1 == "Inode" && $2 == "ID" {print $3; exit}' <<<"$inode_by_path")"
    [[ "$inode_id" =~ ^[0-9]+$ ]]
    inode_by_id="$(cli inode "$inode_id")"
    grep -q 'multi-block.bin' <<<"$inode_by_id"
    blocks_by_path="$(cli blocks "$target_path")"
    grep -q 'datanode1:9100' <<<"$blocks_by_path"
    grep -q 'datanode2:9100' <<<"$blocks_by_path"
    grep -q 'datanode3:9100' <<<"$blocks_by_path"
    blocks_by_id="$(cli blocks "$inode_id")"
    grep -q 'Block ID' <<<"$blocks_by_id"
    block_id="$(awk '$1 ~ /^[0-9]+$/ {print $1; exit}' <<<"$blocks_by_path")"
    [[ "$block_id" =~ ^[0-9]+$ ]]
    block_detail="$(cli block "$block_id")"
    grep -q "Block ID" <<<"$block_detail"
    grep -q 'Replicas:' <<<"$block_detail"

    log "Testing append, truncate, mv file, and non-recursive rm"
    cli append /work/append.txt "$target_path"
    cli get "$target_path" /work/appended.bin
    cat "$WORK_DIR/multi-block.bin" "$WORK_DIR/append.txt" >"$WORK_DIR/expected-appended.bin"
    cmp "$WORK_DIR/expected-appended.bin" "$WORK_DIR/appended.bin"
    cli truncate "$block_size" "$target_path"
    cli get "$target_path" /work/truncated.bin
    [[ "$(wc -c <"$WORK_DIR/truncated.bin" | tr -d ' ')" == "$block_size" ]]
    cmp <(dd if="$WORK_DIR/multi-block.bin" bs="$block_size" count=1 status=none) \
        "$WORK_DIR/truncated.bin"
    cli mv /e2e/level1/level2/data/one-kib.bin /e2e/level1/level2/data/moved.bin
    cli stat /e2e/level1/level2/data/moved.bin | grep -q 'file'
    cli rm /e2e/level1/level2/data/moved.bin
    if cli stat /e2e/level1/level2/data/moved.bin >/dev/null 2>&1; then
        fail "non-recursive rm did not remove moved.bin"
    fi

    log "Testing overwrite, setrep, and recursive cleanup"
    cli -- put -f /work/replacement.txt "$target_path"
    cli setrep 2 "$target_path"
    cli stat "$target_path" | grep -Eq 'Replication[[:space:]]+2'
    cli get "$target_path" /work/replacement-download.txt
    cmp "$WORK_DIR/replacement.txt" "$WORK_DIR/replacement-download.txt"
    cli -- rm -r /e2e
    if cli stat /e2e >/dev/null 2>&1; then
        fail "/e2e still exists after recursive removal"
    fi

    log "All MiniDFS CLI and file-size E2E assertions passed"
    run_api_tests
}

usage() {
    echo "Usage: $0 {all|build|start|test|api-test|down|reset}"
}

main() {
    local command="${1:-all}"
    check_dependencies
    case "$command" in
        all)
            build_and_test
            start_cluster
            run_tests
            ;;
        build) build_and_test ;;
        start) start_cluster ;;
        test) run_tests ;;
        api-test) run_api_tests ;;
        down) compose down ;;
        reset) compose down -v --remove-orphans ;;
        *) usage; exit 2 ;;
    esac
}

main "$@"
