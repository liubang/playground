# Flux Language Server 优化路线图

## 概述

本文档追踪 flux-ls 从"最小可用"到"生产级 IDE 体验"的完整优化路径。
按优先级分为三个阶段，每阶段完成后更新验收状态。

---

## 第一阶段：性能基础 + 低成本高收益功能

| # | 任务 | 状态 |
|---|------|------|
| 1.1 | AST 缓存层（避免重复解析） | ✅ 完成 |
| 1.2 | 增量文档同步（TextDocumentSyncKind=2） | ✅ 完成 |
| 1.3 | simdjson parser 实例复用（融入 ensure_ast 统一管理） | ✅ 完成 |
| 1.4 | textDocument/documentSymbol | ✅ 完成 |
| 1.5 | textDocument/foldingRange | ✅ 完成 |
| 1.6 | 用户定义符号补全 | ✅ 完成 |

**验收标准**：编译通过，所有单元测试通过，新增功能有对应测试覆盖。

**验收状态**：✅ 已验收（编译通过，10 个测试全部 PASS）

---

## 第二阶段：语义分析层 + 核心导航功能

| # | 任务 | 状态 |
|---|------|------|
| 2.1 | 符号表 + 作用域分析器（SymbolTable + SymbolCollector） | ✅ 完成 |
| 2.2 | textDocument/definition | ✅ 完成 |
| 2.3 | textDocument/references | ✅ 完成 |
| 2.4 | textDocument/rename | ✅ 完成 |
| 2.5 | textDocument/signatureHelp | ✅ 完成 |
| 2.6 | textDocument/documentHighlight | ✅ 完成 |
| 2.7 | 语义诊断增强（未定义标识符警告） | ✅ 完成 |

**验收标准**：编译通过，所有单元测试通过，goto definition / find references / rename 功能端到端可用。

**验收状态**：✅ 已验收（编译通过，15 个测试全部 PASS）

---

## 第三阶段：高级功能 + 编辑器深度集成

| # | 任务 | 状态 |
|---|------|------|
| 3.1 | textDocument/semanticTokens/full（delta 编码，支持定义/引用着色） | ✅ 完成 |
| 3.2 | textDocument/codeAction（quickfix: 自动添加 import） | ✅ 完成 |
| 3.3 | textDocument/inlayHint（函数参数默认值提示） | ✅ 完成 |
| 3.4 | textDocument/selectionRange（word→statement 两级选择） | ✅ 完成 |

**验收标准**：编译通过，所有单元测试通过，semantic tokens 在编辑器中可着色验证。

**验收状态**：✅ 已验收（编译通过，19 个测试全部 PASS）

---

## 变更日志

| 日期 | 变更内容 |
|------|----------|
| 2025-07-19 | 第一阶段完成：AST 缓存、增量同步、documentSymbol、foldingRange、用户符号补全 |
| 2025-07-19 | 第二阶段完成：符号表分析器、definition、references、rename、signatureHelp、documentHighlight、语义诊断 |
| 2025-07-19 | 第三阶段完成：semanticTokens、codeAction、inlayHint、selectionRange |
