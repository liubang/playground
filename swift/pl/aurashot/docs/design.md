# AuraShot 设计文档

> 状态：Draft v0.2
> 定位：macOS 菜单栏截图工具，对标 Xnip；长期接入 cpp/pl/mllm 的 PaddleOCR-VL 引擎，实现光标级文字识别与选取。

## 1. 目标与非目标

### 1.1 目标（按优先级）

1. **截图体验对标 Xnip**：区域截图、窗口磁铁吸附、标注（矩形/箭头/文字/马赛克）、贴图、复制/保存、全局快捷键。
2. **OCR 是终态差异化**：光标 hover 文字块 → 高亮 → 点选/拖拽拷贝；整图 OCR 文本导出；未来支持表格/公式（PaddleOCR-VL 原生能力）。
3. **工程上复用现有资产**：swift/pl/aurabar 的构建/签名/安装链路，cpp/pl/mllm 的 OCR 推理内核。

### 1.2 非目标

- 不做录屏/GIF（ScreenCaptureKit 支持，但超出本期范围）。
- 不做云同步、账号体系、内购。
- 不上 App Store（自签名 + 本地安装，参考 aurabar 的 install.sh 链路）。
- 不做滚动截图（列入远期路线图，单独评估）。

## 2. 与 Xnip 功能对照

| 功能 | Xnip | AuraShot 计划 | 阶段 |
|---|---|---|---|
| 区域截图（拖拽选区） | ✅ | ✅ | M1 |
| 窗口磁铁吸附 | ✅ | ✅ | M1 |
| 复制到剪贴板 / 保存 PNG | ✅ | ✅ | M1 |
| 标注：矩形/椭圆/箭头/直线/文字/马赛克/序号 | ✅ | ✅（矩形/箭头/文字/马赛克先行） | M2 |
| 全局快捷键自定义 | ✅ | ✅（默认 ⌘⇧X） | M0/M1 |
| 贴图 Pin | ✅ | ✅ | M3 |
| OCR（整图） | ✅（系统 Vision） | ✅（PaddleOCR-VL，本地模型） | M4 |
| 光标 hover 选字 | ❌（TextSniper 有） | ✅（OCR 后 bbox 浮层） | M4 |
| 滚动截图 | ✅ | 远期 | — |

## 3. 技术选型与决策

| # | 决策 | 结论 | 理由 |
|---|---|---|---|
| D1 | 构建系统 | **Bazel rules_swift + macos_application**，复制 aurabar 的构建/安装/签名模板（BUILD.bazel、install.sh、make-signing-cert.sh、entitlements） | repo 既有约定；install.sh 用稳定自签名证书重签，**保证 TCC（屏幕录制）授权在反复构建间不丢**——这是截图工具开发期最大的坑。放弃 SwiftPM 方案：SwiftPM 产物是裸二进制、无 bundle，TCC 授权对象（cdhash）每次构建都变，开发迭代不可用。 |
| D2 | 屏幕捕获 | **SCScreenshotManager**（macOS 14+，主路径）；CGWindowListCreateImage 作 fallback | 前者是官方新 API，权限语义清晰；后者 macOS 14 起 deprecated 但仍可用，适配异常场景。LSMinimumSystemVersion 定为 14.0（与 aurabar 一致）。 |
| D3 | 应用形态 | LSUIElement 菜单栏常驻，无 Dock 图标 | Xnip 形态；模板 Info.plist 里已带。 |
| D4 | 全局快捷键 | Carbon RegisterEventHotKey | 不需要辅助功能权限，系统级监听稳定；API 古老但仍被 Alfred/Raycast 等同类使用。默认 ⌘⇧X 截图，预留 ⌘⇧O（OCR 截图）。 |
| D5 | 窗口磁铁吸附 | CGWindowListCopyWindowInfo 枚举 on-screen 窗口几何 | 窗口几何信息**不需要屏幕录制权限**（窗口名需要，坐标不需要），无需额外授权即可做吸附。 |
| D6 | 标注渲染 | 截图 + 标注统一用 **CALayer 树**（CAShapeLayer/CATextLayer），导出时 render 进 CGContext；马赛克用 CIFilter.pixellated | 可无损撤销/编辑（矢量状态模型），导出位图质量与屏幕一致；比 drawRect 整图重画省内存。 |
| D7 | OCR 接入 | UI 层只依赖 Swift 的 OcrEngine 协议；实现层 PaddleOcrEngine 通过 **Objective-C++ 桥接**调 cpp/pl/mllm 静态库 | 进程内调用（OCR 要传整屏像素，XPC/子进程成本不值）；mllm 由 Bazel 编静态库，objc_library 包一层桥接头后 link 进 aurashot_lib。 |
| D8 | 模型分发 | GGUF 不进 repo/App bundle，首次启动后台下载 + SHA256 校验到 ~/Library/Application Support/AuraShot/models/ | 模型 ~1–2GB，bundle 不能膨胀。 |

## 4. 总体架构

```
┌──────────────────────────── AuraShot.app (Swift, AppKit) ───────────────────────────┐
│                                                                                      │
│  app/                AuraShotApp(@main) · AppDelegate · StatusItemController         │
│    │                                                                                 │
│  hotkey/             HotKeyManager (Carbon, ⌘⇧X / ⌘⇧O)                                │
│    │                                                                                 │
│  capture/            ScreenCapture (SCScreenshotManager)                             │
│                      WindowGeometry (CGWindowList, 窗口磁铁吸附)                       │
│                      CapturePermissions (TCC 检查/引导)                               │
│    │                                                                                 │
│  capture_overlay/    CaptureSessionController（会话状态机，见 §5）                     │
│                      OverlayWindow（每屏一个，borderless, screenSaver level）          │
│                      SelectionView（选区拖拽/吸附/尺寸 HUD）                            │
│    │                                                                                 │
│  annotate/           Annotation（状态模型：矩形/箭头/文字/马赛克 + undo 栈）            │
│                      AnnotationLayer（CALayer 渲染）                                  │
│                      ToolStrip（标注工具条：工具/颜色/线宽/撤销/保存/复制）              │
│    │                                                                                 │
│  ocr/                OcrEngine（协议，见 §7）· OcrTypes · NullOcrEngine（占位）        │
│                      TextOverlayView（hover 高亮 bbox，光标选字；M4 启用）              │
│    │                                                                                 │
│  output/             ClipboardWriter · FileSaver · PinWindowController(M3)           │
│    │                                                                                 │
└────┼─────────────────────────────────────────────────────────────────────┼─────────┘
     │  ocr/PaddleOcrEngine (ObjC++ 桥, M4)                                  │
     ▼                                                                        │
┌──────────────────────────── C++ OCR 内核（Bazel 静态库）─────────────────────────────┐
│  cpp/pl/mllm: Engine（多模态 prefill+decode）· PaddleOcrTower · mrope · gguf loader  │
│  backend: cpu（已有）/ metal（建设中，OCR 性能关键）                                    │
└────────────────────────────────────────────────────────────────────────────────────┘
```

关键约束：

- **OCR 完全解耦**：M0–M3 阶段 ocr/ 目录只有协议和 NullOcrEngine。接 OCR 时只新增 PaddleOcrEngine 与桥接层，UI 零改动。
- **每屏独立捕获**：SCScreenshotManager 以 SCDisplay 为单位出图；跨屏选区为 M1 末期增强，先支持单屏选区。
- **状态模型与渲染分离**：标注是 Annotation 值类型数组，CALayer 只负责渲染，undo/重做/导出共用同一份状态。

## 5. 核心流程：一次截图的生命周期

```
快捷键 ⌘⇧X / 菜单点击（M4 起另有 ⌘⇧O 触发"截图 + OCR"）
   │
   ▼
CapturePermissions.ensure()          ── 未授权 → 引导面板(跳系统设置)，终止
   │
   ▼
CaptureSessionController.begin()
   ├─ 隐藏自身窗口，等待 ~80ms（让 UI 沉底）
   ├─ ScreenCapture.snapshotAllDisplays()  → [DisplayID: CGImage]（含 backingScaleFactor）
   ├─ 每屏创建 OverlayWindow：背景=该屏截图（调暗 15% 暗示"冻结"）
   └─ CGWindowListCopyWindowInfo 抓窗口几何 → 吸附候选表
   │
   ▼  交互阶段（SelectionView）
   hover:   命中窗口几何 → 显示该窗口虚框（磁铁吸附预览）
   拖拽:    实时选区 + 尺寸 HUD（W×H）
   Esc:     cancel() → 销毁全部 overlay
   单击吸附窗口 / 双击: confirm(selection)
   │
   ▼  标注阶段（M2 起；M1 直接跳过）
   选区旁显示 ToolStrip，标注写入 Annotation 状态栈（支持 ⌘Z undo）
   │
   ▼  输出阶段
   合成：屏幕截图 crop(selection) + 标注层 render（按 backingScaleFactor 出图）
   ├─ ⌘C / 回车 → ClipboardWriter（PNG 表示）
   ├─ ⌘S        → FileSaver（NSSavePanel，默认 ~/Pictures/AuraShot/yyyyMMdd-HHmmss.png）
   └─ "识别文字" (M4) → OcrEngine.recognize → TextOverlayView（hover 选字）/ 整图文本面板
   │
   ▼
end()：销毁 overlay、释放截图（M4 后可缓存到 OCR 完成）
```

## 6. 关键技术点

### 6.1 权限与签名（复用 aurabar 链路）

- TCC 屏幕录制授权按 codesign 标识判定。开发期反复构建必须走 install.sh 的**稳定自签名证书重签**流程（make-signing-cert.sh 一次性建证书），否则每构建一次就要重开一次系统设置。
- Info.plist 需要 NSScreenCaptureUsageDescription（zh_CN 文案，沿用 aurabar 风格）。
- 启动时检测：无权限 → 一次性引导面板（说明 + "打开系统设置"按钮 + 轮询授权状态，授权后自动继续）。

### 6.2 多显示器与坐标系（最容易出 bug 的地方）

- **三套坐标族**：CG 全局坐标（CGWindowList、SCDisplay，原点主屏左上角，y 向下）；NS 屏幕坐标（NSScreen.frame，原点主屏左下角，y 向上）；视图坐标（NSView，y 向上）。所有转换收敛到 CoordinateSpace.swift，单元测试覆盖（含主屏在左/右/上、负坐标排列）。
- **Retina 换算**：backingScaleFactor 每屏独立（外接 1x 屏 + 内置 2x 屏）。选区 rect 是 point 单位；裁剪 CGImage 前乘对应屏的 scale。OCR 输入图像与 bbox 一律用**像素坐标**，只在输出层换算一次。
- 窗口吸附按 CGWindowList 的 CG 坐标直接比对，避免来回换算误差。

### 6.3 窗口磁铁吸附

- 调用方式如下：

```swift
CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
```

- 过滤条件：kCGWindowLayer == 0（普通应用层）、alpha > 0、面积 > 阈值。
- 列表顺序即 z-order（前→后），hover 时命中第一个包含光标的窗口 → 单击选中该窗口 frame。
- 排除 AuraShot 自身窗口与系统 UI 进程。
- 拖拽中按住 ⇧ 临时切换"自由/吸附"模式（Xnip 行为）。

### 6.4 全局快捷键

- Carbon RegisterEventHotKey + InstallEventHandler(kEventHotKeyPressed)。
- 键位模型 HotKey { keyCode, modifiers } 存 UserDefaults；M2 提供偏好设置面板可改。
- 注册失败（冲突）→ 菜单栏图标红点 + 菜单内提示文案。

### 6.5 标注系统

- 状态模型（值类型）：

```swift
enum AnnotationKind { case rect, ellipse, arrow, line, text, mosaic }

struct Annotation {
    var kind: AnnotationKind
    var frame: CGRect            // 选区内 point 坐标
    var strokeColor: RGBA
    var lineWidth: CGFloat
    var text: String?            // kind == .text
    var fontSize: CGFloat
}
```

- 渲染：AnnotationLayer 为每个 Annotation 建 CAShapeLayer/CATextLayer；马赛克特殊——对截图 crop 区域应用 CIFilter.pixellated 后生成 CALayer。
- 编辑交互：拖拽创建；选中后可拖顶点改形、⌘Z 撤销（undo 栈即标注数组的差分）。
- 导出：CGContext 按 scale 创建 → 画截图 crop → 遍历渲染标注层 → CGImage。

### 6.6 输出

- **剪贴板**：NSPasteboard 同时写 PNG 与 TIFF 表示（兼容 Finder/预览/微信等目标）。
- **保存**：NSSavePanel，默认目录 ~/Pictures/AuraShot/，文件名 yyyyMMdd-HHmmss.png（格式偏好可配置，M2）。
- **贴图（M3）**：NSPanel（nonactivatingPanel，level 为 floating），支持缩放、透明度、关闭快捷键；多贴图共存。

## 7. OCR 解耦协议与接入路线

### 7.1 Swift 侧协议（M0 即定型，UI 只依赖它）

```swift
/// OCR 结果块。rect 一律为图像像素坐标（CGImage 空间），
/// 与设备无关；输出层负责按 backingScaleFactor 换算。
struct OcrBlock {
    enum Kind { case text, title, table, formula, figure }
    var rect: CGRect
    var text: String
    var kind: Kind
    var confidence: Float
}

protocol OcrEngine: Sendable {
    /// 识别整图，返回按阅读顺序排列的块。
    func recognize(_ image: CGImage) async throws -> [OcrBlock]
}

/// M0–M3 占位实现：返回空。接 mllm 后被 PaddleOcrEngine 替换。
struct NullOcrEngine: OcrEngine {
    func recognize(_ image: CGImage) async throws -> [OcrBlock] { [] }
}
```

### 7.2 桥接层（M4）

- cpp/pl/mllm 编译为静态库；新增一个 objc_library 桥接目标，暴露 ObjC 类 ACOcrCore，核心方法即 recognizeImage:completion:。
- Swift 侧 PaddleOcrEngine 实现 OcrEngine 协议，包装 ACOcrCore，负责坐标换算与 JSON 到 OcrBlock 的解析。
- 模型文件：PaddleOCR-VL GGUF（mmproj + LLM 两件套），ModelDownloader 下载至 Application Support，校验 SHA256，mmap 加载（mllm 的 WeightEntry 设计已支持）。

### 7.3 交互（M4 差异化功能）

- OCR 完成后在截图浮层上铺透明 TextOverlayView：NSTrackingArea hover 命中 bbox → 高亮；单击拷贝该块文本；⌘拖拽跨块框选拼接。
- 工具条新增"复制全部文本"（按阅读顺序拼接 markdown）。
- 表格/公式块右键导出 CSV / LaTeX（PaddleOCR-VL 结构化输出的直接收益）。

## 8. 里程碑

| 里程碑 | 内容 | 验收标准 |
|---|---|---|
| M0 骨架 | App 壳、菜单栏、Carbon 快捷键、TCC 引导、install.sh 链路跑通 | 安装后按 ⌘⇧X 可唤起空 overlay 并 Esc 退出 |
| M1 截图核心 | 区域选区、窗口磁铁吸附、尺寸 HUD、复制/保存 | 双显示器 + 混合 Retina 下坐标无偏移；吸附命中正确 |
| M2 标注 | 矩形/箭头/文字/马赛克、undo、偏好设置（快捷键/保存路径） | 标注导出与屏幕所见一致；⌘Z 可用 |
| M3 贴图 | Pin 窗口、缩放/透明度 | 贴图置顶、多贴图共存 |
| M4 OCR | 桥接 mllm、模型下载器、hover 选字、整图文本 | 截图→选字→粘贴全链路 < 3s（Metal 后端目标） |

M0 的安装命令：

```bash
bazel run //swift/pl/aurashot:install
```

## 9. 风险与开放问题

1. **TCC 开发迭代成本**：已知解法（aurabar 的稳定证书链路），风险低但需首日跑通。
2. **SCScreenshotManager 边界**：全屏游戏/受 DRM 内容（如部分视频 App）返回黑帧——属系统行为，fallback CGWindowListCreateImage 同样受限，不做特殊处理，文档注明即可。
3. **跨屏选区**：overlay 跨屏拼接交互复杂，M1 先单屏；待真实需求验证。
4. **OCR 性能**：PaddleOCR-VL 在 CPU 后端对大截图可能 >5s，依赖 mllm 的 Metal 后端建设进度；若 M4 时 Metal 未就绪，先以"整图识别 + 结果面板"形式交付（不阻塞 hover 选字的设计，仅体验降级）。
5. **开放问题**：① 截图历史（Xnip 无，TextSniper 无，但可能很香）是否做；② 贴图是否支持标注二次编辑；③ 模型是否提供"快速模式"（更小模型 / 系统 Vision 兜底）。
