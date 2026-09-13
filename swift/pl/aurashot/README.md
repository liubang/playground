# AuraShot

macOS 菜单栏截图工具：区域/窗口截图、标注与马赛克、本地 mllm OCR（bundle 内置 `mllm_server`）、贴图（Pin）与剪贴板输出。

- Bundle ID：`cc.liubang.aurashot`
- 最低系统：macOS 14

## 构建与安装（Bazel）

```bash
# 构建（产出 .app zip）
bazel build //swift/pl/aurashot:AuraShot

# 一键安装：解包到 /Applications（同卷 staging + 原子 mv）、lsregister
# 重注册、并以稳定本地身份重新 codesign（TCC 屏幕录制授权才能跨构建保留）
bazel run //swift/pl/aurashot:install

# 启动验证（菜单栏 app 必须从 LaunchServices 启动，见下方「注意」）
open /Applications/AuraShot.app

# 创建本地自签名身份（幂等，一台机器一次；install 时缺失也会自动补）
bazel run //swift/pl/aurashot:make-signing-cert
```

## 测试

```bash
bazel test //swift/pl/aurashot/ut/...
```

## 环境变量

| 变量 | 默认 | 说明 |
|------|------|------|
| `AURASHOT_INSTALL_DIR` | `/Applications` | `:install` 的目标目录 |
| `AURASHOT_SIGN_IDENTITY` | 自动发现/创建 | 显式指定 codesigning 身份 |
| `AURASHOT_CERT_CN` | `AuraShot Dev (liubang)` | 自签身份 CN |

## 注意

- **Bundle ID 纪律**：AuraShot 是 LSUIElement/菜单栏 app，绝不能从终端或 IDE 直接运行 bundle 内二进制（macOS Tahoe 会把菜单栏可见性记录永久绑定到隐藏分组，bundle ID 会废了——AuraClip 的前车之鉴）。构建产物只走 `bazel run :install` + `open`。
- bundle 内的 `mllm_server`（OCR 引擎）经 `opt_binary` 固定为 release 构建（仓库默认 C++ 配置是 debug+ASan，不适合打包）；模型 GGUF（数 GB）不打进 bundle，仍在模型目录下。
- `:install` 在替换前会 `pkill` 掉正在运行的 AuraShot 及旧 bundle 里残留的 `mllm_server`（避免端口被旧二进制占用）。
