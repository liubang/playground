# SSTable Engine

用 C++20 实现的 SSTable (Sorted String Table) 存储引擎，即 LSM-Tree 数据库的核心磁盘组件。支持完整的读写路径：Block 前缀压缩编解码、布隆过滤器（Standard + Blocked Bloom）、压缩（Snappy/ZSTD）、两级迭代器、Copy-on-Write 版本管理。

数据模型为宽列模型，类似 HBase：`rowkey + column_family + column_qualifier + timestamp + type`。

## 文档

- [docs/architecture.md](docs/architecture.md) — 架构概览：文件格式、组件关系、数据流、设计决策

## 技术栈

| 领域 | 选型 | 说明 |
| ---- | ---- | ---- |
| 语言 | C++20 | concepts, constexpr |
| 构建 | Bazel (bzlmod) | 确定性构建 |
| 哈希 | XXHash | Bloom Filter 哈希函数 |
| 校验 | CRC32C | Block 完整性校验 |
| 压缩 | Snappy / ZSTD | 可选 Block 压缩 |
| 工具 | folly::ScopeGuard | RAII 资源清理 |

## 构建

```bash
# 构建库
bazel build //cpp/pl/sst:sstable

# 构建 CLI
bazel build //cpp/pl/sst/cli:sst_cli
```

## 测试

```bash
bazel test //cpp/pl/sst/ut/...
```

覆盖 9 个测试文件：cell、comparator、encoding、compression、filter_policy、block、sstable_format、table 端到端读写、回归测试。

## 代码结构

```
cpp/pl/sst/
├── cell.h                     # 数据模型 (CellKey + value)
├── comparator.h               # 键比较器 + shortest separator
├── encoding.h                 # 定长整数序列化
├── options.h                  # 读写配置 (block_size, compression, bloom bits)
├── block_builder.{h,cpp}      # 写入侧 Block 构建，前缀压缩
├── block.{h,cpp}              # 读取侧 Block 解析，二分查找
├── filter_policy.{h,cpp}      # Standard/Blocked Bloom Filter
├── compression.{h,cpp}        # 压缩适配器
├── sstable_format.{h,cpp}     # 文件格式：BlockHandle, Footer, FileMeta, BlockReader
├── sstable_builder.{h,cpp}    # SST 文件写入器
├── sstable.{h,cpp}            # SST 文件读取器 (open + get)
├── sstable_iterator.{h,cpp}   # 两级迭代器 (index + data)
├── sstable_version_manager.{h,cpp}  # Copy-on-write 版本管理
├── iterator.h                 # 抽象迭代器接口
├── cli/                       # CLI 工具（开发中）
├── docs/
│   └── architecture.md        # 架构文档
└── ut/                        # 单元测试
```

## 已知限制

- 单线程读写，无并发控制
- 未实现 Compaction（预留接口）
- CLI 工具尚在开发中
- 无 WAL / MemTable 层（只实现磁盘 SSTable 部分）
