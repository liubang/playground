#!/usr/bin/env bash
# Kerberos 实验室 E2E：覆盖密码认证、服务票据、加密通信、无凭证拒绝、keytab 认证。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERBEROS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TIMEOUT_SECONDS="${E2E_TIMEOUT_SECONDS:-120}"
COMPOSE=(docker compose --project-directory "$KERBEROS_DIR" -f "$KERBEROS_DIR/docker-compose.yml")

log() { printf '\n==> %s\n' "$*"; }
fail() {
    printf '\n[FAIL] %s\n' "$*" >&2
    exit 1
}
require_cmd() { command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"; }
compose() { "${COMPOSE[@]}" "$@"; }

on_error() {
    local status=$?
    printf '\n[FAIL] E2E failed (exit %d). Containers were preserved.\n' "$status" >&2
    compose ps >&2 || true
    printf '[INFO] Inspect logs with: cd %s && docker compose logs --tail=200\n' "$KERBEROS_DIR" >&2
    exit "$status"
}
trap on_error ERR

check_dependencies() {
    require_cmd docker
    docker compose version >/dev/null
    docker info >/dev/null
}

service_ready() {
    local service="$1" container status
    container="$(compose ps -q "$service")"
    test -n "$container" || return 1
    status="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$container")"
    [[ "$status" == healthy || "$status" == running ]]
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

start_lab() {
    log "Building images and starting KDC lab"
    compose up -d --build
    local service
    for service in kdc demo-server client; do
        wait_for "$service" service_ready "$service"
    done
}

run_tests() {
    local output

    log "Test 1: alice 密码 kinit，应拿到 TGT"
    output="$(compose exec -T client bash -c 'echo alice123 | kinit alice && klist' 2>&1)"
    grep -q 'krbtgt/LAB.LOCAL@LAB.LOCAL' <<<"$output" || fail "alice 未拿到 TGT"
    echo "$output"

    log "Test 2: 错误密码应被 KDC 拒绝（预认证生效）"
    if compose exec -T client bash -c 'echo wrong-password | kinit alice' >/dev/null 2>&1; then
        fail "错误密码竟然 kinit 成功"
    fi
    echo "[OK] 错误密码被拒绝"

    log "Test 3: 访问 demo 服务，应完成双向认证并收到加密回显"
    output="$(compose exec -T client python3 /app/client.py 'hello from e2e' 2>&1)"
    grep -q 'mutual=True' <<<"$output" || fail "双向认证未完成"
    grep -q 'ECHO\[alice@LAB.LOCAL\]: hello from e2e' <<<"$output" || fail "回显内容不正确"
    echo "$output"

    log "Test 4: ccache 中应同时存在 TGT 和 demo/demo-server 服务票据"
    output="$(compose exec -T client klist 2>&1)"
    grep -q 'krbtgt/LAB.LOCAL@LAB.LOCAL' <<<"$output" || fail "缺少 TGT"
    grep -q 'demo/demo-server' <<<"$output" || fail "缺少服务票据"
    echo "$output"

    log "Test 5: kdestroy 后访问服务应失败（认证是必须的）"
    compose exec -T client kdestroy
    if compose exec -T client python3 /app/client.py 'should fail' >/dev/null 2>&1; then
        fail "无凭证竟然访问成功"
    fi
    echo "[OK] 无凭证访问被拒绝"

    log "Test 6: keytab 免密认证（服务间场景）"
    output="$(compose exec -T client bash -c \
        'kinit -kt /keytabs/demo.service.keytab demo/demo-server@LAB.LOCAL && klist' 2>&1)"
    grep -q 'Default principal: demo/demo-server@LAB.LOCAL' <<<"$output" || fail "keytab 认证失败"
    echo "$output"

    log "Test 7: 另一个用户 bob 也可认证并访问服务（服务端能区分身份）"
    output="$(compose exec -T client bash -c \
        'kdestroy && echo bob12345 | kinit bob && python3 /app/client.py "hi from bob"' 2>&1)"
    grep -q 'ECHO\[bob@LAB.LOCAL\]: hi from bob' <<<"$output" || fail "bob 访问失败或身份不对"
    echo "$output"

    log "Test 8: 服务端日志应记录 alice 和 bob 的认证身份"
    output="$(compose logs demo-server 2>&1)"
    grep -q '认证通过: alice@LAB.LOCAL' <<<"$output" || fail "服务端日志缺少 alice"
    grep -q '认证通过: bob@LAB.LOCAL' <<<"$output" || fail "服务端日志缺少 bob"

    log "All E2E assertions passed"
}

usage() {
    echo "Usage: $0 {all|start|test|down|reset}"
}

main() {
    local command="${1:-all}"
    check_dependencies
    case "$command" in
    all)
        start_lab
        run_tests
        ;;
    start) start_lab ;;
    test) run_tests ;;
    down) compose down ;;
    reset) compose down -v ;;
    *)
        usage
        exit 2
        ;;
    esac
}

main "$@"
