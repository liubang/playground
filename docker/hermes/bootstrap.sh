#!/usr/bin/env bash
# Hermes 模块：生成 Dashboard 认证信息并按安全边界暴露端口。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/bootstrap-common.sh
source "$SCRIPT_DIR/../scripts/bootstrap-common.sh"
bootstrap_init "$SCRIPT_DIR" "$@"

bootstrap_check_dependencies
bootstrap_handle_reset

mkdir -p data
echo "==> 准备 .env..."
if [ -f .env ]; then
    log_skip ".env 已存在，不覆盖"
else
    echo ""
    echo "首次初始化，请设置 Dashboard 登录信息"
    prompt_value HERMES_DASHBOARD_BASIC_AUTH_USERNAME "Dashboard 用户名" "admin"
    prompt_secret HERMES_DASHBOARD_BASIC_AUTH_PASSWORD "Dashboard 密码" 16
    HERMES_DASHBOARD_BASIC_AUTH_SECRET="$(gen_secret 32)"
    select_bind_mode \
        "仅 Dashboard 绑定 0.0.0.0，Gateway API 仅本机可访问" \
        "Dashboard 和 Gateway API 均绑定 0.0.0.0" \
        "Gateway API 将对外开放"
    write_env_file .env \
        HERMES_DASHBOARD_BASIC_AUTH_USERNAME "$HERMES_DASHBOARD_BASIC_AUTH_USERNAME" \
        HERMES_DASHBOARD_BASIC_AUTH_PASSWORD "$HERMES_DASHBOARD_BASIC_AUTH_PASSWORD" \
        HERMES_DASHBOARD_BASIC_AUTH_SECRET "$HERMES_DASHBOARD_BASIC_AUTH_SECRET" \
        PUBLIC_BIND_ADDR "$PUBLIC_BIND_ADDR" \
        INTERNAL_BIND_ADDR "$INTERNAL_BIND_ADDR"
    log_ok ".env 已生成（模式: ${BIND_MODE}，Dashboard: ${PUBLIC_BIND_ADDR}，Gateway API: ${INTERNAL_BIND_ADDR}）"
    echo "  Dashboard 用户名: ${HERMES_DASHBOARD_BASIC_AUTH_USERNAME}"
    echo "  Dashboard 密码已写入 .env，请妥善保管"
fi

if [ "$START_SERVICES" = true ]; then
    compose_start
    show_compose_status
    printf '\n==> 访问方式:\n'
    echo "  Gateway API   http://localhost:8642"
    echo "  Dashboard     http://localhost:9119"
    echo ""
    echo "完成！"
else
    print_no_start
fi
