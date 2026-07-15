#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIGDATA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$BIGDATA_DIR/../.." && pwd)"
JAVA_DIR="$REPO_ROOT/java"
SPARK_MODULE="$JAVA_DIR/pl/bigdata/spark-extensions"
TRINO_MODULE="$JAVA_DIR/pl/bigdata/trino-extensions"
SPARK_DEST="$BIGDATA_DIR/extensions/spark/jars/spark-extensions.jar"
TRINO_DEST="$BIGDATA_DIR/extensions/trino/plugins/e2e-functions"
BUILD_SYSTEM="${BUILD_SYSTEM:-bazel}"
MAVEN_IMAGE="${MAVEN_IMAGE:-maven:3.9-eclipse-temurin-23}"
MAVEN_CACHE_VOLUME="${MAVEN_CACHE_VOLUME:-playground-maven-cache}"
TIMEOUT_SECONDS="${E2E_TIMEOUT_SECONDS:-600}"
COMPOSE=(docker compose --project-directory "$BIGDATA_DIR" -f "$BIGDATA_DIR/docker-compose.yml")

log() { printf '\n==> %s\n' "$*"; }
fail() { printf '\n[FAIL] %s\n' "$*" >&2; exit 1; }
require_cmd() { command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"; }
compose() { "${COMPOSE[@]}" "$@"; }

on_error() {
    local status=$?
    printf '\n[FAIL] E2E failed (exit %d). Containers and volumes were preserved.\n' "$status" >&2
    compose ps >&2 || true
    printf '[INFO] Inspect logs with: cd %s && docker compose logs --tail=200\n' "$BIGDATA_DIR" >&2
    exit "$status"
}
trap on_error ERR

check_dependencies() {
    require_cmd docker
    require_cmd unzip
    if [[ "$BUILD_SYSTEM" == bazel ]]; then
        require_cmd bazel
    elif [[ "$BUILD_SYSTEM" != maven ]]; then
        fail "unsupported BUILD_SYSTEM: $BUILD_SYSTEM (expected bazel or maven)"
    fi
    docker compose version >/dev/null
    docker info >/dev/null
}

build_extensions() {
    local spark_jar trino_zip trino_jar
    if [[ "$BUILD_SYSTEM" == bazel ]]; then
        log "Building Spark and Trino extensions with Bazel"
        (
            cd "$REPO_ROOT"
            bazel test //java/pl/bigdata:tests --test_output=errors
            bazel build //java/pl/bigdata:spark_extension_jar //java/pl/bigdata:trino_plugin_zip
        )
        spark_jar="$REPO_ROOT/bazel-bin/java/pl/bigdata/spark-extensions/spark-extensions.jar"
        trino_zip="$REPO_ROOT/bazel-bin/java/pl/bigdata/trino-extensions/e2e-functions.zip"
        trino_jar="trino-extensions.jar"
    else
        log "Building Spark and Trino extensions with Maven in $MAVEN_IMAGE"
        docker run --rm \
            -v "$REPO_ROOT:/workspace" \
            -v "$MAVEN_CACHE_VOLUME:/root/.m2" \
            -w /workspace/java \
            "$MAVEN_IMAGE" \
            mvn -pl pl/bigdata/spark-extensions,pl/bigdata/trino-extensions -am clean package
        spark_jar="$SPARK_MODULE/target/spark-extensions-1.0.0-SNAPSHOT.jar"
        trino_zip="$TRINO_MODULE/target/e2e-functions.zip"
        trino_jar="trino-extensions-1.0.0-SNAPSHOT.jar"
    fi

    rm -f "$SPARK_DEST"
    rm -rf "$TRINO_DEST"
    install -m 0644 "$spark_jar" "$SPARK_DEST"
    unzip -q "$trino_zip" -d "$BIGDATA_DIR/extensions/trino/plugins"
    test -f "$TRINO_DEST/$trino_jar"
    log "Extensions deployed"
}

service_ready() {
    local service="$1" container status
    container="$(compose ps -q "$service")"
    test -n "$container" || return 1
    status="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$container")"
    [[ "$status" == healthy || "$status" == running ]]
}

wait_for() {
    local description="$1"; shift
    local deadline=$((SECONDS + TIMEOUT_SECONDS))
    log "Waiting for $description"
    until "$@"; do
        (( SECONDS < deadline )) || fail "timed out waiting for $description"
        sleep 3
    done
}

hdfs_ready() {
    local report
    report="$(compose exec -T namenode hdfs dfsadmin -report 2>/dev/null)" || return 1
    grep -Eq '^[[:space:]]*Live datanodes \(1\):' <<<"$report"
}

trino_ready() {
    local output
    output="$(compose exec -T trino-coordinator trino --server http://127.0.0.1:8099 --output-format TSV --execute "SELECT count(*) FROM system.runtime.nodes WHERE state = 'active'" 2>/dev/null)" || return 1
    [[ "$output" == "2" ]]
}

start_cluster() {
    log "Preparing framework dependencies and local credentials"
    "$BIGDATA_DIR/bootstrap.sh" --no-start
    log "Rebuilding Spark nodes and the Trino extension image"
    compose up -d --build --force-recreate
    local service
    for service in namenode datanode mysql hivemetastore hiveserver2 spark-master spark-worker trino-coordinator trino-worker-1; do
        wait_for "$service" service_ready "$service"
    done
    wait_for "one live HDFS DataNode" hdfs_ready
    compose exec -T namenode hdfs dfs -mkdir -p /user/hive/warehouse /tmp
    compose exec -T namenode hdfs dfs -chmod -R 777 /user/hive/warehouse /tmp
    wait_for "two active Trino nodes" trino_ready
}

run_tests() {
    log "Mounting SQL fixtures into the Spark container"
    docker cp "$SCRIPT_DIR/spark-extension.sql" spark-master:/opt/spark/e2e-spark-extension.sql
    docker cp "$SCRIPT_DIR/spark-persistent-validation.sql" spark-master:/opt/spark/e2e-spark-persistent-validation.sql

    log "Testing Spark custom format and Hive-style UDFs"
    compose exec -T spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077 \
        -f /opt/spark/e2e-spark-extension.sql
    log "Testing persistent Spark UDF from a fresh session"
    compose exec -T spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077 \
        -f /opt/spark/e2e-spark-persistent-validation.sql

    log "Checking Trino plugin installation on coordinator and worker"
    local service logs
    for service in trino-coordinator trino-worker-1; do
        logs="$(compose logs "$service")"
        grep -q 'Loading plugin /usr/lib/trino/plugin/e2e-functions' <<<"$logs"
        grep -q 'Installing io.github.liubang.trino.E2eFunctionsPlugin' <<<"$logs"
    done

    log "Testing Trino scalar function, NULL handling, nodes, and catalogs"
    compose exec -T trino-coordinator trino --server http://127.0.0.1:8099 \
        --file /dev/stdin <"$SCRIPT_DIR/trino-extension.sql"
    log "All E2E assertions passed"
}

cleanup_test_data() {
    log "Cleaning E2E database and container-side SQL fixtures"
    if service_ready spark-master; then
        compose exec -T spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077 \
            -e "DROP DATABASE IF EXISTS e2e_extension CASCADE" || true
        compose exec -T spark-master rm -f \
            /opt/spark/e2e-spark-extension.sql /opt/spark/e2e-spark-persistent-validation.sql || true
    fi
}

clean_artifacts() {
    cleanup_test_data
    rm -f "$SPARK_DEST"
    rm -rf "$TRINO_DEST"
    log "Local extension deployment artifacts removed"
}

usage() {
    echo "Usage: $0 {all|build|start|test|clean|down|reset}"
}

main() {
    local command="${1:-all}"
    check_dependencies
    case "$command" in
        all)
            build_extensions
            start_cluster
            run_tests
            clean_artifacts
            ;;
        build) build_extensions ;;
        start) start_cluster ;;
        test) run_tests ;;
        clean) clean_artifacts ;;
        down) compose down ;;
        reset)
            compose down -v
            clean_artifacts
            ;;
        *) usage; exit 2 ;;
    esac
}

main "$@"
