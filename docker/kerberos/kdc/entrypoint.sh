#!/usr/bin/env bash
# KDC 容器入口：首次启动时初始化 realm、创建主体并导出 keytab，然后前台运行 krb5kdc。
set -euo pipefail

REALM="${KRB5_REALM:-LAB.LOCAL}"
# 实验室环境使用固定弱密码，方便体验；真实环境绝不能这么干。
MASTER_PASSWORD="${KRB5_MASTER_PASSWORD:-masterkey123}"
ADMIN_PASSWORD="${KRB5_ADMIN_PASSWORD:-admin123}"
ALICE_PASSWORD="${KRB5_ALICE_PASSWORD:-alice123}"
BOB_PASSWORD="${KRB5_BOB_PASSWORD:-bob12345}"

KEYTAB_DIR=/keytabs
STAMP=/var/lib/krb5kdc/.realm_initialized

mkdir -p "$KEYTAB_DIR"

if [ ! -f "$STAMP" ]; then
    echo "[init] 创建 realm: $REALM"
    # -s: 生成 stash 文件（保存 master key，KDC 重启免输密码）
    kdb5_util create -s -P "$MASTER_PASSWORD" -r "$REALM"

    echo "[init] 创建主体..."
    # 管理员（*/admin 匹配 kadm5.acl）
    kadmin.local -q "addprinc -pw $ADMIN_PASSWORD admin/admin@$REALM"
    # 两个测试用户，用于体验 kinit
    kadmin.local -q "addprinc -pw $ALICE_PASSWORD alice@$REALM"
    kadmin.local -q "addprinc -pw $BOB_PASSWORD bob@$REALM"
    # demo 服务主体：服务不会输密码，用随机密钥 + keytab
    kadmin.local -q "addprinc -randkey demo/demo-server@$REALM"

    touch "$STAMP"
    echo "[init] 初始化完成"
else
    echo "[init] realm 已存在，跳过初始化"
fi

# keytab 与 realm 初始化解耦：keytabs 卷被单独清空时也能自愈。
# 注意 ktadd 默认会重新随机化密钥（KVNO +1），旧 keytab 即刻失效。
if [ ! -f "$KEYTAB_DIR/demo.service.keytab" ]; then
    echo "[init] 导出 keytab 到共享卷: $KEYTAB_DIR/demo.service.keytab"
    kadmin.local -q "ktadd -k $KEYTAB_DIR/demo.service.keytab demo/demo-server@$REALM"
    chmod 444 "$KEYTAB_DIR/demo.service.keytab"
fi

echo "[run] 启动 kadmind（后台）与 krb5kdc（前台）..."
kadmind
exec krb5kdc -n
