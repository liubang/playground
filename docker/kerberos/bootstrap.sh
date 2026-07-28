#!/usr/bin/env bash
# Kerberos 实验室：KDC + GSSAPI demo 服务 + 客户端容器，用于体验 Kerberos 认证全流程。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/bootstrap-common.sh
source "$SCRIPT_DIR/../scripts/bootstrap-common.sh"
bootstrap_init "$SCRIPT_DIR" "$@"

bootstrap_check_dependencies
bootstrap_handle_reset

if [ "$START_SERVICES" = true ]; then
    compose_start

    kdc_healthy() {
        docker compose ps kdc 2>/dev/null | grep -q "healthy"
    }
    wait_until "KDC 就绪" 30 2 kdc_healthy || die "KDC 启动失败"

    show_compose_status
    printf '\n==> 快速体验:\n'
    echo "  # 进入客户端容器"
    echo "  docker compose exec client bash"
    echo ""
    echo "  # 1) 用户认证：kinit 拿 TGT（密码 alice123）"
    echo "  kinit alice && klist"
    echo ""
    echo "  # 2) 访问 Kerberos 保护的服务（自动用 TGT 换服务票据）"
    echo "  python3 /app/client.py 'hello kerberos'"
    echo ""
    echo "  # 3) 销毁票据后再试，会被拒绝（感受认证的作用）"
    echo "  kdestroy && python3 /app/client.py"
    echo ""
    echo "  # 4) 服务间的姿势：用 keytab 免密认证"
    echo "  kinit -kt /keytabs/demo.service.keytab demo/demo-server@LAB.LOCAL && klist"
    echo ""
    echo "完整测试: ./tests/e2e.sh"
else
    print_no_start
fi
