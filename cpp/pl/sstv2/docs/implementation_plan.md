# SSTableV2 实现规划

基于 `design.md` 的详细设计，本文档规划从零开始的编码实现方案。

---

## 1. 模块划分

```
cpp/pl/sstv2/
  codec/           基础编码原语（endian, varint, memcomparable）
  types/           类型系统（DataType, Value, Schema, InternalSchema, Row, InternalRow）
  block/           Block 格式（header, data table, column-store unit, offset table, bitmap）
  pattern/         Pattern 编码器/解码器（pattern 0-5, 100）
  bloom/           Bloom Filter 构建与查询
  index/           Index Tree 构建与遍历
  metadata/        Metadata Section 编解码
  file/            文件级组装（Tail, Locator, Builder, Reader, ValueFile）
  compress/        Block 压缩/解压
  ut/              单元测试（按模块子目录组织）
  docs/            设计文档
```

---

## 2. 模块依赖关系

```
file/ (顶层)
  |-- block/
  |     |-- pattern/
  |     |     +-- codec/
  |     |-- compress/
  |     +-- codec/
  |-- index/
  |     |-- block/
  |     +-- codec/
  |-- bloom/
  |     +-- codec/
  |-- metadata/
  |     |-- codec/
  |     +-- types/
  |-- types/
  +-- codec/
```

依赖方向：上层依赖下层，下层不依赖上层。`codec/` 是最底层的基础模块，`types/` 不依赖 `codec/`（纯类型定义）。

---

## 3. 核心设计决策

### 3.1 外部 Schema 与内部 Schema 分离

**Schema（外部）**：纯粹描述用户定义的 M 列 RowKey。不知道系统列的存在。这是用户接口的边界。

**InternalSchema（内部）**：持有 `const Schema&`，在其基础上追加 7 个系统列，提供统一的 `column(i)` 接口覆盖 [0, M+7) 全部列：

```
Index       Name        Type       SortOrder
-----       ----        ----       ---------
0..M-1      (user)      (user)     (user)
M           Version     kVersion   kDescending
M+1         OpType      kUint8     kAscending
M+2         Flag        kUint64    (none)
M+3         Filename    kString    (none)
M+4         Offset      kUint64    (none)
M+5         Length      kUint64    (none)
M+6         Checksum    kUint64    (none)
```

前 M+2 列参与 all_key 排序，后 5 列是 payload 元数据。

### 3.2 KV 分离原则

InternalRow 是纯粹的 **key 侧** M+7 列容器。Value 的实际字节（无论 embedded 还是 separated）不存储在 InternalRow 中，而是通过独立通道传递。

Internal Table 的 (Filename, Offset, Length, Checksum) 列是"value 的定位指针"，不是 value 本身。Data Table 和 Value File 本质上是同一角色——value 存储区，区别只是一个在 block 内部，一个在外部文件。

### 3.3 数据流架构

**写路径：**

```
Row (用户输入)
  │
  ▼
SSTableBuilder::add(Row)
  │  校验 schema、构造 ColumnFlag、决定 embedded/separated
  │  如果 separated：写 value file，得到 offset/length/checksum
  │  如果 embedded：value bytes 暂存
  │  填充 M+7 列 → InternalRow
  │  计算 all_key
  │
  ├─→ InternalRow (纯 M+7 列)          ─┐
  ├─→ all_key (std::string)             ─┼─→ BlockBuilder::add(row, all_key, payload)
  └─→ value_payload (std::string_view)  ─┘
                                           │
                                           ▼
                                    Block bytes → Key File
```

**读路径：**

```
Key File bytes
  │
  ▼
BlockReader::decode()
  │  解码 column-store units → M+7 列
  │
  ├─→ InternalRow (纯 M+7 列)
  └─→ data_table buffer (block 生命周期内有效)
        │
        ▼
SSTableReader
  │  根据 Filename/Offset/Length 从 data_table 或 value file 取 value bytes
  │  剥离系统列 → 返回用户结果
```

### 3.4 Value 类型设计

Value 使用 `std::variant` + 模板 traits 实现类型安全的 tagged union：

- `TypeMapping<DataType>` 提供编译期 DataType → C++ 类型映射
- `Value::make<DT>(v)` 编译期类型安全构造
- `get<DT>()` / `ref<DT>()` / `take<DT>()` 编译期类型安全访问
- `visit(callable)` 支持 std::visit 模式匹配
- `make_value(T)` 通过 NativeTypeMapping 反向推导自动构造

### 3.5 ColumnFlag 设计

ColumnFlag 是 64-bit 标志位的强类型封装，全链路 constexpr：

- 位布局：DT[0-7] + C[8] + B[9]，bits 10-63 保留为 0
- 工厂方法：`for_value(dt, checksum, bool_val)` / `for_data_block()` / `for_index_block()`
- 语义查询：`is_index_entry()` / `is_data_block_ptr()` / `is_value_flag()` / `is_valid()`
- 索引块中 DT 使用私有类型 21(DataBlock)/22(IndexBlock)，C 和 B 必须为 0

---

## 4. 实现顺序（自底向上）

### Step 1: codec/ — 基础编码原语 ✅

| 文件 | 职责 |
|------|------|
| `codec/endian.h` | Little-endian 定长整数编解码（header-only, memcpy） |
| `codec/varint.h/.cpp` | LEB128 varint + inline zigzag |
| `codec/comparable.h/.cpp` | Memcomparable encoding（sign-flip+BE, 8-byte escaped groups, bitwise-invert for desc） |

---

### Step 2: types/ — 类型系统 ✅

| 文件 | 职责 |
|------|------|
| `types/data_type.h` | DataType 枚举 + constexpr 分类 traits + `operator<<` |
| `types/op_type.h` | OpType 强类型枚举（kPut, kDelete） |
| `types/column_flag.h` | ColumnFlag class（constexpr 位域操作 + 语义方法 + 验证） |
| `types/value.h` | Value（std::variant + TypeMapping 模板 traits，header-only） |
| `types/schema.h` | Schema + SchemaBuilder（用户 RowKey 列定义，带验证） |
| `types/internal_schema.h` | InternalSchema（M+7 列统一视图，持有 Schema 引用） |
| `types/row.h` | Row（用户输入信封：key_columns + version + op_type + value payload） |
| `types/internal_row.h` | InternalRow（纯 M+7 列容器 + typed accessors） |

**设计要点**：

- Schema 只描述用户的 M 列 RowKey，通过 SchemaBuilder 验证构造（拒绝空名、重复名、非 key 兼容类型）。
- InternalSchema 在 Schema 基础上追加 7 个系统列，提供统一的列索引接口。
- InternalRow 是纯粹的 key 侧 M+7 列容器，不携带 value bytes 和 all_key。
- Row 是用户接口，version/op_type 作为独立字段（用户不需要知道它们在内部是列）。
- Row → InternalRow 的转换是 SSTableBuilder 的职责。

---

### Step 3: pattern/ — Pattern 编码器

**目标**：实现 Column-Store Unit 的各种 pattern 编码。

| 文件 | 职责 |
|------|------|
| `pattern/pattern.h` | PatternId 枚举，PatternEncoder/PatternDecoder 接口 |
| `pattern/raw.h/.cpp` | Pattern 0：无编码，定长列直接存储 |
| `pattern/compound.h/.cpp` | Pattern 100：变长/复合列拆分为子列 |
| `pattern/stream_vbyte.h/.cpp` | Pattern 1：Stream VByte（Phase 2） |
| `pattern/constant_stride.h/.cpp` | Pattern 4/5：等差序列（Phase 2） |
| `pattern/pfor.h/.cpp` | Pattern 2：PFOR（Phase 3） |
| `pattern/dict.h/.cpp` | Pattern 3：Dictionary（Phase 3） |
| `pattern/selector.h/.cpp` | 自动选择最优 pattern 的策略（Phase 2） |

Phase 1 实现 Pattern 0 和 Compound Pattern（子列用 Pattern 0）。

---

### Step 4: compress/ — 压缩

| 文件 | 职责 |
|------|------|
| `compress/compressor.h` | Compressor 接口 |
| `compress/none.h` | 无压缩（直通） |
| `compress/lz4.h/.cpp` | LZ4（Phase 2） |
| `compress/zstd.h/.cpp` | Zstd（Phase 2） |

Phase 1 只实现无压缩。

---

### Step 5: block/ — Block 格式

| 文件 | 职责 |
|------|------|
| `block/header.h/.cpp` | 52 字节 header 编解码 |
| `block/data_table.h/.cpp` | Data Table 构建（value 存储区） |
| `block/offset_table.h/.cpp` | Offset Table varint 序列编解码 |
| `block/block_builder.h/.cpp` | 接受 (InternalRow, all_key, value_payload) 构建 Block |
| `block/block_reader.h/.cpp` | 解析 Block，输出 InternalRow + data_table buffer |

**BlockBuilder 接口设计**：

```cpp
class BlockBuilder {
public:
    explicit BlockBuilder(const InternalSchema& schema, const BlockOptions& opts);

    // 添加一行。row 是纯 M+7 列，all_key 是排序键，payload 是 embedded value bytes。
    // separated value 的 payload 为空（bytes 已写入 value file）。
    // 返回 false 表示 block 已满，该行未被接受。
    bool add(const InternalRow& row, std::string_view all_key, std::string_view payload);

    // 序列化为完整 Block 字节。
    std::string finish();

    void reset();
    bool empty() const;
    size_t row_count() const;
    size_t estimated_size() const;
};
```

**BlockReader 接口设计**：

```cpp
class BlockReader {
public:
    static BlockReader open(std::string_view block_bytes, const InternalSchema& schema);

    size_t row_count() const;

    // 解码第 i 行的 M+7 列。
    InternalRow row(size_t i) const;

    // 从 data table 中按 (offset, length) 切片取 embedded value bytes。
    std::string_view value_payload(uint64_t offset, uint64_t length) const;
};
```

---

### Step 6: bloom/ — Bloom Filter

| 文件 | 职责 |
|------|------|
| `bloom/builder.h/.cpp` | Bloom Filter 构建 + 序列化 |
| `bloom/reader.h/.cpp` | Bloom Filter 反序列化 + 查询 |

---

### Step 7: metadata/ — Metadata Section

| 文件 | 职责 |
|------|------|
| `metadata/section.h/.cpp` | 通用 section 编解码（magic + checksum + map entries） |
| `metadata/configuration.h/.cpp` | CFIG section |
| `metadata/schema_section.h/.cpp` | SEMA section |
| `metadata/statistics.h/.cpp` | STAT section |

---

### Step 8: index/ — Index Tree

| 文件 | 职责 |
|------|------|
| `index/tree_builder.h/.cpp` | 索引树构建器（后序遍历布局） |
| `index/tree_reader.h/.cpp` | 索引树遍历 + 二分查找 |

---

### Step 9: file/ — 文件级组装

| 文件 | 职责 |
|------|------|
| `file/tail.h/.cpp` | Tail 编解码（32 字节） |
| `file/locator.h/.cpp` | Locator 编解码 |
| `file/value_file.h/.cpp` | Value File 读写 |
| `file/sstable_builder.h/.cpp` | SSTable 写入器 |
| `file/sstable_reader.h/.cpp` | SSTable 读取器 |

**SSTableBuilder 接口设计**：

```cpp
class SSTableBuilder {
public:
    SSTableBuilder(Schema schema, std::string key_file_path, BuilderOptions opts);

    // 按 all-key 严格递增顺序添加行。
    // 内部负责：
    //   1. 校验 row 与 schema 匹配
    //   2. 计算 all_key（memcomparable encoding）
    //   3. 决定 embedded/separated
    //   4. 构造 InternalRow（填充 M+7 列）
    //   5. 调用 BlockBuilder::add(row, all_key, payload)
    //   6. Block 满时 flush，注册到 IndexTreeBuilder
    //   7. 收集 all_key 到 BloomFilterBuilder
    Status add(const Row& row);

    Status finish();
    void abort();
};
```

---

## 5. 当前进度

| Step | 状态 | 说明 |
|------|------|------|
| Step 1: codec/ | ✅ 完成 | endian, varint, comparable 全部实现并测试通过 |
| Step 2: types/ | ✅ 完成 | 全部类型定义完成并测试通过 |
| Step 3: pattern/ | 待实现 | |
| Step 4: compress/ | 待实现 | |
| Step 5: block/ | 待实现 | |
| Step 6: bloom/ | 待实现 | |
| Step 7: metadata/ | 待实现 | |
| Step 8: index/ | 待实现 | |
| Step 9: file/ | 待实现 | |

---

## 6. 测试策略

每个模块的测试放在 `ut/{module}/` 目录下，由顶层 `cpp/pl/sstv2/BUILD` 中的 `cc_test` target 引用。

| 层级 | 范围 | 示例 |
|------|------|------|
| Unit | 单个函数/类 | varint round-trip, ColumnFlag constexpr 验证 |
| Integration | 模块间协作 | BlockBuilder + Pattern0 → BlockReader 解析 |
| End-to-End | 完整读写 | SSTableBuilder 写 N 行 → SSTableReader 全部读回验证 |

---

## 7. 构建系统

使用 Bazel，每个子目录一个 BUILD 文件，细粒度 `cc_library` target。

外部依赖：`@googletest` (测试)。CRC32C 待定（可能用 absl 或自实现）。

---

## 8. 编码规范

- 命名空间：`pl::sstv2::{module}`
- C++17/20，不使用异常
- `#pragma once`
- 所有磁盘格式使用 little-endian
- 文件头使用 nvim CopyRight 命令生成
- 优先 header-only，只在必要时分离 .cpp
- constexpr 优先：能在编译期求值的绝不推迟到运行时
