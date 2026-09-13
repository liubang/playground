# AuraBar

macOS 菜单栏面板：日历（节假日/农历/节气）、天气（Apple Weather / OpenMeteo / QWeather）、系统状态（CPU/GPU/磁盘/电池）等模块的菜单栏时钟与弹窗。

- Bundle ID：`cc.liubang.aurabar`
- 最低系统：macOS 14

## 构建与安装（Bazel）

```bash
# 构建（产出 .app zip）
bazel build //swift/pl/aurabar:AuraBar

# 一键安装：解包到 /Applications（同卷 staging + 原子 mv）、lsregister
# 重注册、并以稳定本地身份重新 codesign
bazel run //swift/pl/aurabar:install

# 启动验证（菜单栏 app 必须从 LaunchServices 启动，见下方「注意」）
open /Applications/AuraBar.app

# 创建本地自签名身份（幂等，一台机器一次；install 时缺失也会自动补）
bazel run //swift/pl/aurabar:make-signing-cert
```

## 测试

```bash
bazel test //swift/pl/aurabar/ut/...
```

## WeatherKit  entitlement

`com.apple.weatherkit` 是受限 entitlement：自签本地身份没有 provisioning profile，带上它 amfid 会拒绝启动 app。因此：

- 构建默认使用**空 dev entitlements**（`resources/AuraBar.dev.entitlements`），WeatherKit 不可用，退回其他天气源；
- 有付费开发者账号时可用完整 entitlements 构建：

```bash
bazel build //swift/pl/aurabar:AuraBar --//swift/pl/aurabar:weatherkit
```

- `:install` 重签时按签名身份自动选 entitlements：`Developer ID Application:` / `Apple Development:` 等真实开发者身份 → 完整集，其余 → dev 集；可用 `AURABAR_WEATHERKIT=1/0` 显式覆盖。

## 环境变量

| 变量 | 默认 | 说明 |
|------|------|------|
| `AURABAR_INSTALL_DIR` | `/Applications` | `:install` 的目标目录 |
| `AURABAR_SIGN_IDENTITY` | 自动发现/创建 | 显式指定 codesigning 身份 |
| `AURABAR_CERT_CN` | `AuraBar Dev (liubang)` | 自签身份 CN |
| `AURABAR_WEATHERKIT` | 按签名身份推断 | 重签时是否使用完整 entitlements（`1`/`0`） |

## 注意

- **Bundle ID 纪律**：AuraBar 是菜单栏 app，绝不能从终端或 IDE 直接运行 bundle 内二进制（macOS Tahoe 的菜单栏可见性记录问题，AuraBar/AuraClip 都踩过）。构建产物只走 `bazel run :install` + `open`。
