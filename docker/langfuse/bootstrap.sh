#!/usr/bin/env bash
# Langfuse v3 模块：生成所有凭证、配置外部 URL，并启动完整观测栈。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../scripts/bootstrap-common.sh"
bootstrap_init "$SCRIPT_DIR" "$@"

bootstrap_check_dependencies
bootstrap_handle_reset

gen_hex_secret() {
    local bytes="${1:-32}" length secret
    [[ "$bytes" =~ ^[1-9][0-9]*$ ]] || die "无效的密钥字节数: $bytes"
    length=$((bytes * 2))
    secret="$(docker run --rm alpine:3 sh -c "tr -dc 0-9a-f </dev/urandom | head -c '$length'")"
    [ "${#secret}" -eq "$length" ] || die "十六进制密钥生成失败"
    printf '%s' "$secret"
}

validate_http_url() {
    local name="$1" value="$2"
    [[ "$value" =~ ^https?://[^[:space:]/]+(:[0-9]+)?(/[^[:space:]]*)?$ ]] || \
        die "$name 必须是完整的 http(s) URL: $value"
}

prepare_env() {
    echo "==> 准备 .env..."
    if [ -f .env ]; then
        log_skip ".env 已存在，不覆盖"
        return
    fi

    select_bind_mode \
        "Langfuse Web 和媒体存储绑定 0.0.0.0，MinIO Console 仅本机可访问" \
        "Langfuse Web、媒体存储和 MinIO Console 均绑定 0.0.0.0" \
        "MinIO Console 将对外开放（所有入口均未启用 TLS）"

    local langfuse_public_url="http://localhost:3100"
    local minio_public_url="http://localhost:9190"
    if [ "$BIND_MODE" != "local" ]; then
        printf '\n公开模式下，外部 URL 必须能被浏览器和 SDK 访问，不能使用 localhost。\n'
        local public_host
        public_host="$(hostname)"
        prompt_value langfuse_public_url "Langfuse 外部 URL" "http://${public_host}:3100"
        prompt_value minio_public_url "MinIO S3 外部 URL" "http://${public_host}:9190"
        [[ "$langfuse_public_url" != *localhost* && "$langfuse_public_url" != *127.0.0.1* && "$langfuse_public_url" != *0.0.0.0* ]] || \
            die "公开模式的 Langfuse URL 必须使用其他设备可访问的主机名或 IP"
        [[ "$minio_public_url" != *localhost* && "$minio_public_url" != *127.0.0.1* && "$minio_public_url" != *0.0.0.0* ]] || \
            die "公开模式的 MinIO URL 必须使用其他设备可访问的主机名或 IP"
    fi
    validate_http_url LANGFUSE_PUBLIC_URL "$langfuse_public_url"
    validate_http_url MINIO_PUBLIC_URL "$minio_public_url"

    local postgres_password clickhouse_password redis_auth minio_password
    local nextauth_secret salt encryption_key
    postgres_password="$(gen_secret 32)"
    clickhouse_password="$(gen_secret 32)"
    redis_auth="$(gen_secret 32)"
    minio_password="$(gen_secret 32)"
    nextauth_secret="$(gen_secret 48)"
    salt="$(gen_secret 32)"
    encryption_key="$(gen_hex_secret 32)"

    write_env_file .env \
        LANGFUSE_VERSION "3.223.0" \
        POSTGRES_VERSION "17" \
        REDIS_VERSION "7" \
        CLICKHOUSE_VERSION "latest" \
        MINIO_VERSION "latest" \
        POSTGRES_USER "postgres" \
        POSTGRES_PASSWORD "$postgres_password" \
        POSTGRES_DB "postgres" \
        CLICKHOUSE_USER "clickhouse" \
        CLICKHOUSE_PASSWORD "$clickhouse_password" \
        REDIS_AUTH "$redis_auth" \
        MINIO_ROOT_USER "minio" \
        MINIO_ROOT_PASSWORD "$minio_password" \
        NEXTAUTH_SECRET "$nextauth_secret" \
        SALT "$salt" \
        ENCRYPTION_KEY "$encryption_key" \
        TELEMETRY_ENABLED "false" \
        LANGFUSE_ENABLE_EXPERIMENTAL_FEATURES "false" \
        LANGFUSE_PORT "3100" \
        MINIO_API_PORT "9190" \
        MINIO_CONSOLE_PORT "9191" \
        PUBLIC_BIND_ADDR "$PUBLIC_BIND_ADDR" \
        INTERNAL_BIND_ADDR "$INTERNAL_BIND_ADDR" \
        LANGFUSE_PUBLIC_URL "$langfuse_public_url" \
        MINIO_PUBLIC_URL "$minio_public_url"

    log_ok ".env 已生成（模式: ${BIND_MODE}，权限: 600）"
    echo "  所有数据库密码和应用密钥已随机生成，请妥善保管 .env"
}

check_web_ready() {
    docker compose exec -T langfuse-web node -e \
        'fetch("http://localhost:3000/api/public/health?failIfDatabaseUnavailable=true").then(r => process.exit(r.ok ? 0 : 1)).catch(() => process.exit(1))' \
        >/dev/null 2>&1
}

check_worker_ready() {
    docker compose exec -T langfuse-worker node -e \
        'fetch("http://localhost:3030/api/health").then(r => process.exit(r.ok ? 0 : 1)).catch(() => process.exit(1))' \
        >/dev/null 2>&1
}

prepare_env

if [ "$START_SERVICES" = true ]; then
    compose_start
    if ! wait_until "Langfuse Web 就绪" 90 3 check_web_ready; then
        docker compose ps
        printf '\n最近的 Web 日志:\n' >&2
        docker compose logs --tail=80 langfuse-web >&2
        exit 1
    fi
    if ! wait_until "Langfuse Worker 就绪" 60 3 check_worker_ready; then
        docker compose ps
        printf '\n最近的 Worker 日志:\n' >&2
        docker compose logs --tail=80 langfuse-worker >&2
        exit 1
    fi
    show_compose_status
    # .env 由本脚本生成且仅包含通过校验的 key/value。
    # shellcheck source=/dev/null
    source .env
    printf '\n==> 访问方式:\n'
    # 变量由上面的 .env 动态加载。
    # shellcheck disable=SC2153
    echo "  Langfuse       ${LANGFUSE_PUBLIC_URL}"
    # shellcheck disable=SC2153
    echo "  MinIO Console  http://localhost:${MINIO_CONSOLE_PORT}"
    echo ""
    echo "完成！首次登录请在 Langfuse 页面注册用户并创建项目。"
else
    print_no_start
fi
