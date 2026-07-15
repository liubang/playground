#!/usr/bin/env bash
# Monitor 模块：生成 Grafana 认证信息并启动 Prometheus + Grafana。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/bootstrap-common.sh
source "$SCRIPT_DIR/../scripts/bootstrap-common.sh"
bootstrap_init "$SCRIPT_DIR" "$@"

bootstrap_check_dependencies
bootstrap_handle_reset

echo "==> 准备 .env..."
if [ -f .env ]; then
    log_skip ".env 已存在，不覆盖"
else
    select_bind_mode \
        "仅 Grafana 绑定 0.0.0.0，Prometheus 仅本机可访问" \
        "Grafana 和无认证的 Prometheus 均绑定 0.0.0.0" \
        "Prometheus 将无认证对外开放"
    GRAFANA_ADMIN_USER="admin"
    GRAFANA_ADMIN_PASSWORD="$(gen_secret 20)"
    write_env_file .env \
        GRAFANA_ADMIN_USER "$GRAFANA_ADMIN_USER" \
        GRAFANA_ADMIN_PASSWORD "$GRAFANA_ADMIN_PASSWORD" \
        PUBLIC_BIND_ADDR "$PUBLIC_BIND_ADDR" \
        INTERNAL_BIND_ADDR "$INTERNAL_BIND_ADDR"
    log_ok ".env 已生成（模式: ${BIND_MODE}，Grafana: ${PUBLIC_BIND_ADDR}，Prometheus: ${INTERNAL_BIND_ADDR}）"
    echo "  Grafana 用户名: ${GRAFANA_ADMIN_USER}"
    echo "  Grafana 密码已写入 .env，请妥善保管"
fi

if [ "$START_SERVICES" = true ]; then
    compose_start
    show_compose_status
    printf '\n==> 访问方式:\n'
    echo "  Prometheus    http://localhost:9090"
    echo "  Grafana       http://localhost:3000  (认证信息见 .env)"
    echo ""
    echo "完成！"
else
    print_no_start
fi
