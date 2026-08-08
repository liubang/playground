# Loom 代码 Review 报告与修复跟踪

> 生成时间：2026-07-28。每个问题的修复状态见「状态」列；修复均采用「先写失败用例复现 → 修复 → 用例验证收住」的流程。

## 一、高严重度 Bug

| #   | 位置                                                       | 问题                                                                                                                                                                                                      | 状态      |
| --- | ---------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| H1  | `agent/compact.go:271,403` ↔ `session/sqlite_store.go:783` | compaction 的 mask/archive 把 artifact ID 只写进占位文本，从不写入 `ContentPart.Artifact` 字段，GC 引用收集（`checkpointArtifactRefs`）看不到 → `loom gc` 会误删活跃会话的压缩产物，transcript 留下死指针 | 已修复 |
| H2  | `domain/limits.go:103-104`                                 | `Usage.WallTime`/`Usage.CostUSD` 从无生产代码写入，`MaxWallTime`(30min)/`MaxEstimatedCostUSD`(5.0) 两条 runaway 硬限制永远不触发                                                                          | 已修复 |
| H3  | `app/controller.go:863`（写）vs `:787,:1053`（读）         | `c.runID` 由 turn goroutine 持锁写、Run 循环 goroutine 无锁读，存在 data race（`-race` 可复现）                                                                                                           | 已修复 |
| H4  | `agent/run.go:1222-1231`                                   | 审批流「先 flushEvents 发布 `approval.requested`，后 `RequestApproval` 注册 pending 槽」；窗口期内前端应答会因 binding 不匹配被拒，决策永久丢失（目前靠 UI 700ms guard 隐式掩盖）                         | 已修复 |
| H5  | `tool/builtin/rg.go:59` + `process/sandbox_linux.go:23`    | `rgAvailable` 只探测 rg 二进制存在；Linux 沙箱未实现（fail-closed）时 `runner.Run` 必败，search/glob 整体报废且不回退 Go fallback                                                                         | 已修复 |
| H6  | `config/resolve.go:322-345`                                | 模型级 `wire_api` 只校验并写入元数据，`buildProvider` 只用 provider 级 wireAPI 构建唯一实例 → 如 `deepseek-reasoner` 配 `wire_api: responses` 静默失效                                                    | 已修复 |
| H7 | `process/runner.go:210-215` | `cmd.Wait()` 返回后立即 `closeReadPipe`，管道缓冲区尾部数据可静默丢失且不标 `Truncated` | 已修复 |

## 二、中严重度 Bug

| #   | 位置                                          | 问题                                                                                                                                           | 状态      |
| --- | --------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| M1  | `agent/goal.go:91-95` vs `:197`               | goal 置 `budget_limited` 后 `update_goal complete` 被静默丢弃（仅 `Active` 时应用 Close），工具结果却返回 applied → 模型收到成功反馈但状态未变 | 已修复 |
| M2  | `agent/run.go:1232-1237,1289-1295`            | 审批请求失败、执行前 flush 失败只 `return err`，Run 停留 active 无终态事件；与 `callModel` 的 `terminate(OutcomeFailed)` 语义不一致            | 已修复 |
| M3  | `model/anthropic/provider.go:91-94`           | `base_url` 以 `/v1` 结尾时拼出 `/v1/v1/messages`（openai 侧已处理 `/v1` 后缀，两边不一致）                                                     | 已修复 |
| M4  | `model/sse/sse.go:85-101`                     | 无冒号行/未知字段直接报错，违反 SSE 规范（应忽略），网关新增字段即整条流失败；`eventName` 在无 data 事件被跳过时泄漏给下一事件                 | 已修复 |
| M5  | `model/anthropic/provider.go:571`             | 未知 SSE 事件名 `default` 报错——Anthropic 新增事件类型即炸，应忽略/降级 warning                                                                | 已修复 |
| M6  | `model/openai/provider.go:209-239`            | Responses API 路径静默丢弃 `temperature`（chat 路径会发送），同一配置两种行为且无提示                                                          | 已修复 |
| M7  | `tool/builtin/rg.go:96-105`                   | `runRipgrep` 丢弃 `Truncated` 信号：输出超限时 search 对拼接半行 JSON 解析失败整体报错；glob 产生接缝假路径且 `truncated=false`                | 已修复 |
| M8  | `tool/builtin/rg.go:130`                      | rg 未传 `--max-columns`，单行超 1MB（minified JS）时 scanner token-too-long 整个 search 失败                                                   | 已修复 |
| M9  | `tool/command/run_cmd.go:237-238`             | `buildApprovalDesc` 在 `ArgsHash` 赋值之前调用 → 审批描述里的 args_hash 永远走兜底（算法不同），与权限事件记录对不上，审计失效                 | 已修复 |
| M10 | `process/runner.go:462-467` | 沙箱模式下模型传的 `env` 被 allowlist 静默丢弃，工具描述未说明，模型无法察觉会反复重试 | 已修复 |
| M11 | `workspace/path.go:232-255`                   | `AtomicWrite` hash 复检与 rename 间 TOCTOU 窗口：新建文件场景下窗口内出现的同名文件被无提示覆盖                                                | 已修复 |
| M12 | `ui/update.go:1341-1358`                      | `handleEventsClosed`：重订阅计数成功后永不复位（累计断 3 次输入永久锁死）；重订阅丢弃 unsubscribe 句柄，旧订阅泄漏到进程结束                   | 已修复 |
| M13 | `cmd/loom/main.go:691-704`                    | `consoleApprover` 每次调用泄漏一个 stdin 读取 goroutine，且 `bufio.Reader` 重建吞预输入                                                        | 已修复 |
| M14 | `ui/update.go:192-195`                        | 任意 `tea.Msg`（按键/spinner tick）都触发 `layout()` 全量重建 transcript 字符串，streaming 高频下 CPU 可感知                                   | 已修复 |
| M15 | `tool/webfetch/webfetch.go:382-392`           | `truncateAtBoundary` 按字节截断可切断 UTF-8（`boundedHeadTailString` 已有正确示范）                                                            | 已修复 |
| M16 | `tool/builtin/search.go:261-268` | Go fallback 静默忽略 `glob/type/no_ignore` 参数，结果集包含模型明确排除的文件且无提示 | 已修复 |
| M17 | `permission/policy.go:62`、`rules.go:384-397` | permission 硬编码 `"run_cmd"` 名字 + 独立 struct 重解析 canonical JSON，三处契约靠人肉保持一致，改名即静默降级                                 | 已修复 |

## 三、冗余代码

| #   | 位置                                                                                          | 问题                                                                                                                                                                                                                 | 状态                                                                  |
| --- | --------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| R1  | `runtimeevent/aggregator.go`（全文件 226 行）                                                 | 是 `agent/stream_hooks.go` 的过期副本，除自身测试外零调用方，且不支持 reasoning 事件、无 malformed-arguments 兜底，已行为漂移                                                                                        | 已删除                                                             |
| R2  | `session/sqlite_store.go:233-303` vs `307-387`                                                | `AppendEvents` 与 `AppendEventsAndCheckpoint` 约 70 行事务骨架完全重复，应抽 `appendEventsTx`                                                                                                                        | 已处理                                                             |
| R3  | 7 个工具包的 baseTool 骨架                                                                    | `prepareCall/verifyPreparedCall/signPrepared/decodeStrict/errorResult` 等逐字复制且已漂移（gittools `errorResult` 多分支、skillread 丢 context.Canceled 映射）                                                       | ⬜ 未处理                                                             |
| R4  | `workspace/path.go:307`、`builtin/common.go:59`、`gittools/common.go:54`、`lint/common.go:41` | 敏感路径清单重复 4 份（skillread 唯一正确复用 `workspace.IsSensitive`）                                                                                                                                              | 已处理                                                             |
| R5  | anthropic vs openai provider                                                                  | `toolResultContent`/`messageText`/schema 解码/Stream 骨架/read-error 收尾大段重复，建议抽 `model/wireutil`                                                                                                           | ⬜ 未处理                                                             |
| R6  | `app/controller.go:262-564`                                                                   | 11 个公开方法共享 ~20 行 RPC 样板（约 200 行），抽 `call(ctx, cmd)` 私有助手                                                                                                                                         | ⬜ 未处理                                                             |
| R7  | `channel_approver.go` vs `channel_questioner.go`                                              | 注册-等待-删除、DenyAll/SkipAll 近乎对称，可抽泛型 `pendingHub[K,V]` 并统一注册/发布顺序                                                                                                                             | ⬜ 未处理                                                             |
| R8  | ui 层                                                                                         | 三组 picker 窗口化逻辑相同；`formatTokens`≡`humanizeTokens`；`reasoningDialLabel`≡`describeReasoning`；预览常量双端各一份                                                                                            | 已处理                                                             |
| R9  | 多处                                                                                          | 死代码/死配置：`CanTerminate` 恒 true；`MaxParallelTools`/`MaxRepeatedActions` 无消费方；`ControllerStateFatal` 从未赋值；`Message.MarshalJSON` 无操作；`contentResult` 未使用；edit 包 `hashBytes`/`sha256Hex` 双份 | 部分处理（MarshalJSON/contentResult/hashBytes 已删；其余留待后续） |
| R10 | 5 处 | 字节截断切 UTF-8：`compact.go:149`、`goal.go:261`、`prompt.go:405`、`render/diff.go:142`、`webfetch.go:386`（应统一 rune 边界截断） | 已修复（webfetch 于 M15、其余 4 处于本轮） |

## 四、抽象不合理

| #   | 位置                           | 问题                                                                                                                                                           | 状态      |
| --- | ------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| A1  | `agent/run.go:632-712`         | `Loop` 是 30+ 公有字段依赖袋，无构造器校验（Registry/Model 为 nil 运行期才炸）                                                                                 | ⬜ 未处理 |
| A2  | permission ↔ command           | 靠字符串契约耦合（见 M17），建议 `domain.PreparedCall` 增加类型化 `ExecArgv` 字段并纳入签名                                                                    | ⬜ 未处理 |
| A3  | ui → app                       | UI 直接依赖具体类型 `*app.Controller` 与 `app.ControllerState`，无法脱离完整 app 装配单测；建议窄接口                                                          | ⬜ 未处理 |
| A4  | `model/stream/stream.go:33-43` | `Emitter` 的 false-中止契约无人遵守（anthropic pump 全部丢弃返回值），契约是假的                                                                               | ⬜ 未处理 |
| A5  | `tool/command/run_cmd.go:631`  | `classifyRunError` 对 process 包错误文本做子串匹配，改文案即静默失灵，应导出 sentinel errors                                                                   | ⬜ 未处理 |
| A6  | `tool/gittools/common.go:422`  | 绕过 `process.Runner` 直接 exec：不经沙箱、无进程组隔离、限流/超时逻辑平行重复                                                                                 | ⬜ 未处理 |
| A7 | 注释与实现不符 | `run.go:1811,1823`「ArgsHash HMAC」实为截断 SHA-256 且 Execute 不校验；`policy.go:89`「root-owned」含 `/usr/local/bin`、`/opt/homebrew/bin`（均非 root-owned） | 已修复（trusted dirs 运行时校验；ArgsHash 部分经复核为 M9 修复后的误报——见修复记录） |
| A8  | `domain/errors.go:80`          | `domain.As` 重复实现标准库且语义有偏差（As 返回 false 不再 unwrap、不支持 `Unwrap() []error`），建议删除改用 `errors.As`                                       | 已修复 |
| A9  | `domain/context.go:85-97`      | `ContextManifest` 半数字段无生产者，YAGNI 过度设计                                                                                                             | ⬜ 未处理 |
| A10 | `render/jsonl.go:89` | 注释称 encode 失败写 stderr，实际写 stdout 污染协议流，且 `err.Error()` 未 JSON 转义可产出非法 JSONL | 已修复 |

### M17 permission 与工具名的字符串契约（2026-08-01 修复）

- **方案（A2 的正解）**：`domain.PreparedCall` 新增类型化 `ExecRequest{Argv, Escalated, NeedsNetwork}` 字段；`run_cmd` 与新增的 `exec_session` 在 Prepare 填充并纳入各自 HMAC 签名指纹。permission 层新增唯一入口 `ExecInfoOf`：签名过的 `ExecRequest` 为权威来源，原始 JSON 解析（`ParseRunCmdCall`）仅作为 Prepare 之外构造的调用的兜底（测试、审批 UI 边界）。RuleDecider/DangerDecider/SessionDecider/BaselineDecider 与 `RuleApprover` 全部切换到 `ExecInfoOf`，`"run_cmd"` 工具名硬编码从判定路径消除——任何填充 `ExecRequest` 的工具自动获得 argv 前缀规则、danger 拦截与 session 记忆。`RememberCall`/`ApprovalRulePreview`/`RunCmdTrustPreview` 同时覆盖 `exec_session`。
- **复现用例**：`permission/policy_test.go: TestExecRequestTypedContractPreferred`（JSON 与签名契约不一致时以契约为准）、`TestExecSessionMatchesArgvRules`（exec_session 走 argv 规则）。
- **验证**：`go test ./pl/loom/...` 全绿，`bazel test //go/pl/loom/...` 33/33。

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

### H7 进程退出后尾部输出丢失（2026-07-28 修复）
- **根因（比原判断更深一层）**：`exec.Cmd.Wait()` 本身就会在进程退出时关闭 `StdoutPipe` 的读端（官方文档："incorrect to call Wait before all reads from the pipe have completed"），runner 自己的 `closeReadPipe` 只是重复关闭——竞态无法通过调整顺序消除。
- **方案**：改用自建 `os.Pipe()`（`cmd.Stdout/Stderr = pipeW`），父进程在 Start 后放弃写端；进程组退出后读端自然 EOF，reader 排空内核缓冲后才收尾；仅当 detached 子进程持有写端导致 drain 超时（100ms）时才强制关管，并用 FIONREAD ioctl（`pipe_pending_darwin/linux.go`，x/sys 在 darwin 未导出常量故自定义）探测是否有未排空的字节，有（或探测失败）才标 `Truncated`——detached 但无遗留数据的场景不误报。
- **复现用例**：`runner_test.go: TestRunnerDrainsOutputAfterProcessExit`（slowSink 每写 2ms 让竞态 deterministic：修复前稳定丢 64KB 且 `Truncated=false`）；既有间歇失败 `TestRunCmdToolExternalizesLargeOutput` 修复后 20/20 通过。
- **验证**：`go test ./internal/process/` 全绿（含 detached stdio 既有用例），`GOOS=linux go build` 通过。

### A10 JSONL 诊断污染协议流（2026-07-28 修复）
- **方案**：`JSONL` 新增 `errOut`（默认 stderr，`WithErrorWriter` 可注入）；encode 失败写诊断到 errOut 并返回 error（durable 事件向调用方传播），不再向协议流写未转义的合成错误行。
- **复现用例**：`render/jsonl_test.go: TestJSONL_InvalidPayload`（原空占位测试改写为真实用例：协议流必须为空、errOut 有诊断、error 传播）。

### R10 剩余 4 处 UTF-8 截断（2026-07-28 修复）
- **方案**：`compact.go` 复用同包 `cutAtRuneBoundary`；`goal.go` 改用 `truncateRunes`（rune 计数）；`prompt.go` 新增本地 `cutAtRuneBoundary`；`render/diff.go` 截断前回退到 rune 边界。
- **复现用例**：`compact_test.go: TestBuildSummaryReplacementTruncatesAtRuneBoundary`、`run_test.go: TestUpdateGoalApprovalDescTruncatesAtRuneBoundary`、`prompt_test.go`（既有 CJK 超宽用例补强 UTF-8 断言）、`diff_test.go: TestTruncateDiffLineKeepsUTF8`。

### M10 沙箱 env 静默丢弃（2026-07-28 修复）
- **方案**：`buildMinimalEnv` 返回被丢弃的 override keys → `process.Result.DroppedEnvKeys`；run_cmd 输出 `note` 合并 `droppedEnvNote`（新增 `combineNotes`）；工具描述声明 allowlist 语义与 note 回显。
- **复现用例**：`runner_test.go: TestRunnerStripsSecretEnvironmentVariables`（新增 DroppedEnvKeys 断言）、`run_cmd_test.go`（Note 必须包含被丢弃的 MY_SECRET_TOKEN）。

### M16 search fallback 忽略过滤参数（2026-07-28 修复）
- **方案**：fallback 路径用 `matchGlobPath` 实现 rg 语义 glob 过滤（无斜杠匹配 basename、支持 `!` 否定且否定优先）；`searchOutput` 新增 `note` 字段披露无法生效的过滤（type、.gitignore 规则）。
- **复现用例**：`builtin_test.go: TestSearchGoFallbackAppliesGlobFilters`（含否定优先）、`TestSearchGoFallbackNotesUnappliedFilters`。

### A7 trustedProgramDirs 静态信任（2026-07-28 修复）
- **方案**：`isTrustedProgramDir` 运行时校验——目录必须在候选清单内 + root 所有 + 组/其他人不可写（`dirOwnedByRoot` 按 darwin||linux / 其他平台分文件，BUILD 同步）；本机实测 `/opt/homebrew/bin` 为 `liubang:admin drwxrwxr-x`，修复后不再获得 basename 信任。修正了注释的虚假安全假设。
- **复现用例**：`rules_test.go: TestNormalizeTrustedPathRejectsWritableDir`、`TestNormalizeTrustedPathRejectsNonRootOwnedDir`；既有 homebrew 断言改为运行时条件断言。
- **遗留（已闭环）**：`run.go` 的「ArgsHash HMAC」注释疑虑经复核为 M9 修复前的旧状态，M9 后注释与实现一致（见下方「A7 ArgsHash 注释复核」）。

### M11 AtomicWrite TOCTOU 覆盖（2026-07-28 修复）
- **方案**：① 新建文件场景改用 `os.Link` 提交（link 是原子 create-if-absent，EEXIST 即报「expected hash mismatch: file appeared during atomic write」），彻底消除窗口；② 已有文件场景在 rename 前再做一次 `recheckExpectedHash`（POSIX 无 compare-and-swap rename，窗口从整个 temp 写入时长缩到微秒级，注释说明该残余限制）；③ 新增 `beforeCommitHook` 测试注入点。
- **复现用例**：`workspace/path_test.go: TestAtomicWriteNewFileDoesNotOverwriteAppearingFile`（hook 在 commit 前创建同名文件，旧逻辑静默覆盖、修复后报错且原文件保留）、`TestAtomicWriteExistingFileRechecksHashBeforeCommit`（commit 前修改既有文件必须被拒）。两个用例均在临时回退旧逻辑后确认失败。
- **验证**：`go test ./internal/workspace/` 全绿。

### M14 任意 tea.Msg 触发 transcript 全量重建（2026-07-28 修复）
- **方案**：`BlockIndex` 新增单调 `version`（Add/Remove/Toggle×3/confirmPendingUserBlock/ApplyRuntimeEvent 处 bump，整体替换靠指针比较）；`syncTranscript` 在「索引指针+版本+宽度+主题均未变且无 volatile block（进行中/spinner 块）」时直接跳过 O(transcript) 重建。漏 bump 的代价是一次多余重建（安全方向），反向才会酿陈旧屏。
- **复现用例**：`ui/ui_test.go: TestUpdateSkipsTranscriptRebuildWhenNothingChanged`（先只加 `transcriptBuilds` 计数器验证旧行为：8 次按键 → 8 次全量重建；修复后按键不再重建、block 变更事件仍触发重建）。
- **验证**：`go test ./internal/ui/` 全绿。

### R2/R4/R8 小重构（2026-07-28）
- **R2**：`sqlite_store.go` 抽 `appendEventsTx`（版本校验+事件插入+版本推进骨架，`extra` 回调承载 checkpoint 写入）与 `validateContiguousEvents`（统一两个公开方法的事件校验，错误文案取原两者中更详细的版本）。
- **R4**：`workspace.ContainsSensitiveComponent` 导出为唯一清单；builtin/gittools/lint 三份逐字拷贝的 map+循环删除，各自保留一行委托。
- **R8**：
  - `reasoningDialLabel`(ui) ≡ `describeReasoning`(app) → 收敛为 `domain.ReasoningSpec.Label()`；
  - 预览/diff 常量双端各一份 → `domain.ToolPreviewMaxLines/ToolPreviewMaxBytes/ToolDiffMaxLines`；
  - `formatTokens` → 委托 `humanizeTokens`（既有测试期望全部兼容，1k–10k 区间显示从 "6k" 变为 "6.1k"）；
  - picker 窗口化×2 → `pickerWindow`；
  - `cutAtRuneBoundary`(agent/prompt) → 收敛为 `domain.TruncateAtRuneBoundary`；
  - **附带修复（R10 同类新发现）**：`app/controller.go: boundPreviewLines` 与 `ui/blocks.go: toolResultPreviewText` 的字节截断同样可切 UTF-8，一并改用 rune 边界截断。
- **验证**：`go test ./...` 全绿，`bazel test //go/pl/loom/...` 31/31（首轮有 1 次间歇失败，复跑 3 轮稳定通过，与本批改动无关）。

### A7 ArgsHash 注释复核（2026-07-28 复核，无需改码）
- 复核结论：review 原文的「实为截断 SHA-256 且 Execute 不校验」是 **M9 修复前的旧状态**；M9 已统一为真实 HMAC-SHA256（7 个工具的 `signPrepared` 均 `hmac.New(sha256.New, key)`，Execute 均 `hmac.Equal` 再校验），当前 `run.go:1858,1870` 的「ArgsHash HMAC that Execute re-verifies」注释与实现一致。「截断」仅存在于审批描述的 12 位展示前缀（`approvalDescHashPrefixBytes`），不影响校验。

### broker 序号空洞（2026-07-28 修复，review 低危项）
- **方案**：`Publish` 先用 `b.sequence+1` 构造并校验事件，通过后才提交序号。
- **复现用例**：`broker_test.go: TestBrokerRejectedEventDoesNotBurnSequence`。

### 冗余/死代码清理（2026-07-28）

- **R1**：删除 `runtimeevent/aggregator.go` + `aggregator_test.go`（agent/stream_hooks.go 的过期副本，零调用方），同步更新 BUILD。
- **R9（部分）**：删除无操作 `Message.MarshalJSON`、未使用的 `command.contentResult` 与 `builtin.defaultSearchContextLines`、edit 包重复的 `hashBytes`（统一 `sha256Hex`）。`CanTerminate`/`MaxParallelTools`/`MaxRepeatedActions`/`ControllerStateFatal` 涉及语义决策（实现对应能力 vs 删除配置项），留待后续。
- **A8**：删除 `domain.As`（语义偏离标准库），全部 18 处调用点改为 `errors.As`，`TestAsAgentError` 改为 `TestAgentErrorUnwrapsWithErrorsAs`。
- **验证**：`go build ./...`、`go vet ./...`、`gofmt -l .` 干净，`go test ./...` 全绿。

### H2 WallTime/CostUSD 从不更新（2026-07-28 修复）

- **方案**：① `Run` 新增 `turnStartedAt`（NewRun/RestoreRun/ResetUsageForNewTurn 锚定），`touchWallTime()` 在 `CheckBudget`/新增 `CheckRunaway()`/token 记账处折叠已逝窗口；② `Loop` 新增 `CostInputUSDPerMTok`/`CostOutputUSDPerMTok`，token 记账（抽取 `accountUsage`，callModel 与 summarizeForCompaction 共用）按费率折算 `CostUSD`；③ 费率从 tracing 配置（`cost_input_usd_per_mtok` 等）接入 controller 与 headless main，未配置时成本记账关闭。
- **复现用例**：`agent/run_test.go: TestRunWallTimeCountsTowardBudget`（FakeClock 推进 2min 触发硬 breach，重置窗口后清除）、`TestLoopExecuteCostBudgetExhausted`（2M tokens × $1/MTok 超过 $1 限额 → budget_exhausted）。
- **验证**：`go test ./internal/agent/ ./internal/app/ ./cmd/...` 全绿。

---

## 六、第二轮 Review（2026-08-08）

> 本轮由多代理并行深读全部 13 个内部包 + `go vet` + `deadcode` 静态分析交叉验证。编号沿用既有体系（H/M/R/A），新增 D（死代码）类。状态列随修复推进更新。

### 高严重度 Bug

| #   | 位置                                            | 问题                                                                                                                                                                          | 状态      |
| --- | ----------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| H8  | `tool/gittools/log.go:125,151-155`              | `git_log` 的 path 用 workspace 相对路径而非 repo 相对路径（`resolveRepoPath` 结果被丢弃），repo_root≠工作区根时 path 筛选必失效；diff/blame 均正确，唯独 log 错                | ✅ 已修复 |
| H9  | `model/openai/provider.go:978-987`              | chat completions 路径 EOF 一律报 `StopProviderError`；不发 `[DONE]` 的兼容网关在 `finishSeen` 后关连接会丢弃已完成结果（responses 路径 993-996 已有优雅收尾，chat 漏了同样保护） | ✅ 已修复 |
| H10 | `tool/subagent/delegate.go:245`、`wait.go:88`、`resume.go:106` | delegate/wait/resume 无 HMAC 签名（裸 `sha256(canonical)[:16]`），`DelegateTaskTool.Prepare` 缺 `call.Validate()` 与名称复核，Execute 不核验 Definition/Risk | ✅ 已修复 |
| H11 | `server/handlers_workspaces.go:146-203`         | browse 只对返回的 parent 链接做 home 限制，请求的 `path` 本身可列任意绝对路径（`?path=/etc` 正常返回），与文档化安全模型不符                                                    | ✅ 已修复 |
| H12 | `mcp/tool.go:70-80`                             | MCP `readOnlyHint` → `CapFSRead`（R1）被基线自动批准，但 readOnly 不承诺无网络 → 搜索/抓取类 MCP 工具形成无审批网络出口（loom 自己的 web_fetch 走 R3+域名规则）               | ✅ 已修复 |
| H13 | `tool/exsession/manager.go:71-94`               | `commitArtifacts` 先置 `committed=true` 再 Commit；失败时 staging 已被 defer 的 Abort 销毁 → 会话输出永久丢失且无信号，error 被两个调用点 `_ =` 吞掉                            | ✅ 已修复 |
| H14 | `session/sqlite_store.go:1128`                  | `GetOrCreateShare` SELECT-then-INSERT + `ON CONFLICT DO UPDATE` 跨进程竞态：并发/重入时先调用方拿到已被覆盖的失效 token                                                          | ✅ 已修复 |
| H15 | `agent/run.go:1488,1487-1507,1511-1515`；`runaway.go:155-164` | 终止缺陷×4：①`maxOutputStops` 声称"连续"截断但从不重置（实际全程累计）；②goal wrap-up 回合内 `StopMaxOutput` 存在无限循环窗口；③未知 stop reason 无条件回 PhasePreparing 可无界重试；④stall 提醒 `stallTurns` 归零后 level 恒为 1 被去重挡住，实际只触发一次 | ✅ 已修复 |

### 中严重度 Bug

| #   | 位置                                        | 问题                                                                                                                                                     | 状态      |
| --- | ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| M18 | `client/http.go:330-377`                    | `pumpSSE` 对 `server.resync`/`server.draining` 只丢帧不终止（与注释承诺不符），调用方无法区分重连与不再重连 → 服务端关机期重连风暴；`scanner.Err()` 从不检查 | ✅ 已修复 |
| M19 | `client/http.go:203-215,217`                | `State()` 在 getter 里做同步网络调用且任何错误都映射成 `Booting`（死会话显示"启动中"）；`Done()` 与 inproc 语义不一致（服务端会话结束不触发）              | ✅ 已修复 |
| M20 | `app/session_service.go:981-1061`           | 订阅路径不检查 `closing`，`wg.Add` 撞上 `Shutdown` 的 `wg.Wait` 属 WaitGroup 误用（可 panic）                                                             | ✅ 已修复 |
| M21 | `app/controller.go:1131-1152`               | steer 与 turn 结束 relay 之间 TOCTOU：prompt 滞留到下次提交才注入，UI 却已提示 "Queued"                                                                   | ✅ 已修复 |
| M22 | `app/bootstrap.go:263-329`                  | `buildSubagentRegistry`/`buildCoderRegistry`/`subagent.NewManager` 失败路径不 Close 已创建的 `sessionManager`（相邻路径都 Close，不一致）                  | ✅ 已修复 |
| M23 | `mcp/client.go:161-176,211-221`             | 协议版本协商结果解析后丢弃（后续请求始终发自家版本）；`tools/list` 未处理 `nextCursor` 分页，工具多的服务端被静默截断                                      | ✅ 已修复 |
| M24 | `server/server.go:267-295`                  | 中间件注释承诺的 panic recovery 未实现；CORS 无 preflight 处理，`--allow-origin` 对浏览器实际不可用（OPTIONS 被 401）                                       | ✅ 已修复 |
| M25 | `tool/builtin/view_image.go:127-137`        | 先整读再限尺寸：对大文件调 view_image 会把进程内存打爆（`pathInfo.Info.Size()` 已拿到，读前即可拒绝）；`edit/common.go:231` 同模式                          | ✅ 已修复 |
| M26 | `tool/gittools/common.go:571-606`           | git 全部命令在沙箱外执行，虽禁 hooks/ext-diff，但 repo 级 `.git/config` 的 `core.fsMonitor` 等仍可注入命令（恶意仓库跑 git_status 即中招）                 | ✅ 已修复 |
| M27 | `server/errors.go:125-147`                  | `Contains(msg, "invalid")` 过宽：任何 message 碰巧含 "invalid" 的内部错误被错映射为 400                                                                   | ✅ 已修复 |
| M28 | `agent/run.go:1689-1694`                    | stall watchdog 只补偿 approval 等待，不补偿 ask_user 提问等待——用户思考超 StallTimeout 健康 run 被判 Failed                                                 | ✅ 已修复 |
| M29 | `session/rewind.go`、`session/sqlite_store.go` | `RewindSession` 不清理 `artifact_refs`（GC 永久泄漏）和 `memory_jobs`（rewind 后不再重新提取）；`DeleteSession` 非事务三条 DELETE                       | ✅ 已修复 |
| M30 | `model/anthropic/provider.go:276-285,364`   | `appendBlock` 把块并入上一条同 role 消息：`tool_result` 出现在 assistant 首块时会并入前一条 user 消息，wire 上 tool_result 先于 tool_use → API 400          | ✅ 已修复 |
| M31 | `trace/otel.go:116-118,236`                 | `otel.SetErrorHandler` 进程级全局设置多次 Setup 互相覆盖；所有 provider 的 `gen_ai.system` 硬编码 `"openai"`（Anthropic 直连也被标记 openai）               | ✅ 已修复 |
| M32 | `permission/rules.go:531-540`               | 会话记忆规则先插入先匹配而非最长前缀优先：更具体的授权（如 `go test`）被宽泛规则（`go`）遮蔽并继承其更宽 grant                                               | ✅ 已修复 |
| M33 | `permission/decider.go:37-228`              | 一次策略评估重复解析 argv 多达 4 次（每次 = JSON unmarshal + shell 解包），应在 Chain 入口解析一次透传                                                      | ✅ 已修复 |
| M34 | `model/stream/stream.go:70-81`              | panic 恢复路径中 emit 可能永久阻塞（recover 先于 Close 执行，events 缓冲满且消费者流失时 `close(s.events)` 永不执行）                                       | ↩️ 修复后被作者回退（保留阻塞 emit 语义，见修复记录） |
| M35 | `config/init.go:62-289`                     | init 模板缺整个 `memory:`/`image:`/`logging:`/`workspaces:` 节及 `subagent.max_output_tokens`，与 schema 严重漂移，用户无法从模板发现这些功能               | ✅ 已修复 |
| M36 | `cmd/loom/main.go:593-604`                  | headless 路径缺 provider nil 检查（controller.runTurn 有显式检查并注释"fail loudly"），配置异常时直接 nil panic                                            | ✅ 已修复 |

### 死代码（deadcode + 人工 grep 双重确认）

| #   | 位置                                  | 内容                                                                                                                                          | 状态      |
| --- | ------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| D1  | `domain/lifecycle.go:178-186`         | `CanTerminate` 无条件返回 true，前面遍历是死逻辑（R9 遗留，本轮决策处理）                                                                        | ✅ 已删除 |
| D2  | `domain/ids.go:103-104`               | `ParseTurnID`/`ParseMessageID` 无调用方                                                                                                        | ✅ 已删除 |
| D3  | `domain/lifecycle.go:62-66`           | 4 个 `SuspensionReason` 常量零引用；`Suspend()` 不校验 reason 合法性                                                                            | ✅ 已删除 |
| D4  | `domain/interfaces.go:162`            | `ModelEventProviderWarning` 定义后两个 provider 从未发射                                                                                         | ⏭️ 保留（stream_hooks 有容错 case + 协议文档记录） |
| D5  | `tool/edit/common.go:333-368`         | `splitFileLines`/`joinFileLines`/`fileLine` 整段死代码                                                                                           | ✅ 已删除 |
| D6  | `tool/gittools/common.go:253,418,434` | 死函数 `sameSortedStrings`；`buildRevParseVerifyArgs` 与 `buildRefVerifyArgs` 函数体完全相同                                                     | ✅ 已删除/合并 |
| D7  | `tool/builtin/view_image.go:42`       | 死常量 `maxImagePath`                                                                                                                           | ✅ 已删除 |
| D8  | `model/openai/provider.go:1362,1365,1389` | `sawArgumentEvents`/`doneSeen` 写后未读；`toolCallDelta.Type` 从不校验                                                                      | ✅ 已删除 |
| D9  | `client/http.go:132,333-375`          | `mapWireError` 的 `status` 死参数；SSE `id` 变量解析后靠 `_ = id` 压制编译器                                                                    | ✅ 已删除 |
| D10 | `process/types.go:42,52-54`           | `Isolation.Unsafe()` 零调用、`isolationMode.String()` 不可达——`unsafe: true` 标记从未生效                                                        | ✅ 已删除 |
| D11 | `app/workspace_registry.go:91,100`    | `NewSingletonWorkspaceService`/`newSingletonRegistry` 不可达                                                                                     | ⏭️ 保留（10 处测试调用方，测试组装工具） |
| D12 | `tool/exsession/manager.go:47`；`common.go:255-302` | `sessionEntry.cwd` 写后从不读；`drainSession` 的 error 返回恒为 nil（调用方死分支）                                                  | ✅ 已修复 |
| D13 | `ui/update.go:2328`；`picker.go:203`  | `submitPromptCmd`、`pickerWindow` 无调用者（后者是 R8 去重产物，已被 `Finder.windowRows` 取代）                                                  | ✅ 已删除 |
| D14 | `ui/update.go:79,2335,1213`           | `promptSubmittedMsg.imagePaths` 死字段链（收集→透传→从不读取）                                                                                   | ✅ 已删除 |
| D15 | `render/linear.go`、`render/jsonl.go` | 两个渲染器生产零引用（`loom run` 直接 fmt.Print），deadcode 确认整体不可达；决策：保留为演示组件 or 删除                                          | ⏭️ 决策保留（演示组件） |
| D16 | `agent/run.go:2386`；`stream_hooks.go:333` | `lastAssistantText`/`aggregateStream` 仅测试引用；前者与 `subagent/delegate.go:536` 逐行重复                                                | ✅ 已收敛（导出 agent.LastAssistantText，aggregateStream 移入测试文件） |
| D17 | `app/controller.go:2557`；`ui/blocks.go:851`；`render/linear.go:41` | 孤儿注释×2、`indent` 死字段                                                                                                                | ✅ 已删除（另删零引用的 Controller.getState） |

### 冗余（新增，编号续 R10）

| #   | 位置                              | 问题                                                                                                                              | 状态      |
| --- | --------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- | --------- |
| R11 | `app/bootstrap.go:524-706`        | 三份复制的工具注册清单（主 agent/researcher/coder 共享 12 个只读工具逐字三遍），新增只读工具要改三处                                  | ✅ 已处理 |
| R12 | `config/resolve.go:1047` ↔ `mcp/manager.go:34` | `config.MCPServer` 与 `mcp.ServerConfig` 10 字段完全相同，`process_runtime.go:269-283` 逐字段手工拷贝；应类型别名          | ✅ 已处理 |
| R13 | `agent/ask.go:168`、`plan.go:306`、`goal.go:338` | 三个 `xxxError` 函数逐行相同                                                                                              | ✅ 已处理 |
| R14 | `server/handlers_artifacts.go:34-62` ↔ `handlers_share.go:95-126` | artifact 服务块逐行重复（id/size 解析 + ArtifactRef + 响应头 + 写字节）                                          | ✅ 已处理 |
| R15 | `client/http.go:115,312`          | 错误响应解码块在 `do()` 与 `SubscribeEvents` 重复，可抽 `decodeWireError`                                                           | ✅ 已处理 |

> 备注：R3（baseTool 骨架 10 份复制 + errorResult 3 变体漂移）、R5（provider 间重复）、R6（controller RPC 样板，本轮复核为 14 个方法约 250 行）本轮复核仍然成立，继续挂账。

### 优化空间（记录备查，不阻塞修复）

| #   | 位置                        | 问题                                                                                                                     |
| --- | --------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| O1  | `prompt/prompt.go:259-627`  | 每个模型请求都重做全部磁盘/进程 I/O（规则文件、目录遍历、fork 两个 git 进程），静态内容可按 mtime/turn 缓存               |
| O2  | `session/sqlite_store.go`   | `ListCheckpoints` 为 80 字符 label 反序列化全部 checkpoint 完整 transcript；回放 `rebuildIndexes` O(n²)                   |
| O3  | `agent/compact.go:260-270`  | `maskRange` Level-2b 每轮全量 `estTokens` → O(n²)；可维护增量估算                                                          |
| O4  | `agent/stream_hooks.go:150,182` | 流式聚合 `+=` 拼字符串 O(n²)，改 `strings.Builder`                                                                     |
| O5  | `anthropic/provider.go:350-357,487-489` | 工具 Arguments/InputSchema 双重 JSON 编解码，可直接内嵌 `json.RawMessage`                                      |
| O6  | `ui/view.go:1554`           | `truncateDisplayWidth` 逐 rune `lipgloss.Width(string(r))`（热路径每 rune 一次分配+全宽计算）                              |
| O7  | `process/runner.go:417-449` | 每个命令对可执行文件做两遍完整 sha256，第一遍结果可复用                                                                    |
| O8  | `ui/update.go:2039-2060`    | `mergeSnapshot`/`hasEquivalentBlock` O(n²)，可按块 ID/内容哈希建索引                                                       |

### 第二轮修复记录（2026-08-08）

> **回归验证**：`go build ./...`、`go vet ./...`、`gofmt -l .` 干净；`go test ./...` 全绿；`go test -race ./internal/app/ ./internal/session/ ./internal/client/ ./internal/agent/ ./internal/model/...` 全绿；`bazel test //go/pl/loom/...` 通过；真实模型 e2e（`LOOM_E2E_LLM=1`，本机 ~/.loom/config.yaml）通过。deadcode 复扫：本轮目标项全部消除，残余命中均为测试工具/演示组件（见 D4/D11/D15）。

- **H8**：`validateGitLogArgs` 改为返回 readPaths（含 path 绑定），Execute 复核绑定后用 `pathInfo.RepoRelative` 组装 pathspec，与 git_blame/git_diff 对齐。回归：`gittools_test.go: TestGitLogToolPathFilterWithSubdirRepoRoot`。
- **H9**：`finishChatReadError` 增加 `EOF && state.finishSeen` 分支走 `finishChatDone` 优雅收尾（镜像 responses 路径）。回归：`openai/provider_test.go: TestStreamChatCompletionsToleratesMissingDoneAfterFinishReason`。
- **H10**：subagent 新增 `sign.go`——每工具实例随机 HMAC key，签名指纹含 CallID/ToolName/canonical args/Risk；三个工具 Prepare 补 `call.Validate()`+名称复核并签名，Execute 先 `verifyPreparedCall`（名称/来源/Risk/HMAC）。e2e 中手工拼装的 PreparedCall 改为走 Prepare（签名协议生效的直接证据）。回归：`delegate_test.go: TestDelegateExecuteRejectsTamperedPreparedCall`（篡改 Risk/伪造 hash/跨实例签名均拒绝）。
- **H11**：browse 对请求 path 本身做 home 限制（`EvalSymlinks` 后的 homeResolved 前缀校验），parent 链接同步用 homeResolved。回归：`server_test.go: TestBrowseDirectories` 新增 `/etc` 拒绝断言，临时目录改建于 $HOME 内。
- **H12**：`capabilitiesForSpec` 的 ReadOnlyHint 分支叠加 `CapNetworkConnect` → R3 需审批（MCP 规范 openWorldHint 默认 true 且 wire 类型无法区分未设置）。回归：`mcp/tool_test.go: TestCapabilitiesForSpecReadOnlyIsNotAutoApprovable` 等两个。
- **H13**：`commitArtifacts` 仅在全部 Commit 成功后置 `committed`，失败保留 staging 供重试；`drainSession` 签名去掉恒 nil 的 error，commit 失败写进输出 note；顺带删除 `sessionEntry.cwd` 死字段与冗余 nil 判断（D12）。
- **H14**：`GetOrCreateShare` 改 `INSERT ... ON CONFLICT DO NOTHING` + 回读，并发创建都拿到持久化 token。回归：`sqlite_store_test.go: TestSQLiteStoreConcurrentShareCreationReturnsPersistedToken`（-race 通过）。
- **H15**：①callModel 在非 `StopMaxOutput` 时重置 `maxOutputStops`；②`determineCompletion` 的 StopMaxOutput 分支改为任何 `WrapUpPending != ""` 都着陆（`wrapUpOutcome` 补 goal_tokens→Succeeded），消除 goal wrap-up 无限循环窗口；③未知 stop reason 新增 `unknownStopStreak` 上限（`maxUnknownStopRetries=3`）后 OutcomeFailed；④stall 提醒不再清零 `stallTurns`，level 递增实现周期性提醒且文案反映真实停滞数。回归：`TestLoopMaxOutputStreakResetsOnNormalTurn`、`TestLoopGoalWrapUpTurnOverflowStillLands`、`TestLoopUnknownStopReasonRetriesBounded`、`TestRunawayStallReminderFiresPeriodically`。
- **M18/M19/D9/R15**（client）：`pumpSSE` 复用 `model/sse.Parser`（无空格前缀兼容、EOF flush 末帧、读错误 slog 可见）；`server.resync` 终止流、`server.draining` 额外关闭 `Done()`（关机不再重连）；`State()` 增加 lastState 缓存，404→Closed，瞬时错误保持最后已知状态；删 `mapWireError` 死参数与 `_ = id` hack；抽 `decodeWireError`。回归：client_test.go 新增 7 个用例（resync/draining/紧前缀/读错误/状态映射/wire 错误/非信封错误）。
- **M20**：订阅入口在 `s.mu` 临界区内检查 `closing` 并完成 `wg.Add`，关闭后返回 `ErrDraining`。回归：`TestSessionServiceSubscribeAfterShutdownDrains`、`TestSessionServiceSubscribeShutdownRace`（-race）。
- **M21**：steer 入队后在 `c.mu` 下复查 state，已 Idle 则立即按新 prompt relay；relay 逻辑抽 `relayPendingSteers` 共用。回归：`TestControllerSteerLandingAfterTurnEndRelaysImmediately`（修复前稳定复现滞留）。
- **M22**：`NewWorkspaceBootstrap` 改具名返回值 + defer 统一清理链，删除 7 处手工 Close。回归：`TestNewWorkspaceBootstrapFailureClosesSessionManager`（goroutine 计数观测 reaper 回收）。
- **M23**：initialize 协商的旧版本协议存入 transport 并在后续请求的 `MCP-Protocol-Version` 回送；`ListTools` 支持 nextCursor 分页。回归：`TestHTTPClientAdoptsNegotiatedProtocolVersion`、`TestHTTPClientListToolsPagination`。
- **M24/M27/R14**：withMiddleware 补 panic recovery（500 + slog，不泄漏 panic 值）与 CORS preflight（合法 Origin 的 OPTIONS 204 + Allow-Methods/Headers）；`mapError` 新增 `domain.ErrInvalidInput` sentinel 结构化匹配，字符串兜底收窄为精确短语集；artifact 服务块抽 `parseArtifactRefParam`/`serveArtifactBytes` 共用。回归：`middleware_test.go`/`errors_test.go`/`handlers_share_test.go` 新增用例。
- **M25**：view_image 用 `pathInfo.Info.Size()` 读前拒绝超限；edit 用 `snapshot.Size` 预检；顺带 `strings.NewReader(string(data))` → `bytes.NewReader`。
- **M26**：`gitBaseArgs` 增加 `-c core.fsmonitor=false -c core.untrackedCache=false`，堵住 repo 级 `.git/config` 命令注入。
- **M28**：新增 `Loop.executeTool`——`CapUserInteract` 工具（ask_user）的等待时长补偿进 stall 基线（与 awaitApproval 同语义）。
- **M29**：rewind 事务内 `recomputeArtifactRefs`（从存活 checkpoint 重算登记）；stale/claimed memory_jobs 重置为 pending 重新提取；`DeleteSession` 三条 DELETE 包事务。回归：`TestSQLiteStoreRewindSessionRecomputesArtifactRefs`、`TestRewindSessionResetsStaleMemoryJob`、`TestSQLiteStoreDeleteSessionRemovesAllSessionData`。
- **M30**：anthropic 消息装配重构为 `messageSink`，`appendToolResult` 只允许并入全为 tool_result 的 user 消息（并行结果），并入含文本的 user 消息直接报错（避免 tool_result 先于 tool_use 的必 400 wire）。回归：`TestAppendAssistantToolResultMergeGuard`。
- **M31**：otel error handler 改进程级单例 fan-out（Setup 订阅/Shutdown 退订，sync.Once 安装）；`gen_ai.system` 从模型名启发式推导（claude/anthropic→anthropic，gpt/openai→openai，其余 unknown）。回归：`TestExportErrorHandlerFanOut`、`TestGenAISystemFromModel`、`TestGenerationSpanGenAISystem`。
- **M32**：`SessionRules.Match` 改最长前缀优先（等长保持插入序）。回归：`TestSessionMatchLongestPrefixWins`。
- **M33**：`Chain.Evaluate` 惰性解析 argv 一次，经未导出 `evalContext`/`contextDecider` 快路径透传给四个内置 Decider（接口签名不变，编译期断言防退化）。回归：`TestChainSharedExecContextEquivalence`（六种调用形态 verdict 与逐 decider 独立解析逐字段一致）。
- **M34**：修复（panic 路径改非阻塞 emit）后被作者回退，保留原阻塞语义；风险边缘（缓冲满+消费者流失+未 Close 时 goroutine 滞留），如后续观察到流挂死再评估。
- **M35**：init 模板补齐 `logging:`/`memory:`/`image:`/`workspaces:` 节与 `subagent.max_output_tokens`。回归：`TestTemplateCoversSchemaSections`（18 个顶层节 + 默认值一致性）。
- **M36**：headless 路径补 `provider == nil` 检查，与 controller.runTurn 对齐。
- **死代码**：D1/D2/D3/D5/D6/D7/D8/D9/D10/D13/D14/D17 删除（另删零引用 `Controller.getState`）；D16 收敛为 `agent.LastAssistantText` 导出复用、`aggregateStream` 移入测试；D4（stream_hooks 容错 case + 协议文档记录）、D11（11 处测试调用方）、D15（演示组件）经复核保留。
- **冗余**：R11 抽 `toolFactory`/`registerToolFactories`/`readOnlyToolFactories` 公共表（三处注册清单合一，registerMemoryTools 同构复用）；R12 `config.MCPServer = mcp.ServerConfig` 类型别名，process_runtime 直接透传；R13 三个 xxxError 收敛为 `toolErrorResult`；R14/R15 见上。
- **格式**：修复 8 个存量 gofmt 未格式化文件（runtimeevent/event.go、cmd/loom/main.go、imagegen、subagent 测试等），建议 CI 增加 gofmt 门禁。

### 第二轮真实环境 e2e 验收（2026-08-08）

- `LOOM_E2E_LLM=1 go test ./e2e/ -run TestServeRealModelE2E`（本机真实 provider）：**PASS**（17.1s）——真实工具循环（read_file 暗号验证）、事件流水位交接、busy-turn steering、幂等提交、跨 client 会话恢复全链路通过。
- `LOOM_E2E_LLM=1 go test ./e2e/ -run TestMemoryPipelineRealModelE2E`（本机真实 provider）：**PASS**（32.5s）——会话落盘 → 后台记忆提取（Claimed:1 Succeeded:1）→ Phase 2  consolidation（MEMORY.md/summary/raw 产物齐全）全链路通过。
- 备注：`bazel test` 首轮 `process_test` 出现一次间歇性时序失败（`TestSessionIncrementalReadAndExit` 的 150ms 时序窗口），复跑及 `go test -count=10` 均稳定通过，与本批改动无关。
