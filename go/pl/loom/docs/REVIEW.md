# Loom 代码 Review 报告与修复跟踪

> 生成时间：2026-07-28。每个问题的修复状态见「状态」列；修复均采用「先写失败用例复现 → 修复 → 用例验证收住」的流程。

## 一、高严重度 Bug

| #   | 位置                                                       | 问题                                                                                                                                                                                                      | 状态      |
| --- | ---------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| H1  | `agent/compact.go:271,403` ↔ `session/sqlite_store.go:783` | compaction 的 mask/archive 把 artifact ID 只写进占位文本，从不写入 `ContentPart.Artifact` 字段，GC 引用收集（`checkpointArtifactRefs`）看不到 → `loom gc` 会误删活跃会话的压缩产物，transcript 留下死指针 | ✅ 已修复 |
| H2  | `domain/limits.go:103-104`                                 | `Usage.WallTime`/`Usage.CostUSD` 从无生产代码写入，`MaxWallTime`(30min)/`MaxEstimatedCostUSD`(5.0) 两条 runaway 硬限制永远不触发                                                                          | ✅ 已修复 |
| H3  | `app/controller.go:863`（写）vs `:787,:1053`（读）         | `c.runID` 由 turn goroutine 持锁写、Run 循环 goroutine 无锁读，存在 data race（`-race` 可复现）                                                                                                           | ✅ 已修复 |
| H4  | `agent/run.go:1222-1231`                                   | 审批流「先 flushEvents 发布 `approval.requested`，后 `RequestApproval` 注册 pending 槽」；窗口期内前端应答会因 binding 不匹配被拒，决策永久丢失（目前靠 UI 700ms guard 隐式掩盖）                         | ✅ 已修复 |
| H5  | `tool/builtin/rg.go:59` + `process/sandbox_linux.go:23`    | `rgAvailable` 只探测 rg 二进制存在；Linux 沙箱未实现（fail-closed）时 `runner.Run` 必败，search/glob 整体报废且不回退 Go fallback                                                                         | ✅ 已修复 |
| H6  | `config/resolve.go:322-345`                                | 模型级 `wire_api` 只校验并写入元数据，`buildProvider` 只用 provider 级 wireAPI 构建唯一实例 → 如 `deepseek-reasoner` 配 `wire_api: responses` 静默失效                                                    | ✅ 已修复 |
| H7  | `process/runner.go:210-215`                                | `cmd.Wait()` 返回后立即 `closeReadPipe`，管道缓冲区尾部数据可静默丢失且不标 `Truncated`                                                                                                                   | ⬜ 未修复 |

## 二、中严重度 Bug

| #   | 位置                                          | 问题                                                                                                                                           | 状态      |
| --- | --------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| M1  | `agent/goal.go:91-95` vs `:197`               | goal 置 `budget_limited` 后 `update_goal complete` 被静默丢弃（仅 `Active` 时应用 Close），工具结果却返回 applied → 模型收到成功反馈但状态未变 | ✅ 已修复 |
| M2  | `agent/run.go:1232-1237,1289-1295`            | 审批请求失败、执行前 flush 失败只 `return err`，Run 停留 active 无终态事件；与 `callModel` 的 `terminate(OutcomeFailed)` 语义不一致            | ✅ 已修复 |
| M3  | `model/anthropic/provider.go:91-94`           | `base_url` 以 `/v1` 结尾时拼出 `/v1/v1/messages`（openai 侧已处理 `/v1` 后缀，两边不一致）                                                     | ✅ 已修复 |
| M4  | `model/sse/sse.go:85-101`                     | 无冒号行/未知字段直接报错，违反 SSE 规范（应忽略），网关新增字段即整条流失败；`eventName` 在无 data 事件被跳过时泄漏给下一事件                 | ✅ 已修复 |
| M5  | `model/anthropic/provider.go:571`             | 未知 SSE 事件名 `default` 报错——Anthropic 新增事件类型即炸，应忽略/降级 warning                                                                | ✅ 已修复 |
| M6  | `model/openai/provider.go:209-239`            | Responses API 路径静默丢弃 `temperature`（chat 路径会发送），同一配置两种行为且无提示                                                          | ✅ 已修复 |
| M7  | `tool/builtin/rg.go:96-105`                   | `runRipgrep` 丢弃 `Truncated` 信号：输出超限时 search 对拼接半行 JSON 解析失败整体报错；glob 产生接缝假路径且 `truncated=false`                | ✅ 已修复 |
| M8  | `tool/builtin/rg.go:130`                      | rg 未传 `--max-columns`，单行超 1MB（minified JS）时 scanner token-too-long 整个 search 失败                                                   | ✅ 已修复 |
| M9  | `tool/command/run_cmd.go:237-238`             | `buildApprovalDesc` 在 `ArgsHash` 赋值之前调用 → 审批描述里的 args_hash 永远走兜底（算法不同），与权限事件记录对不上，审计失效                 | ✅ 已修复 |
| M10 | `process/runner.go:462-467`                   | 沙箱模式下模型传的 `env` 被 allowlist 静默丢弃，工具描述未说明，模型无法察觉会反复重试                                                         | ⬜ 未修复 |
| M11 | `workspace/path.go:232-255`                   | `AtomicWrite` hash 复检与 rename 间 TOCTOU 窗口：新建文件场景下窗口内出现的同名文件被无提示覆盖                                                | ⬜ 未修复 |
| M12 | `ui/update.go:1341-1358`                      | `handleEventsClosed`：重订阅计数成功后永不复位（累计断 3 次输入永久锁死）；重订阅丢弃 unsubscribe 句柄，旧订阅泄漏到进程结束                   | ✅ 已修复 |
| M13 | `cmd/loom/main.go:691-704`                    | `consoleApprover` 每次调用泄漏一个 stdin 读取 goroutine，且 `bufio.Reader` 重建吞预输入                                                        | ✅ 已修复 |
| M14 | `ui/update.go:192-195`                        | 任意 `tea.Msg`（按键/spinner tick）都触发 `layout()` 全量重建 transcript 字符串，streaming 高频下 CPU 可感知                                   | ⬜ 未修复 |
| M15 | `tool/webfetch/webfetch.go:382-392`           | `truncateAtBoundary` 按字节截断可切断 UTF-8（`boundedHeadTailString` 已有正确示范）                                                            | ✅ 已修复 |
| M16 | `tool/builtin/search.go:261-268`              | Go fallback 静默忽略 `glob/type/no_ignore` 参数，结果集包含模型明确排除的文件且无提示                                                          | ⬜ 未修复 |
| M17 | `permission/policy.go:62`、`rules.go:384-397` | permission 硬编码 `"run_cmd"` 名字 + 独立 struct 重解析 canonical JSON，三处契约靠人肉保持一致，改名即静默降级                                 | ⬜ 未修复 |

## 三、冗余代码

| #   | 位置                                                                                          | 问题                                                                                                                                                                                                                 | 状态                                                                  |
| --- | --------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| R1  | `runtimeevent/aggregator.go`（全文件 226 行）                                                 | 是 `agent/stream_hooks.go` 的过期副本，除自身测试外零调用方，且不支持 reasoning 事件、无 malformed-arguments 兜底，已行为漂移                                                                                        | ✅ 已删除                                                             |
| R2  | `session/sqlite_store.go:233-303` vs `307-387`                                                | `AppendEvents` 与 `AppendEventsAndCheckpoint` 约 70 行事务骨架完全重复，应抽 `appendEventsTx`                                                                                                                        | ⬜ 未处理                                                             |
| R3  | 7 个工具包的 baseTool 骨架                                                                    | `prepareCall/verifyPreparedCall/signPrepared/decodeStrict/errorResult` 等逐字复制且已漂移（gittools `errorResult` 多分支、skillread 丢 context.Canceled 映射）                                                       | ⬜ 未处理                                                             |
| R4  | `workspace/path.go:307`、`builtin/common.go:59`、`gittools/common.go:54`、`lint/common.go:41` | 敏感路径清单重复 4 份（skillread 唯一正确复用 `workspace.IsSensitive`）                                                                                                                                              | ⬜ 未处理                                                             |
| R5  | anthropic vs openai provider                                                                  | `toolResultContent`/`messageText`/schema 解码/Stream 骨架/read-error 收尾大段重复，建议抽 `model/wireutil`                                                                                                           | ⬜ 未处理                                                             |
| R6  | `app/controller.go:262-564`                                                                   | 11 个公开方法共享 ~20 行 RPC 样板（约 200 行），抽 `call(ctx, cmd)` 私有助手                                                                                                                                         | ⬜ 未处理                                                             |
| R7  | `channel_approver.go` vs `channel_questioner.go`                                              | 注册-等待-删除、DenyAll/SkipAll 近乎对称，可抽泛型 `pendingHub[K,V]` 并统一注册/发布顺序                                                                                                                             | ⬜ 未处理                                                             |
| R8  | ui 层                                                                                         | 三组 picker 窗口化逻辑相同；`formatTokens`≡`humanizeTokens`；`reasoningDialLabel`≡`describeReasoning`；预览常量双端各一份                                                                                            | ⬜ 未处理                                                             |
| R9  | 多处                                                                                          | 死代码/死配置：`CanTerminate` 恒 true；`MaxParallelTools`/`MaxRepeatedActions` 无消费方；`ControllerStateFatal` 从未赋值；`Message.MarshalJSON` 无操作；`contentResult` 未使用；edit 包 `hashBytes`/`sha256Hex` 双份 | 🔶 部分处理（MarshalJSON/contentResult/hashBytes 已删；其余留待后续） |
| R10 | 5 处                                                                                          | 字节截断切 UTF-8：`compact.go:149`、`goal.go:261`、`prompt.go:405`、`render/diff.go:142`、`webfetch.go:386`（应统一 rune 边界截断）                                                                                  | ⬜ 未处理                                                             |

## 四、抽象不合理

| #   | 位置                           | 问题                                                                                                                                                           | 状态      |
| --- | ------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| A1  | `agent/run.go:632-712`         | `Loop` 是 30+ 公有字段依赖袋，无构造器校验（Registry/Model 为 nil 运行期才炸）                                                                                 | ⬜ 未处理 |
| A2  | permission ↔ command           | 靠字符串契约耦合（见 M17），建议 `domain.PreparedCall` 增加类型化 `ExecArgv` 字段并纳入签名                                                                    | ⬜ 未处理 |
| A3  | ui → app                       | UI 直接依赖具体类型 `*app.Controller` 与 `app.ControllerState`，无法脱离完整 app 装配单测；建议窄接口                                                          | ⬜ 未处理 |
| A4  | `model/stream/stream.go:33-43` | `Emitter` 的 false-中止契约无人遵守（anthropic pump 全部丢弃返回值），契约是假的                                                                               | ⬜ 未处理 |
| A5  | `tool/command/run_cmd.go:631`  | `classifyRunError` 对 process 包错误文本做子串匹配，改文案即静默失灵，应导出 sentinel errors                                                                   | ⬜ 未处理 |
| A6  | `tool/gittools/common.go:422`  | 绕过 `process.Runner` 直接 exec：不经沙箱、无进程组隔离、限流/超时逻辑平行重复                                                                                 | ⬜ 未处理 |
| A7  | 注释与实现不符                 | `run.go:1811,1823`「ArgsHash HMAC」实为截断 SHA-256 且 Execute 不校验；`policy.go:89`「root-owned」含 `/usr/local/bin`、`/opt/homebrew/bin`（均非 root-owned） | ⬜ 未处理 |
| A8  | `domain/errors.go:80`          | `domain.As` 重复实现标准库且语义有偏差（As 返回 false 不再 unwrap、不支持 `Unwrap() []error`），建议删除改用 `errors.As`                                       | ✅ 已修复 |
| A9  | `domain/context.go:85-97`      | `ContextManifest` 半数字段无生产者，YAGNI 过度设计                                                                                                             | ⬜ 未处理 |
| A10 | `render/jsonl.go:89`           | 注释称 encode 失败写 stderr，实际写 stdout 污染协议流，且 `err.Error()` 未 JSON 转义可产出非法 JSONL                                                           | ⬜ 未处理 |

## 五、修复记录

> **回归验证（2026-07-28）**：`bazel test //go/pl/loom/...` 31/31 通过；`go test -race ./internal/app/` 通过；`go vet ./...`、`gofmt -l .` 干净。

### H1 compaction artifact 引用未登记（2026-07-28 修复）

- **方案**：① mask 时在 `ToolResult.Content` 追加 `PartArtifact` 引用（两个 provider 渲染时均跳过 PartArtifact，wire 不变）；② archive marker 消息只能携带 text part，故将「被归档 span 的全部引用 + 归档 artifact 本身」JSON 编码进 `Metadata[domain.MetadataCompactedArtifacts]`；③ domain 新增 `Message.ArtifactRefs()` 与元数据键常量，`checkpointArtifactRefs` 改为消费两者。
- **复现用例**：`agent/compact_test.go: TestCondenseMaskRegistersArtifactRef`、`TestCondenseArchiveMarkerCarriesArtifactRefs`；`session/sqlite_store_test.go: TestSQLiteStoreTracksCompactionMetadataArtifactRefs`（端到端：SaveCheckpoint → ListArtifactRefs）。
- **验证**：`go test ./internal/agent/ ./internal/session/ ./internal/domain/` 全绿。

### H3 controller c.runID data race（2026-07-28 修复）

- **方案**：`handleSteer`/`handleCancelTurn`/`handleSubmitPrompt` 发布事件前在 `c.mu` 下快照 `sessionID/runID/turnCounter` 三元组，与既有正确范式（`onTurnFinished`）对齐。
- **复现用例**：`app/controller_test.go: TestControllerPublishPathsDoNotRaceRunID`（slowModel 保持 turn 存活，30 轮 steer/cancel 轰炸），修复前 `-race` 稳定报 `controller.go:863` 写 vs `:787` 读。
- **验证**：`go test -race ./internal/app/` 全绿。

### H4 审批决策先于 pending 槽注册到达（2026-07-28 修复）

- **方案**：`ChannelApprover` 新增 early-decision 缓存（FIFO 上限 16）：`ResolveApproval` 在无 pending 槽时缓存决策并返回 true；`RequestApproval` 注册前先消费 binding 精确匹配的缓存决策（不匹配则丢弃并正常阻塞）。不改动 loop 的「先发布审计事件再阻塞等待」顺序。
- **复现用例**：`app/channel_approver_test.go: TestChannelApproverDecisionBeforeRegistration`（先 Resolve 后 Request）、`TestChannelApproverEarlyDecisionBindingMismatch`（篡改 hash 的早决策不得满足请求）。
- **验证**：`go test -race ./internal/app/` 全绿。

### H5 rg 在沙箱不可用时整体报错（2026-07-28 修复）

- **方案**：`rg.go` 新增 `isSandboxFailure`（识别 `ErrSandboxUnavailable`/`ErrSandboxRequired`）；search/glob 的 rg 执行路径遇到该类错误时降级到 Go fallback 引擎，输出 `engine: go_fallback`。
- **复现用例**：`builtin/builtin_test.go: TestSearchFallsBackWhenSandboxUnavailable`、`TestGlobFallsBackWhenSandboxUnavailable`（fakeRgRunner 返回沙箱错误，断言引擎降级且结果正确）。
- **验证**：`go test ./internal/tool/builtin/` 全绿。

### H6 模型级 wire_api 覆盖被忽略（2026-07-28 修复）

- **方案**：`ResolvedProvider` 新增 `WireModels map[string]domain.Model`（按 wire_api 预构建实例）与 `ModelFor(modelName)`；resolve 时为每个 distinct wire_api 构建实例；controller 与 headless main 改用 `ModelFor(current.Model)`。
- **复现用例**：`config/config_test.go: TestResolveModelWireAPIOverrideBuildsDistinctInstance`（twoProviderYAML 中 deepseek-reasoner 配 responses，断言返回不同实例）。
- **验证**：`go test ./internal/config/ ./internal/app/ ./cmd/...` 全绿。

### M1 budget_limited goal 无法关闭（2026-07-28 修复）

- **方案**：`drainGoalUpdates` 的 Close 分支放宽到 `Active || BudgetLimited`（wrap-up 提示本就允许模型在此时 complete）。
- **复现用例**：`agent/run_test.go: TestGoalBudgetLimitedCanBeCompleted`（端到端：budget 耗尽 → wrap-up turn 调用 update_goal complete → 断言状态 Complete）。
- **验证**：`go test ./internal/agent/ -run TestGoal` 全绿。

### M2 失败路径不 terminate Run（2026-07-28 修复）

- **方案**：统一语义——Execute 入口/循环末尾/callModel 预请求/awaitApproval/executeTools 的 flush 失败与审批请求失败，全部 `terminate(OutcomeFailed)` 后再返回错误（terminate 用 detached ctx 持久化终态事件，store 故障时至少内存态终态）。
- **复现用例**：`agent/run_test.go: TestLoopApprovalRequestFailureTerminatesRun`（errorApprover）、`TestLoopExecutionFlushFailureTerminatesRun`（failOnExecutionStartStore 在执行开始事件落盘时注入故障）。
- **验证**：`go test ./internal/agent/` 全绿。

### M3 anthropic base_url /v1 后缀（2026-07-28 修复）

- **方案**：端点拼接识别 `/v1` 后缀（补 `/messages`），不再产生 `/v1/v1/messages`。
- **复现用例**：`anthropic/provider_test.go: TestNewHandlesV1SuffixInBaseURL`（httptest 断言请求路径）。

### M4 SSE 解析过硬 + eventName 泄漏（2026-07-28 修复）

- **方案**：按 WHATWG 规范——无冒号行按「字段名+空值」处理、未知字段忽略、空行分发后重置事件名缓冲。更新了两个断言旧行为的测试（`TestParserRejectsMalformedLine`/`TestParserRejectsUnknownField` 合并为 `TestParserToleratesBareLineAndUnknownField`）。
- **复现用例**：`sse/sse_test.go: TestParserToleratesUnknownFieldsAndBareLines`、`TestParserResetsEventNameAfterEmptyDispatch`。

### M5 anthropic 未知事件类型报错（2026-07-28 修复）

- **方案**：事件分发 `default` 分支改为忽略（前向兼容）。
- **复现用例**：`anthropic/provider_test.go: TestStreamIgnoresUnknownEventTypes`。

### M6 Responses API 丢弃 temperature（2026-07-28 修复）

- **方案**：`marshalResponsesRequest` 补充 `temperature` 字段（非零时），与 chat 路径对齐。
- **复现用例**：`openai/provider_test.go: TestMarshalResponsesRequestIncludesTemperature`。
- **验证（M3–M6）**：`go test ./internal/model/...` 全绿。

### M9 run_cmd 审批描述 args_hash 与审计不符（2026-07-28 修复）

- **方案**：先签名后拼描述；desc 展示真实 HMAC ArgsHash 的前 12 位（`approvalDescHashPrefixBytes`），删除不同算法的 `shortArgsHash` 兜底。
- **复现用例**：`command/run_cmd_test.go`（`TestRunCmdToolSuccessAndNonZeroExit` 中新增断言 desc 携带签名前缀）。
- **验证**：`go test ./internal/tool/command/` 全绿。

### M7 rg 截断未传播 + 接缝假数据（2026-07-28 修复）

- **方案**：① `runRipgrep` 返回 runner 的 `StdoutTruncated`；② search 的 `decodeRgEvents` 容错跳过接缝坏行并标记 partial，输出 `Truncated` 取三者之或；③ glob 用新增的 `trimPreviewSeam`（基于 process 导出的 `PreviewSeamOffset`，与 streamCollector 3/8 头尾切分同源）丢弃接缝两侧半行，并传播 `Truncated`。
- **复现用例**：`builtin/builtin_test.go: TestSearchRipgrepToleratesSeamCorruption`、`TestGlobRipgrepPropagatesTruncation`、`TestTrimPreviewSeamDropsPartialLines`。
- **验证**：`go test ./internal/tool/builtin/ ./internal/process/` 全绿。

### M8 rg 未传 --max-columns（2026-07-28 修复）

- **方案**：`rgCommonArgs` 增加 `--max-columns 262144`（256KB，含转义后仍远低于 scanner 1MB token 上限）。
- **复现用例**：`builtin/builtin_test.go: TestRgCommonArgsIncludesMaxColumns`。

### M12 UI 重订阅计数不复位 + 订阅泄漏（2026-07-28 修复）

- **方案**：① 任何成功送达的事件将 `resubscribes` 归零（计数语义改为「连续失败」）；② `Model` 持有 `unsubscribeEvents`，`handleEventsClosed` 重订阅前释放旧订阅，`StartTUI` 退出时释放最终模型的当前订阅（broker unsubscribe 幂等）。
- **复现用例**：`ui/ui_test.go: TestRuntimeEventResetsResubscribeBudget`、`TestHandleEventsClosedReleasesOldSubscription`。
- **验证**：`go test ./internal/ui/` 全绿。

### M13 consoleApprover goroutine 泄漏（2026-07-28 修复）

- **方案**：改为「单 reader goroutine + 行 channel」模型（`sync.Once` 启动，stdin EOF 退出），同一 stream 上的预输入按序送达；`start(io.Reader)` 可注入便于测试。
- **复现用例**：`cmd/loom/main_test.go: TestConsoleApproverSharedReaderPreservesBufferedInput`（预输入 y/n 按序消费 + EOF deny）、`TestConsoleApproverAwaitAnswerRespectsCancellation`。
- **验证**：`go test ./cmd/loom/` 全绿。

### M15 webfetch 字节截断切 UTF-8（2026-07-28 修复）

- **方案**：`truncateAtBoundary` 在行边界回退后再做 UTF-8 有效性回退（逐字节退到合法 rune 边界）。
- **复现用例**：`webfetch/webfetch_test.go: TestTruncateAtBoundary` 新增 CJK 切分用例。
- **验证**：`go test ./internal/tool/webfetch/` 全绿。

### 冗余/死代码清理（2026-07-28）

- **R1**：删除 `runtimeevent/aggregator.go` + `aggregator_test.go`（agent/stream_hooks.go 的过期副本，零调用方），同步更新 BUILD。
- **R9（部分）**：删除无操作 `Message.MarshalJSON`、未使用的 `command.contentResult` 与 `builtin.defaultSearchContextLines`、edit 包重复的 `hashBytes`（统一 `sha256Hex`）。`CanTerminate`/`MaxParallelTools`/`MaxRepeatedActions`/`ControllerStateFatal` 涉及语义决策（实现对应能力 vs 删除配置项），留待后续。
- **A8**：删除 `domain.As`（语义偏离标准库），全部 18 处调用点改为 `errors.As`，`TestAsAgentError` 改为 `TestAgentErrorUnwrapsWithErrorsAs`。
- **验证**：`go build ./...`、`go vet ./...`、`gofmt -l .` 干净，`go test ./...` 全绿。

### H2 WallTime/CostUSD 从不更新（2026-07-28 修复）

- **方案**：① `Run` 新增 `turnStartedAt`（NewRun/RestoreRun/ResetUsageForNewTurn 锚定），`touchWallTime()` 在 `CheckBudget`/新增 `CheckRunaway()`/token 记账处折叠已逝窗口；② `Loop` 新增 `CostInputUSDPerMTok`/`CostOutputUSDPerMTok`，token 记账（抽取 `accountUsage`，callModel 与 summarizeForCompaction 共用）按费率折算 `CostUSD`；③ 费率从 tracing 配置（`cost_input_usd_per_mtok` 等）接入 controller 与 headless main，未配置时成本记账关闭。
- **复现用例**：`agent/run_test.go: TestRunWallTimeCountsTowardBudget`（FakeClock 推进 2min 触发硬 breach，重置窗口后清除）、`TestLoopExecuteCostBudgetExhausted`（2M tokens × $1/MTok 超过 $1 限额 → budget_exhausted）。
- **验证**：`go test ./internal/agent/ ./internal/app/ ./cmd/...` 全绿。
