# Loom Neovim 风格 UI 设计

> 状态：Draft（Phase 1/2 已实现；Phase 3 合成器与 help/question 浮窗已实现）  
> 日期：2026-07-31  
> 前置文档：`docs/TUI_DESIGN.md`

## 1. 背景与动机

Loom TUI 的第一版（`TUI_DESIGN.md`）交付了常驻对话所需的全部基础：模式驱动的状态机（`ModeChat / ModeSearch / ModeApproval / …`）、Block 化的 Transcript、三级渲染缓存（glamour 缓存、block 指纹缓存、`syncTranscript` 版本跳过）。交互层则沿用了 Claude Code 风格的 Ctrl 系快捷键与"替换式"面板。

本设计的目标是把交互层迁移到 Neovim 的交互范式，参考 `snacks.nvim` 的组件模型：

1. **Picker 统一抽象**：snacks.picker 的 input（fuzzy 过滤）+ list + preview 三窗格是选择器类交互的最优解。现状三个 picker（`SessionPicker` / `ModelPicker` / `ReasoningPicker`）是近乎复制粘贴的三份代码，无过滤、无预览。
2. **快捷键与 vim 对齐**：j/k 移动、`/` 搜索、`gg/G` 跳转、Esc 分层退出。现状 Ctrl 系绑定（`Ctrl+E/R/O/F/Y/G`）与 vim 原生语义几乎全部冲突，且不可配置。
3. **浮窗（floating window）**：help / approval / question / picker 以浮窗形式叠加在 Transcript 之上，而非替换主区域。

### 1.1 现状基础（有利条件）

- UI 已是**模式驱动**（`Mode` 枚举），与 vim 的 modal 哲学同构；picker 内已有 j/k；approval 已有 `y/a/t/n` 快速键。
- 渲染管线的性能优化全部是**内容寻址缓存**，与渲染目标（inline / alt-screen、替换式 / 浮窗）解耦，重构不会触碰它们。
- `ui.alt_screen` 配置项与 `tea.WithAltScreen()` 接线已存在。

### 1.2 现状差距

- 无 keymap 抽象：快捷键硬编码在 `update.go` 的 switch 中，不可配置、无法演进。
- 无统一 picker：三份复制代码，无 fuzzy filter、无 preview。
- 无图层概念：`View()` 用 if-else 让各模式互斥地占据主区域，无法叠加。

## 2. 目标与非目标

### 2.1 目标

1. **Keymap 抽象层**（Phase 1）：所有可迁移的快捷键收敛为「上下文 → 按键 → 动作」绑定表；`config.yaml` 的 `ui.keymap` 可覆盖默认绑定；非法覆盖被忽略并给出告警，绝不因配置错误崩溃。
2. **统一 Picker 组件**（Phase 2）：一个泛型 `Finder[T]` 承载 fuzzy 过滤、预览窗格、vim 双模式（insert/normal）导航；Session/Model/Reasoning 三个 picker 迁移到其上。
3. **浮窗合成器**（Phase 3，本文仅定方向）：alt-screen 模式下 help/approval/question/picker 改为真浮窗；inline 模式保持替换式布局。
4. **Transcript vim 导航**（Phase 4，本文仅定方向）：`gg/G/Ctrl+D/Ctrl+U/`、`n/N`；Composer 保持 insert-first，不做 modal 编辑。
5. **Leader key + which-key**（Phase 5，本文仅定方向）。

### 2.2 非目标

- Composer 的 normal/insert 双模式编辑。聊天输入 99% 的时间在打字，`Esc` 进 normal 再 `i` 回来对聊天场景是净损耗。Composer 永远 insert-first。
- inline 模式下的真浮窗。inline renderer 的行跟踪对帧高度敏感（见 `view.go` 首帧注释与 `frame_stability_test.go`），浮窗只在 alt-screen 下启用。
- 鼠标交互的重新设计（保持现状）。
- 主题系统重构。

## 3. 设计原则

1. **缓存纪律高于一切**：任何新组件（picker、浮窗合成器）只消费「渲染好的字符串」，不得触发 `syncTranscript` 或 `renderCache` 的失效。浮窗盖住 Transcript 时只做几何裁剪，不重渲被盖内容。该纪律用测试锁死（见 §8）。
2. **默认值即现状，演进走配置**：Phase 1 的默认绑定与现有快捷键完全一致，老用户零感知；vim 化的新绑定通过 `ui.keymap` 或后续 Phase 逐步引入。
3. **vim 语义只施加于浏览域**：Transcript 滚动、搜索、picker 导航可以完全 vim 化；文本输入域（Composer、picker 过滤框、question 自由文本行）永远是 insert-first。
4. **渐进可交付**：每个 Phase 独立可发布，不依赖后续 Phase；Phase 1/2 不依赖 alt-screen 决策。
5. **可测试优先**：keymap 解析、finder 过滤/排序/窗口化均为纯函数式组件，确定性单测覆盖（延续 `ChoiceList` 的 frontend-agnostic 风格）。

## 4. 总体路线图

| Phase | 内容 | 依赖 | 状态 |
|-------|------|------|------|
| 1 | Keymap 抽象层 + `ui.keymap` 配置 | 无 | 本次实现 |
| 2 | 统一 `Finder[T]`（fuzzy + preview + vim 双模式），替换三个 picker | Phase 1（picker 键走 keymap） | 本次实现 |
| 3 | 图层合成器；help/question/picker 浮窗化（alt-screen 限定）；approval 保持全宽 band | Phase 1 | 部分实现 |
| 4 | Transcript vim 导航（`gg/G/Ctrl+D/Ctrl+U/`、`n/N`） | Phase 1 | 设计方向已定 |
| 5 | Leader key + which-key 延迟提示 | Phase 1/4 | 设计方向已定 |

## 5. Phase 1：Keymap 抽象层

### 5.1 模型

```go
// KeyContext 是键位生效的 UI 上下文，与 Mode 对应但粒度更粗：
// 三个 picker 模式共享 ContextPicker。
type KeyContext string

const (
    ContextChat     KeyContext = "chat"
    ContextPicker   KeyContext = "picker"
    ContextApproval KeyContext = "approval" // 预留，Phase 3 迁移
    ContextSearch   KeyContext = "search"   // 预留
)

// Action 是可绑定的抽象动作；处理函数持有动作语义，绑定表只决定
// 哪个键触发它。
type Action string

const (
    // ContextChat（全局查看动作；结构性按键见 §5.4）
    ActionToggleReasoning  Action = "toggle_reasoning"   // ctrl+r
    ActionToggleToolOutput Action = "toggle_tool_output" // ctrl+e
    ActionToggleAllTools   Action = "toggle_all_tools"   // ctrl+o
    ActionTogglePlan       Action = "toggle_plan"        // ctrl+t
    ActionSearchTranscript Action = "search_transcript"  // ctrl+f
    ActionViewSubagent     Action = "view_subagent"      // ctrl+g
    ActionCopyLastReply    Action = "copy_last_reply"    // ctrl+y
    ActionJumpToBottom     Action = "jump_to_bottom"     // ctrl+end

    // ContextPicker
    ActionCursorUp   Action = "cursor_up"   // up, ctrl+k
    ActionCursorDown Action = "cursor_down" // down, ctrl+j
    ActionConfirm    Action = "confirm"     // enter
    ActionClose      Action = "close"       // esc（normal 模式）
)
```

`Keymap` 持有 `map[KeyContext]map[keyString]Action`，`Lookup(ctx, tea.KeyMsg)` 用 `msg.String()`（bubbletea 的规范化键名，如 `ctrl+r`、`shift+tab`、`enter`）查表。

### 5.2 配置覆盖

`config.yaml`：

```yaml
ui:
  keymap:
    chat:
      search_transcript: "ctrl+s"   # action: key，覆盖默认
      copy_last_reply: "ctrl+p"
    picker:
      close: "ctrl+c"
```

- schema：`config.UI` 增加 `Keymap map[string]map[string]string`，经 `resolved.UI.Keymap` → `ui.InitOptions.Keymap` → `StartTUI` 传入。
- 合并规则（`Keymap.WithOverrides`）：先摘除该动作在上下文中的旧绑定，再绑定新键。
- **确定性**：override 按键名字典序处理；新键已被同上下文其他动作占用时，保留先绑定者并记录 warning（map 迭代顺序不得影响结果）。
- **容错**：未知 context / 未知 action / 空键名 → 忽略并收集 warning；`StartTUI` 把第一条 warning 显示在状态栏（`statusIsError=true`），其余丢弃。配置错误永远不影响启动。

### 5.3 键名规范化

修饰键组合（含 `+`）整体小写（`Ctrl+R` ≡ `ctrl+r`）；单字符保持原样（`Q` 与 `q` 是不同的键）。解析与 `msg.String()` 的输出对齐，不另行发明键名语法。

### 5.4 迁移边界（有意保留硬编码的按键）

以下按键与 Composer 编辑逻辑深度交织，Phase 1 不进绑定表：

- `Enter` / `Alt+Enter`（提交 vs 换行，且与补全弹窗联动）
- `↑/↓`（多行草稿内移动 vs 边缘滚动 Transcript vs 补全光标）
- `Tab`（补全应用）
- `Esc`（quitConfirm → 补全关闭 → 取消 turn 的三级语义）
- `Ctrl+C` / `Ctrl+D`（取消/退出状态机）

approval / question / subagent / search 模式的内部按键同样暂不进表（上下文中预留），随 Phase 3/4 逐步迁移。

## 6. Phase 2：统一 Picker（`Finder[T]`）

### 6.1 组件模型

参考 snacks.picker 的三窗格结构，但在一个边框内纵向排布（真浮窗留给 Phase 3）：

```text
┌─ Sessions ─────────────────────────────────────────────┐
│ ❯ abc▏                                          2/42   │  input：fuzzy 过滤
├────────────────────────────┬───────────────────────────┤
│ ▶ abc123  v3 · 2h ago      │ ID:      abc123…          │  list │ preview
│   def456  v1 · 1d ago      │ Version: 3                │
│                            │ Updated: 2026-07-30 14:02 │
├────────────────────────────┴───────────────────────────┤
│ INSERT · ↑/↓/ctrl+j/k move · Enter select · Esc normal │  footer：随模式切换
└────────────────────────────────────────────────────────┘
```

```go
// FinderItem 是一条可选记录：Text 参与过滤与主显示，Hint 是行尾
// 次要信息（dim），Badge 是右对齐标记（如当前项的 ●）。
type FinderItem[T any] struct {
    Value T
    Text  string
    Hint  string
    Badge string
}

// Finder 是 frontend-agnostic 的选择器组件（同 ChoiceList 风格）：
// 宿主负责把按键翻译成方法调用，Render 输出字符串。
type Finder[T any] struct {
    title    string
    items    []FinderItem[T]
    filtered []int                 // items 下标，按匹配分排序
    query    string
    cursor   int                   // filtered 下标
    normal   bool                  // vim normal 模式（list 聚焦）
    preview  func(T) string        // nil 时隐藏 preview 窗格
    loaded   bool                  // 异步数据源（sessions）的加载态
    loadErr  error
}
```

核心方法：`TypeRune/Backspace/SetQuery`（过滤）、`MoveUp/MoveDown`（光标，clamp 在过滤结果内）、`Selected() *T`、`EnterNormal/EnterInsert/ToggleMode`、`Load(items, err)`、`Render(width, height)`。

### 6.2 Fuzzy 过滤

子序列匹配 + 打分排序（fzf 的简化版，纯函数、无依赖）：

- 不区分大小写；query 的每个字符必须按序出现在 Text 中，否则不匹配。
- 打分：连续匹配加分；词边界（字符串头、空格/`/`/`-`/`_` 之后）匹配加分；越靠前命中加分。
- 过滤结果按分数降序、分数相同按原始下标升序（稳定、确定）。
- query 为空时保持原始顺序（sessions 按更新时间、models 按配置顺序）。

### 6.3 Vim 双模式导航

snacks.picker 的模态语义，两种模式下都可用的键走 keymap（`ContextPicker`）：

| 模式 | 键 | 行为 |
|------|----|------|
| insert（默认，过滤框聚焦） | 可打印字符 | 进入过滤 query |
| insert | `↑/↓`、`ctrl+k/ctrl+j` | 移动光标（keymap） |
| insert | `Enter` | 确认选择（keymap） |
| insert | `Esc` | 进入 normal 模式（硬编码，vim 语义） |
| normal（list 聚焦） | `j/k`、`g/G`（硬编码 runes） | 移动/首行/末行 |
| normal | `i`、`a` | 回到 insert 模式 |
| normal | `q`、`Esc` | 关闭 picker（`q` 硬编码，`Esc` 走 keymap `close`） |

- 过滤导致结果集缩小时 cursor 自动 clamp；`Selected()` 永远返回过滤后的高亮项。
- footer 提示随模式切换（`INSERT …` / `NORMAL …`），这是 vim 用户的模式反馈。

### 6.4 三个数据源的迁移

`picker.go` 重写为 Finder 的三个构造器 + 各自的 preview 函数；`ModelOption` / `ReasoningLevels` / `formatTokens` / `formatTimeAgo` 保留：

| Picker | Item.Text | Item.Hint | Preview |
|--------|-----------|-----------|---------|
| Sessions（异步 `Load`） | session ID（短格式） | `v3 · 2h ago` | ID / Version / Created / Updated 完整字段 |
| Models | `provider/model` | `200k ctx · responses` | Provider / Model / Context window / Wire API |
| Reasoning | level label | 短描述 | 完整 Desc |

初始光标：models/reasoning 定位到当前生效项（沿用现有行为，`●` Badge 标记）。

### 6.5 与宿主的接线

- `Model` 字段：`picker/modelPicker/reasoningPicker` → `sessionFinder/modelFinder/reasoningFinder *Finder[T]`。
- 按键路由：Go 方法不支持泛型参数，路由收敛为一个泛型自由函数 `routeFinderKey[T](m Model, msg, f, …)`，三个模式各自的 handler 只保留 confirm 之后的业务（resume session / set model / set reasoning）。
- `sessionsLoadedMsg` 填充 `sessionFinder.Load(items, err)`；加载中/失败/空列表的占位渲染沿用现有文案。
- Slash 补全弹窗（`renderCompletion`）本次**不**迁移到 Finder：它是 Composer 的就地补全而非独立选择器，迁移价值低、风险高；作为后续候选项记录。

## 7. Phase 3–5 方向（不在本次实现）

### 7.1 Phase 3：浮窗合成器（已实现 help/question 浮窗）

已实现（`canvas.go`）：

- `Float`（内容 + 锚点坐标）与 `ComposeFloats(frame, width, height, floats...)`：先把底帧规范化到整屏行数，再把每个浮窗的行按列剪贴到覆盖区间（ANSI-aware 的 `truncateANSI`/`dropANSI`），产出恰好 `height` 行的帧。
- 渲染纪律：合成器只做字符串裁剪拼贴，**永不**调用 block/transcript 渲染函数；被盖住的内容不触发任何缓存失效。`TestFloatsDoNotRebuildTranscript` 锁死：浮窗开关 N 次 `transcriptBuilds` 不变。
- 仅 alt-screen 启用：`View()` 拆为 `renderBase()`（底帧）与 `activeFloats()`（浮窗层），`baseMode()` 把浮窗模式映射回 chat 底帧；inline 模式保持替换式布局。
- help 与 question 是居中浮窗（宽度为屏宽 3/4，上限 76）；三个 picker 是 snacks 式大浮窗（宽 4/5 屏、上限 110，高 3/5 屏、上限 26 行），底帧保持活动（transcript 与 composer 可见）；approval 维持全宽 band（其信息量大且决策依赖完整宽度）。inline 模式下上述全部保持替换式布局。
- 已知限制：背景行假定为样式自包含（lipgloss/glamour 输出均满足）；跨整行的背景填充（如 header 色带）被浮窗覆盖时右侧不补色。

后续可选：approval 浮窗化（需先解决其高度可变性）、which-key 提示复用合成器。

### 7.2 Phase 4：Transcript vim 导航

- 新增「浏览态」：Composer 空闲时 `Esc`（或 `Ctrl+[`）把焦点切到 Transcript，`i`/`a`/任意可打印字符回到 Composer。
- 浏览态绑定（走 keymap `ContextChat` 新增 action）：`j/k` 行滚、`Ctrl+D/Ctrl+U` 半页、`gg/G` 顶/底、`/` 进搜索、`n/N` 跳匹配、`Enter` 展开光标处 tool block、`q`/Esc 回到 Composer。
- 现有 `Ctrl+F` 搜索改为同时接受 `/`；搜索模式内增加 `n/N`（next/prev match）并迁移进 keymap `ContextSearch`。

### 7.3 Phase 5：Leader key + which-key

- `Space` 作为 leader（浏览态）：`<space>s` sessions、`<space>m` model、`<space>r` reasoning、`<space>h` help、`<space>a` subagent view。
- leader 按下 500ms 无后续键时，在 Composer 上方弹出候选提示（which-key 风格），复用 Phase 3 的浮窗或现有补全弹窗的渲染位。

## 8. 兼容性与测试策略

### 8.1 兼容性

- 默认键位完全不变；三个 picker 的打开方式（`/sessions`、`/model`、`/reasoning`）不变。
- picker 内的行为变化（需要说明）：过滤输入成为默认焦点，原「j/k 直接移动」变为「先 `Esc` 进 normal 再 j/k」或直接 `↑/↓`。这是向 snacks 语义对齐的有意变更，footer 与帮助文案同步更新。
- `ui.keymap` 配置错误只产生状态栏 warning。

### 8.2 测试

- `keymap_test.go`：默认绑定可查；override 重绑后旧键失效、新键生效；未知 context/action/冲突 → warning 且结果被忽略；同一 overrides 两次应用结果一致（确定性）。
- `finder_test.go`：fuzzy 打分（连续/词边界/顺序）；过滤后 cursor clamp；空 query 保序；窗口化渲染的 `↑ more`/`↓ more`；preview 窗格出现/隐藏；insert/normal 模式切换与 footer 文案。
- `picker_test.go`：三个构造器的初始光标（当前项）、`●` Badge 不泄漏、meta hint 列对齐。
- 缓存纪律（Phase 3 时补）：浮窗开关循环下 `transcriptBuilds` 不变。
