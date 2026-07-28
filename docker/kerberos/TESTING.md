# Kerberos 实验室测试说明

一个最小但完整的 Kerberos 环境，用于亲手体验认证全流程：`kinit → TGT → 服务票据 → 双向认证 → 会话密钥加密通信`。本文件覆盖拓扑、自动化测试（8 项断言）、手动实验指引与故障排查。

## 拓扑与组件

3 个容器，均在 `kerberos` 网络内：

| 容器 | 镜像 | 角色 |
|------|------|------|
| `krb5-kdc` | `kdc/Dockerfile`（debian + MIT krb5-kdc/kadmind） | KDC，realm `LAB.LOCAL`，88 端口映射到 `127.0.0.1`（tcp+udp） |
| `krb5-demo-server` | `demo/Dockerfile`（python3 + python3-gssapi） | GSSAPI echo 服务，监听 9999，用 keytab 证明 `demo/demo-server@LAB.LOCAL` 身份 |
| `krb5-client` | 同 demo 镜像，`sleep infinity` | 实验操作台，预装 krb5-user / python3-gssapi |

两个 volume：`kdc-data`（realm 数据库 + 初始化标记）与 `keytabs`（KDC 导出、demo-server/client 只读挂载的共享 keytab）。

预置主体（密码可用 `KRB5_*` 环境变量覆盖，见 `kdc/entrypoint.sh`）：

| Principal | 类型 | 凭证 |
|-----------|------|------|
| `admin/admin@LAB.LOCAL` | 管理员（匹配 `kadm5.acl`） | 密码 `admin123` |
| `alice@LAB.LOCAL` | 测试用户 | 密码 `alice123` |
| `bob@LAB.LOCAL` | 测试用户 | 密码 `bob12345` |
| `demo/demo-server@LAB.LOCAL` | 服务主体 | 随机密钥 + `/keytabs/demo.service.keytab` |

## 前提与命令

需要 Docker + Compose v2。镜像构建约 1-2 分钟（之后有缓存）。

```bash
cd docker/kerberos

./bootstrap.sh              # 构建、启动并打印快速玩法；--no-start 只准备不启动；--reset 清空重来

./tests/e2e.sh all          # 构建 + 启动 + 8 项断言
./tests/e2e.sh start        # 只启动并等待健康
./tests/e2e.sh test         # 对已运行的环境执行断言（幂等，可反复跑）
./tests/e2e.sh down         # 停止，保留 volume（realm 数据与票据主体保留）
./tests/e2e.sh reset        # 删除容器和全部 volume
```

`E2E_TIMEOUT_SECONDS` 可调等待上限（默认 120s）。KDC 健康检查通过才启动 demo-server/client，保证 keytab 已导出。

## E2E 断言明细（tests/e2e.sh）

每项断言对应一个 Kerberos 核心概念，建议对照体验：

1. **alice 密码 kinit 拿到 TGT** — 对应 `AS-REQ/AS-REP` 交换；`klist` 中必须出现 `krbtgt/LAB.LOCAL@LAB.LOCAL`。
2. **错误密码被 KDC 拒绝** — `kdc.conf` 中 `default_principal_flags = +preauth` 开启预认证，kinit 时必须先向 KDC 证明知道密码，防止离线爆破。
3. **双向认证 + 加密回显** — 客户端持 TGT 换服务票据（`TGS-REQ/TGS-REP`），与服务端完成 mutual auth（`AP-REQ/AP-REP`），随后消息用协商出的会话密钥经 GSS wrap/unwrap 加密传输；断言回显为 `ECHO[alice@LAB.LOCAL]: hello from e2e`。
4. **ccache 中同时存在 TGT 与服务票据** — 观察"两次换票"的实体证据：Test 1 的 `klist` 只有 TGT，本步多出 `demo/demo-server`。
5. **kdestroy 后访问失败** — 客户端报 `MissingCredentialsError`，证明认证是强制的而非装饰。
6. **keytab 免密认证** — `kinit -kt demo.service.keytab demo/demo-server@LAB.LOCAL` 成功；这是服务/定时任务场景的标准姿势（Hadoop 生态里 NN/DN/Hive 的 keytab 与此同款）。
7. **bob 独立认证访问** — 服务端通过 `ctx.initiator_name` 区分身份，回显为 `ECHO[bob@LAB.LOCAL]: ...`。
8. **服务端审计日志** — `docker compose logs demo-server` 中同时出现 alice 与 bob 的"认证通过"记录。

失败时脚本保留现场并提示查看日志；`test` 子命令依赖运行中的环境，不会自行重建。

## 手动实验指引

```bash
docker compose exec client bash

# 1) 密码认证拿 TGT（密码 alice123）
kinit alice && klist

# 2) 访问 Kerberos 保护的服务；之后再 klist，会多出 demo/demo-server 服务票据
python3 /app/client.py 'hello kerberos'
klist

# 3) 销毁票据后再访问，体验 MissingCredentialsError
kdestroy && python3 /app/client.py

# 4) keytab：查看内容（KVNO、加密类型），并用它免密认证
klist -kt /keytabs/demo.service.keytab
kinit -kt /keytabs/demo.service.keytab demo/demo-server@LAB.LOCAL

# 5) 远程管理 KDC（密码 admin123）
kadmin -p admin/admin@LAB.LOCAL
# kadmin 里可以试试：getprinc alice / addprinc carol / listprincs

# 6) 抓包看 Kerberos 报文（容器内）
apt-get update && apt-get install -y tcpdump
tcpdump -i any -nn port 88   # 另开终端执行 kinit / client.py，观察 AS/TGS 报文
```

宿主机直连：KDC 的 88 端口已映射到 `127.0.0.1`，在 macOS 上给 `/etc/krb5.conf` 增加 `LAB.LOCAL` realm（`kdc = 127.0.0.1`）后即可直接 `kinit alice@LAB.LOCAL`。

## 故障排查

- **KDC 不健康**：`docker compose logs kdc`；常见原因是 88 端口被宿主机占用，改 `docker-compose.yml` 的端口映射或先 `e2e.sh reset`。
- **`Server not found in Kerberos database`**：principal 的 hostname 部分与服务实际主机名不一致。本实验室通过在 `krb5.conf` 中设置 `rdns = false` + `dns_canonicalize_hostname = false` 规避 DNS 反解带来的不一致，principal 必须与 compose 的 `hostname` 严格一致。
- **`Cannot contact any KDC for realm`**：容器内 `getent hosts kdc` 确认解析；确认 `krb5.conf` 挂载生效（`/etc/krb5.conf`）。
- **`Clock skew too great`**：容器与 KDC 共享宿主机时钟，正常不会出现；若出现（如虚拟机时钟漂移），重启 Docker Desktop。
- **demo-server 起不来**：确认 `docker compose logs kdc` 中 keytab 已导出；`keytabs` 卷被单独清空时 KDC 入口脚本会自动重新导出（KVNO +1，旧 keytab 即刻失效）。
- **凭证缓存丢失**：ccache 在 client 容器的 `/tmp`，容器重建即失效，重新 `kinit` 即可。
- **彻底重来**：`./tests/e2e.sh reset && ./tests/e2e.sh all`。

## 已知非生产边界

- 所有密码为固定弱密码（可用环境变量覆盖），仅用于本地学习；realm master key 以 stash 文件明文等效形式保存在 KDC 容器内。
- 单 KDC，无 HA、无从 KDC 同步；749（kadmin）未映射到宿主机，但同网络容器可用。
- 票据生命周期 10h / 可续期 7d；无策略细化（每 principal 的 maxlife 等均默认）。
- demo 协议为自定义 TCP 二进制帧，仅演示 GSSAPI 握手与 wrap/unwrap，不代表 SPNEGO/HTTP 等真实应用层形态。
- `python3-gssapi`（Debian bookworm 1.8.x）的 `SecurityContext` 无 `mutually_authenticated` 属性，代码用 `actual_flags` 判断双向认证，与新版 pip 包 API 有差异。
