# SSTableV2 设计规格书

> **版本**: 1.0
> **实现**: C++20，全新实现于 `cpp/pl/sst2/`

---

## 1. 设计目标与原则

| 原则 | 描述 |
|------|------|
| **职责清晰** | 每个模块承担单一、明确的职责，接口最小化且正交。 |
| **尽可能零拷贝** | 读取路径使用 `std::span<const std::byte>` 和 `std::string_view`；写入路径使用结构化构建器。 |
| **不使用异常做控制流** | 所有可恢复错误通过 `absl::Status` 和 `absl::StatusOr<T>` 传递。 |
| **写入后不可变** | SST 文件一经写入即不可变。Reader 天然线程安全，Builder 为单线程使用。 |
| **模式编码优先** | 模式存储不是事后优化——它是列数据存储的根本方式。每个列存储单元均以模式编码。 |
| **模式驱动一切** | 列布局、子列分解、比较语义全部由 Schema 派生。 |
| **GC 容错** | 文件写入无需临时文件与原子重命名，写入中断产生的不完整文件由上层 GC 机制负责清理。 |

### 本文档覆盖范围

本文档提供 SSTableV2 格式的完整模块级设计，定义了模块边界、核心类接口、数据流和构建系统结构。

本文档**不**覆盖 MemTable、WAL 或 Compaction——它们是 SSTableV2 的消费者。

### 与 `cpp/pl/sst/` 的关系

本实现为**全新设计**，不复用现有 SST V1 的任何代码。V1 格式为 LevelDB 风格的行存储；V2 为列存储，架构根本不同。

---

## 2. 库选型：Abseil vs Folly

### 选择：**Abseil** (`abseil-cpp`)

### 理由

**1. `absl::Cord` 对数据表构建有独特价值**

多轮前缀压缩算法生成的字符串为碎片化片段（前缀 + 后缀片段）。`absl::Cord` 专为高效表示拼接的字符串碎片而设计，无需拷贝——这与数据表的物理布局直接对应：一个逻辑值可能分散在多轮压缩中。Folly 有 `folly::IOBuf` 用于 I/O 缓冲区链，但 `absl::Cord` 提供的是字符串级别的抽象，更契合需求。

**2. `absl::Status` / `absl::StatusOr` 具有更丰富的错误语义**

`absl::Status` 支持错误码、消息和通过 `SetPayload()` 附加的类型化载荷。这允许通过调用栈传播结构化错误信息（例如"CRC 校验不匹配，偏移量 X"）。配合 `ABSL_RETURN_IF_ERROR` 和 `ABSL_ASSIGN_OR_RETURN` 宏，错误处理简洁高效。

**3. 存储格式需要稳定性保证**

Abseil 提供正式的 LTS 发布，具有 3 年兼容性保证。存储格式库必须稳定——文件格式的 bug 不可恢复。

**4. 模块化链接体积**

Abseil 设计为细粒度链接：`//absl/status`、`//absl/strings`、`//absl/container/*` 为独立构建目标。Folly 的依赖图更为紧耦合。

**5. 与存储生态对齐**

Abseil 是 Protocol Buffers、gRPC 和 Google 存储技术栈的基础库。其设计模式（`Cord`、`Span`、`StatusOr`）在存储系统场景中经过了充分验证。

### Abseil 使用清单

| 组件 | 在 SSTableV2 中的用途 |
|------|----------------------|
| `absl::Status` / `absl::StatusOr<T>` | 所有错误处理 |
| `absl::Cord` | 数据表缓冲区管理 |
| `absl::Span<T>` | 编码数据的零拷贝视图 |
| `absl::InlinedVector<T, N>` | 小尺寸优化向量（块级缓冲区） |
| `absl::FixedArray<T>` | 栈分配的临时数组 |
| `absl::flat_hash_map` / `absl::btree_map` | 元数据存储、索引查找 |
| `absl::bit_cast` | 整数编码的安全类型双关 |
| `absl::crc32c` | CRC32C 校验和 |

### 依赖声明

```python
# MODULE.bazel
bazel_dep(name = "abseil-cpp", version = "20250127.1")
```

---

## 3. 目录结构

```
cpp/pl/sst2/
├── BUILD
├── docs/
│   └── spec.md
│
├── types/                         # 类型系统：Variant、Schema、常量
│   ├── BUILD
│   ├── data_type.h                # DataType 枚举 + 分类特性
│   ├── variant.h                  # Variant 类声明
│   ├── variant.cpp
│   ├── schema.h                   # ExternalSchema、InternalSchema
│   ├── schema.cpp
│   ├── internal_row.h             # InternalRow：行的类型化封装
│   ├── constants.h                # 所有魔数、版本常量
│   └── flag.h                     # Flag 位域（DataType + C + B 位）
│
├── encode/                        # 整数编码层
│   ├── BUILD
│   ├── varints.h                  # 变长整数 + ZigZag 编解码
│   ├── varints.cpp
│   ├── stream_vbyte.h             # Stream VByte 编解码（Pattern 1 使用）
│   ├── stream_vbyte.cpp
│   └── fixed.h                    # 定长小端编码（基于 memcpy）
│
├── pattern/                       # 模式存储：7 种编码模式
│   ├── BUILD
│   ├── pattern_encoder.h          # PatternEncoder 抽象接口
│   ├── pattern_decoder.h          # PatternDecoder 抽象接口
│   ├── pattern_selector.h         # 自动选择最佳模式
│   ├── pattern_selector.cpp
│   ├── pattern_none.h/cpp         # 模式 0：无编码
│   ├── pattern_stream_vbyte.h/cpp # 模式 1：Stream VByte
│   ├── pattern_pfor.h/cpp         # 模式 2：PFOR
│   ├── pattern_dict.h/cpp         # 模式 3：字典编码
│   ├── pattern_delta.h/cpp        # 模式 4 & 5：等步长递增/递减
│   └── pattern_compound.h/cpp     # 复合模式（用于组合类型）
│
├── compress/                      # 字符串与块压缩
│   ├── BUILD
│   ├── multi_prefix.h             # 多轮前缀压缩
│   ├── multi_prefix.cpp
│   ├── block_compressor.h         # Snappy/ZSTD 薄封装
│   └── block_compressor.cpp
│
├── block/                         # 块构建与读取
│   ├── BUILD
│   ├── block_header.h/cpp         # 52 字节块头
│   ├── data_table.h/cpp           # 数据表（变长区域）
│   ├── column_store.h/cpp         # 列存储区域（模式编码）
│   ├── offset_table.h/cpp         # 偏移量表（变长整数编码）
│   ├── rowkey_bitmap.h/cpp        # 行键重复位图
│   ├── block_writer.h/cpp         # BlockWriter：组装完整块
│   └── block_reader.h/cpp         # BlockReader：解析与查询块
│
├── index/                         # 索引树：多级 B 树索引
│   ├── BUILD
│   ├── index_block_writer.h/cpp   # 索引块写入器
│   ├── index_tree_builder.h/cpp   # 索引树构建器（后序遍历）
│   └── index_iterator.h/cpp       # 两级迭代器（范围扫描）
│
├── bloom/                         # 布隆过滤器
│   ├── BUILD
│   ├── bloom_format.h             # 磁盘布隆过滤器格式
│   ├── bloom_builder.h/cpp        # 布隆过滤器构建器
│   └── bloom_reader.h/cpp         # 布隆过滤器读取器
│
├── metadata/                      # 元数据段
│   ├── BUILD
│   ├── metadata_section.h/cpp     # 通用段格式（魔数 + 校验和 + 映射）
│   ├── configuration.h/cpp        # 配置段
│   ├── schema_meta.h/cpp          # Schema 元数据段
│   ├── statistics.h/cpp           # 统计信息段
│   ├── compatibility.h/cpp        # 兼容性段
│   └── user_defined.h/cpp         # 用户自定义数据段
│
├── file/                          # 文件层：构建器、读取器、定位器、尾部
│   ├── BUILD
│   ├── locator.h/cpp              # Locator 段
│   ├── tail.h/cpp                 # Tail（32 字节文件尾）
│   ├── value_file_writer.h/cpp    # Value 文件写入逻辑
│   ├── value_file_reader.h/cpp    # Value 文件读取逻辑
│   ├── sstable_builder.h/cpp      # 顶层 SST 文件构建器
│   └── sstable_reader.h/cpp       # 顶层 SST 文件读取器
│
└── ut/                            # 单元测试
    ├── BUILD
    ├── types/
    │   ├── variant_test.cpp
    │   └── schema_test.cpp
    ├── encode/
    │   ├── varints_test.cpp
    │   └── stream_vbyte_test.cpp
    ├── pattern/
    │   ├── pattern_none_test.cpp
    │   ├── pattern_stream_vbyte_test.cpp
    │   ├── pattern_pfor_test.cpp
    │   ├── pattern_dict_test.cpp
    │   ├── pattern_delta_test.cpp
    │   └── pattern_compound_test.cpp
    ├── compress/
    │   └── multi_prefix_test.cpp
    ├── block/
    │   ├── block_writer_test.cpp
    │   └── block_reader_test.cpp
    ├── index/
    │   └── index_tree_test.cpp
    ├── bloom/
    │   └── bloom_test.cpp
    └── file/
        └── sstable_test.cpp       # 集成测试：构建 + 读取往返
```

### 依赖关系图

```
                          ┌─────────────┐
                          │   file/     │  （顶层构建器/读取器）
                          └──────┬──────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                  │
     ┌────────▼──────┐  ┌───────▼───────┐  ┌───────▼──────┐
     │  metadata/    │  │   bloom/      │  │   index/     │
     └───────┬───────┘  └───────┬───────┘  └───────┬──────┘
             │                  │                   │
             └──────────────────┼───────────────────┘
                                │
                        ┌───────▼───────┐
                        │    block/     │
                        └───────┬───────┘
                                │
              ┌─────────────────┼─────────────────┐
              │                 │                  │
     ┌────────▼──────┐  ┌──────▼──────┐   ┌───────▼──────┐
     │  pattern/     │  │  compress/  │   │   encode/    │
     └───────┬───────┘  └──────┬─────┘   └───────┬──────┘
             │                  │                  │
             └──────────────────┼──────────────────┘
                                │
                        ┌───────▼───────┐
                        │    types/     │  （叶子模块，无内部依赖）
                        └───────────────┘
```

**规则：**
- `types/` 不依赖任何内部模块（仅依赖 Abseil）
- `encode/` 仅依赖 `types/`
- `pattern/` 依赖 `types/` 和 `encode/`
- `compress/` 仅依赖 `types/`
- `block/` 依赖 `types/`、`encode/`、`pattern/`、`compress/`
- `index/` 依赖 `block/`
- `bloom/` 依赖 `types/`
- `metadata/` 依赖 `types/` 和 `encode/`
- `file/` 依赖所有其他模块

---

## 4. 类型系统模块 (`types/`)

叶子模块。所有其他模块均依赖于此。

### 4.1 `DataType` 枚举 (`data_type.h`)

```cpp
namespace pl::sst2 {

enum class DataType : uint8_t {
  // 空类型
  kNone = 0,

  // 布尔
  kBool = 1,

  // 有符号整数
  kInt8 = 2, kInt16 = 4, kInt32 = 6, kInt64 = 8,

  // 无符号整数
  kUint8 = 3, kUint16 = 5, kUint32 = 7, kUint64 = 9,

  // 浮点数
  kFloat = 10, kDouble = 11, kLongDouble = 12,

  // 时间类型
  kTime = 13, kVersion = 14,

  // 字符串类型（变长）
  kString = 15, kU16String = 16, kU32String = 17, kBinary = 18,

  // 复合类型
  kArray = 19, kMap = 20,

  // 私有类型（仅内部使用，不出现在用户 Schema 中）
  kDataBlock = 21, kIndexBlock = 22,
};

// === 分类函数 ===

constexpr bool is_none(DataType t)        { return t == DataType::kNone; }
constexpr bool is_bool(DataType t)        { return t == DataType::kBool; }
constexpr bool is_signed_integer(DataType t);
constexpr bool is_unsigned_integer(DataType t);
constexpr bool is_integral(DataType t);
constexpr bool is_floating_point(DataType t);
constexpr bool is_fixed_size(DataType t);    // 所有算术类型 + Bool + Time + Version
constexpr bool is_variable_size(DataType t);  // 字符串类型 + Binary
constexpr bool is_string_type(DataType t);
constexpr bool is_compound(DataType t);      // Array、Map
constexpr bool is_private(DataType t);       // DataBlock、IndexBlock
constexpr bool is_public(DataType t);        // 非 None 且非私有

// 返回定长类型的字节数，否则返回 0
constexpr size_t fixed_size_in_bytes(DataType t);

// 用于调试的可读名称
constexpr std::string_view data_type_name(DataType t);

}  // namespace pl::sst2
```

### 4.2 `Variant` 类 (`variant.h`)

类型安全的值容器，持有任何 `DataType` 值。

```cpp
namespace pl::sst2 {

class Variant {
 public:
  // === 工厂方法 ===
  static Variant none();
  static Variant boolean(bool v);
  static Variant int8(int8_t v);
  static Variant int16(int16_t v);
  static Variant int32(int32_t v);
  static Variant int64(int64_t v);
  static Variant uint8(uint8_t v);
  static Variant uint16(uint16_t v);
  static Variant uint32(uint32_t v);
  static Variant uint64(uint64_t v);
  static Variant float32(float v);
  static Variant float64(double v);
  static Variant time(int64_t microseconds);
  static Variant version(uint64_t v);
  static Variant string(std::string_view s);
  static Variant binary(std::span<const std::byte> b);

  // === 访问器 ===
  DataType type() const;
  bool is_none() const;

  // 类型安全的值提取（类型不匹配时终止）
  bool     as_bool() const;
  int64_t  as_int() const;      // 任意有符号整数 → int64_t
  uint64_t as_uint() const;     // 任意无符号整数 → uint64_t
  double   as_float() const;    // 任意浮点数 → double
  std::string_view as_string() const;
  std::span<const std::byte> as_binary() const;

  // === 比较 ===
  std::strong_ordering operator<=>(const Variant& other) const;
  bool operator==(const Variant& other) const;

  // === 序列化 ===
  void encode_to(std::string& out) const;
  static absl::StatusOr<Variant> decode_from(DataType type,
                                              std::span<const std::byte> data);

 private:
  DataType type_;
  // 内部存储由实现决定
};

}  // namespace pl::sst2
```

**设计要点：**
- `as_int()` 将所有有符号整数宽化为 `int64_t`；`as_uint()` 类推
- 编码格式对定长类型为小端定长，对变长类型为长度前缀 + 数据
- `operator<=>` 实现与存储的排序语义一致

### 4.3 Flag 位域 (`flag.h`)

```cpp
namespace pl::sst2 {

// Flag 编码一个子列的元信息，紧凑为 1 字节：
//   [DataType:6][C:1][B:1]
// - DataType：子列的数据类型
// - C（Compound）：若为复合类型的嵌套子列则置 1
// - B（Bitmap）：若该子列携带 null 位图则置 1
struct Flag {
  DataType type : 6;
  bool compound_bit : 1;
  bool bitmap_bit : 1;

  static Flag from_byte(std::byte b);
  std::byte to_byte() const;
};

}  // namespace pl::sst2
```

### 4.4 Schema (`schema.h`)

```cpp
namespace pl::sst2 {

// 用户提供的列定义
struct ColumnDef {
  std::string name;
  DataType type;
  bool nullable = false;
  std::optional<DataType> element_type;  // Array 元素类型
  std::optional<DataType> key_type;      // Map 键类型
  std::optional<DataType> value_type;    // Map 值类型
};

// 外部 Schema（用户视角）
class ExternalSchema {
 public:
  explicit ExternalSchema(std::vector<ColumnDef> columns);

  size_t num_columns() const;
  const ColumnDef& column(size_t idx) const;
  std::optional<size_t> find_column(std::string_view name) const;

  // row_key 始终为第一列
  const ColumnDef& row_key_column() const;
};

// 内部 Schema：所有列分解为子列的扁平化表示
class InternalSchema {
 public:
  static InternalSchema from_external(const ExternalSchema& ext);

  size_t num_sub_columns() const;
  Flag flag(size_t sub_col_idx) const;
  std::string_view sub_column_name(size_t sub_col_idx) const;

  // 映射：外部列索引 → 子列范围 [start, end)
  std::pair<size_t, size_t> sub_column_range(size_t ext_col_idx) const;
};

}  // namespace pl::sst2
```

### 4.5 子列分解规则

所有类型最终归约为算术类型（可直接模式编码），规则如下：

| 外部类型 | 分解后的子列 |
|----------|-------------|
| 定长类型（Bool、Int*、Uint*、Float、Double、Time、Version）| 1 个子列：值本身 |
| String / Binary | 2 个子列：长度（uint32）+ 数据（string/binary） |
| Array | 3 个子列：元素个数（uint32）+ 偏移量（uint32）+ 元素值（递归分解） |
| Map | 5 个子列：条目数（uint32）+ 键偏移量 + 键值 + 值偏移量 + 值（均递归分解） |

**关键不变量：** 分解后每个子列要么是定长算术类型（可直接进入模式编码），要么是变长字符串（进入多轮前缀压缩后存入数据表）。

### 4.6 `InternalRow` (`internal_row.h`)

```cpp
namespace pl::sst2 {

class InternalRow {
 public:
  InternalRow(const InternalSchema& schema);

  void set(size_t sub_col_idx, Variant value);
  void set_null(size_t sub_col_idx);

  const Variant& get(size_t sub_col_idx) const;
  bool is_null(size_t sub_col_idx) const;

  std::string_view row_key() const;

 private:
  const InternalSchema& schema_;
  std::vector<Variant> values_;
  std::vector<bool> null_flags_;
};

}  // namespace pl::sst2
```

### 4.7 常量 (`constants.h`)

```cpp
namespace pl::sst2 {

// 文件格式标识
constexpr uint32_t kSstMagic = 0x53535432;  // "SST2"
constexpr uint16_t kFormatVersion = 1;

// 块大小限制
constexpr size_t kDefaultBlockSize = 64 * 1024;       // 64 KB
constexpr size_t kMinBlockSize = 4 * 1024;            // 4 KB
constexpr size_t kMaxBlockSize = 1024 * 1024;         // 1 MB

// 索引相关
constexpr size_t kDefaultIndexBlockSize = 4 * 1024;   // 4 KB
constexpr size_t kMaxIndexLevels = 8;

// 布隆过滤器
constexpr size_t kDefaultBloomBitsPerKey = 10;

// KV 分离阈值
constexpr size_t kDefaultValueSizeThreshold = 1024;   // 1 KB

// 校验和
constexpr uint32_t kCrc32cSeed = 0;

// Tail 大小
constexpr size_t kTailSize = 32;

}  // namespace pl::sst2
```

---

## 5. 整数编码模块 (`encode/`)

提供所有整数到字节流的转换原语。

### 5.1 变长整数 (`varints.h`)

```cpp
namespace pl::sst2 {

class Varints {
 public:
  // 编码：返回写入的字节数
  static size_t encode_uint32(uint32_t value, std::byte* dst);
  static size_t encode_uint64(uint64_t value, std::byte* dst);

  // 解码：返回 {解码值, 消耗字节数}
  static std::pair<uint32_t, size_t> decode_uint32(
      std::span<const std::byte> src);
  static std::pair<uint64_t, size_t> decode_uint64(
      std::span<const std::byte> src);

  // ZigZag 编码/解码（有符号 → 无符号映射）
  static uint32_t zigzag_encode32(int32_t v);
  static uint64_t zigzag_encode64(int64_t v);
  static int32_t zigzag_decode32(uint32_t v);
  static int64_t zigzag_decode64(uint64_t v);
};

}  // namespace pl::sst2
```

### 5.2 Stream VByte (`stream_vbyte.h`)

面向 SIMD 友好的批量整数压缩。将控制字节与数据字节分离存储。

```cpp
namespace pl::sst2 {

class StreamVByte {
 public:
  // 批量编码：N 个 uint32_t → 压缩字节流
  static size_t encode(std::span<const uint32_t> values, std::byte* dst);

  // 批量解码：从压缩流还原 N 个值
  static void decode(std::span<const std::byte> src, size_t count,
                     uint32_t* dst);

  // 计算 N 个值编码后的最大可能字节数
  static size_t max_encoded_size(size_t count);
};

}  // namespace pl::sst2
```

**编码格式：**
- 控制字节区：每个控制字节描述 4 个值各自占用的字节数（每值 2 位）
- 数据字节区：实际的值字节紧密排列
- 布局：`[控制字节区][数据字节区]`

### 5.3 定长编码 (`fixed.h`)

```cpp
namespace pl::sst2 {

template <typename T>
  requires std::is_arithmetic_v<T>
void encode_fixed(T value, std::byte* dst) {
  if constexpr (std::endian::native != std::endian::little) {
    value = byte_swap(value);
  }
  std::memcpy(dst, &value, sizeof(T));
}

template <typename T>
  requires std::is_arithmetic_v<T>
T decode_fixed(const std::byte* src) {
  T value;
  std::memcpy(&value, src, sizeof(T));
  if constexpr (std::endian::native != std::endian::little) {
    value = byte_swap(value);
  }
  return value;
}

}  // namespace pl::sst2
```

---

## 6. 模式存储模块 (`pattern/`)

模式编码是 SSTableV2 列存储的核心——它不是可选优化，而是所有列数据的存储方式。

### 6.1 七种编码模式

| 模式 ID | 名称 | 适用场景 | 随机访问 |
|---------|------|----------|----------|
| 0 | None | 无法归入其他模式 | O(1) 定长 / O(n) 变长 |
| 1 | Stream VByte | 通用整数序列 | O(n) 需顺序解码 |
| 2 | PFOR | 大部分值集中于窄范围 | O(1) 基于帧内索引 |
| 3 | Dictionary | 值域基数低（NDV 小） | O(1) 索引查表 |
| 4 | Equal-step Increment | 等步长递增序列 | O(1) base + idx × step |
| 5 | Equal-step Decrement | 等步长递减序列 | O(1) base - idx × step |
| 6 | Constant | 所有值相同 | O(1) 仅存一份 |

### 6.2 模式选择算法 (`pattern_selector.h`)

```cpp
namespace pl::sst2 {

class PatternSelector {
 public:
  // 分析一列值，返回最佳编码模式和对应的编码器
  struct Selection {
    uint8_t pattern_id;
    std::unique_ptr<PatternEncoder> encoder;
    size_t estimated_size;  // 预估编码后字节数
  };

  // values 为该列在当前块内的所有值
  static Selection select(std::span<const uint64_t> values, DataType type);

 private:
  // 内部：逐模式试编码并选最小者
  static bool try_constant(std::span<const uint64_t> values, Selection& out);
  static bool try_delta(std::span<const uint64_t> values, Selection& out);
  static bool try_dict(std::span<const uint64_t> values, Selection& out);
  static bool try_pfor(std::span<const uint64_t> values, Selection& out);
};

}  // namespace pl::sst2
```

**选择策略：** 按优先级尝试 Constant → Delta → Dictionary → PFOR → Stream VByte → None。选择预估编码尺寸最小的模式；若差异不超过 5%，优先选择支持 O(1) 随机访问的模式。

### 6.3 编码器/解码器接口

```cpp
namespace pl::sst2 {

class PatternEncoder {
 public:
  virtual ~PatternEncoder() = default;

  // 编码一列值到输出缓冲区
  virtual absl::Status encode(std::span<const uint64_t> values,
                              std::string& output) = 0;

  // 返回模式 ID
  virtual uint8_t pattern_id() const = 0;
};

class PatternDecoder {
 public:
  virtual ~PatternDecoder() = default;

  // 从编码数据创建解码器
  static std::unique_ptr<PatternDecoder> create(
      uint8_t pattern_id, std::span<const std::byte> data, size_t count);

  // 顺序访问
  virtual uint64_t get(size_t index) const = 0;

  // 批量解码（性能关键路径）
  virtual void get_batch(size_t start, size_t count, uint64_t* dst) const = 0;

  // 该模式是否支持 O(1) 随机访问
  virtual bool supports_random_access() const = 0;
};

}  // namespace pl::sst2
```

### 6.4 PFOR 编码详解

PFOR (Patched Frame of Reference) 适用于大部分值集中在一个较小范围内，但存在少量异常值的场景。

**编码结构：**
- `base`（参考值）：列中的最小值
- `bit_width`：覆盖大部分值所需的位宽
- `packed_values`：(value - base) 按 bit_width 位宽紧密排列
- `exceptions`：超出 bit_width 范围的值单独存储为 (index, full_value) 对
- `exception_count`：异常值数量

**随机访问：** 第 i 个值 = base + packed_values[i]（若非异常值），或从异常列表查找。由于异常稀少且有序，可二分查找。

### 6.5 复合类型的模式编码 (`pattern_compound.h`)

复合类型（Array、Map）分解为多个子列后，每个子列独立进行模式选择和编码。`CompoundPatternEncoder` 负责协调多个子列的编码流程。

---

## 7. 字符串与块压缩模块 (`compress/`)

### 7.1 多轮前缀压缩 (`multi_prefix.h`)

变长字符串列的主要压缩手段。核心思想：对有序字符串列进行多轮前缀消除，大幅降低冗余。

```cpp
namespace pl::sst2 {

class MultiPrefixCompressor {
 public:
  struct Config {
    size_t max_rounds = 4;              // 最多压缩轮数
    size_t min_prefix_len = 4;          // 参与压缩的最小前缀长度
    double min_compression_ratio = 0.1; // 单轮最小压缩收益阈值
  };

  explicit MultiPrefixCompressor(Config config = {});

  // 压缩一组有序字符串
  // 返回：{压缩数据, 重建所需的前缀目录}
  struct CompressResult {
    absl::Cord compressed_data;
    std::vector<std::string> prefix_directory;
    size_t num_rounds;
  };

  absl::StatusOr<CompressResult> compress(
      std::span<const std::string_view> sorted_strings);

  // 解压：给定压缩数据和前缀目录，还原第 idx 个字符串
  static absl::StatusOr<std::string> decompress_one(
      const absl::Cord& data,
      const std::vector<std::string>& prefix_directory,
      size_t idx);
};

}  // namespace pl::sst2
```

**压缩流程：**
1. 第一轮：计算每对相邻字符串的最长公共前缀（LCP），提取高频前缀建立前缀目录
2. 对每个字符串，编码为 (prefix_id, suffix)，其中 suffix 为去除前缀后的剩余部分
3. 对所有 suffix 重复上述过程（第二轮、第三轮...），直至收益低于阈值或达到最大轮数
4. 最终数据存入数据表的 `absl::Cord` 缓冲区

### 7.2 块级压缩 (`block_compressor.h`)

对已编码的数据块进行通用压缩（整块压缩，非列级别）。

```cpp
namespace pl::sst2 {

enum class CompressionType : uint8_t {
  kNone = 0,
  kSnappy = 1,
  kZstd = 2,
};

class BlockCompressor {
 public:
  static absl::StatusOr<std::string> compress(
      CompressionType type,
      std::span<const std::byte> input);

  static absl::StatusOr<std::string> decompress(
      CompressionType type,
      std::span<const std::byte> compressed,
      size_t uncompressed_size);

  // 返回给定类型的最大压缩膨胀上界
  static size_t max_compressed_size(CompressionType type, size_t input_size);
};

}  // namespace pl::sst2
```

---

## 8. 块模块 (`block/`)

块是 SSTableV2 中数据的基本 I/O 单元。每个块包含一批行的列数据。

### 8.1 块头 (`block_header.h`)

52 字节定长头部：

```cpp
namespace pl::sst2 {

struct BlockHeader {
  uint32_t magic;                // 块魔数（数据块 vs 索引块）
  uint32_t checksum;             // 块内容的 CRC32C（不含头部自身）
  uint32_t uncompressed_size;    // 解压后的有效载荷大小
  uint32_t compressed_size;      // 压缩后的有效载荷大小（0 = 无压缩）
  uint16_t num_rows;             // 块内行数
  uint16_t num_sub_columns;      // 子列数量
  CompressionType compression;   // 压缩类型
  uint8_t reserved[3];           // 保留字段
  uint64_t first_row_key_offset; // 第一个 row_key 在数据表中的偏移量
  uint64_t last_row_key_offset;  // 最后一个 row_key 在数据表中的偏移量
  uint32_t data_table_size;      // 数据表区域大小
  uint32_t column_store_size;    // 列存储区域大小
  uint32_t offset_table_size;    // 偏移量表大小

  // 序列化/反序列化
  void encode_to(std::byte* dst) const;
  static absl::StatusOr<BlockHeader> decode_from(
      std::span<const std::byte> src);
};

}  // namespace pl::sst2
```

### 8.2 块物理布局

```
┌──────────────────────────────────────────────┐
│              Block Header (52B)               │
├──────────────────────────────────────────────┤
│            Payload (已压缩或原始)              │
│  ┌────────────────────────────────────────┐  │
│  │         Data Table (变长数据)           │  │
│  ├────────────────────────────────────────┤  │
│  │      Column Store (模式编码数据)        │  │
│  ├────────────────────────────────────────┤  │
│  │         Offset Table (偏移量)           │  │
│  ├────────────────────────────────────────┤  │
│  │       RowKey Bitmap (重复标记)          │  │
│  └────────────────────────────────────────┘  │
└──────────────────────────────────────────────┘
```

### 8.3 数据表 (`data_table.h`)

存储所有变长数据（字符串列经多轮前缀压缩后的结果）。

```cpp
namespace pl::sst2 {

class DataTableBuilder {
 public:
  // 追加一个变长值，返回其在数据表中的偏移量
  size_t append(std::string_view data);
  size_t append(const absl::Cord& data);

  // 构建最终数据表
  absl::Cord build();

  size_t current_size() const;
};

class DataTableReader {
 public:
  explicit DataTableReader(std::span<const std::byte> data);

  // 根据偏移量和长度提取值
  std::string_view get(size_t offset, size_t length) const;
};

}  // namespace pl::sst2
```

### 8.4 列存储 (`column_store.h`)

存储所有子列的模式编码数据，每个子列一个独立的编码段。

```cpp
namespace pl::sst2 {

class ColumnStoreBuilder {
 public:
  explicit ColumnStoreBuilder(const InternalSchema& schema);

  // 逐行添加子列值
  void add_row(size_t sub_col_idx, uint64_t value);
  void add_null(size_t sub_col_idx);

  // 结束当前块的列存储构建
  // 对每个子列执行模式选择 + 编码
  absl::StatusOr<std::string> build();

  // 获取各子列的编码元信息（模式 ID、偏移量等）
  struct SubColumnMeta {
    uint8_t pattern_id;
    uint32_t offset;      // 在列存储区域内的偏移量
    uint32_t size;        // 编码后大小
    bool has_bitmap;      // 是否含 null 位图
  };
  std::span<const SubColumnMeta> sub_column_metas() const;
};

class ColumnStoreReader {
 public:
  ColumnStoreReader(std::span<const std::byte> data,
                    std::span<const ColumnStoreBuilder::SubColumnMeta> metas);

  // 获取第 row_idx 行第 sub_col_idx 子列的值
  uint64_t get(size_t sub_col_idx, size_t row_idx) const;
  bool is_null(size_t sub_col_idx, size_t row_idx) const;

  // 批量获取
  void get_batch(size_t sub_col_idx, size_t start, size_t count,
                 uint64_t* dst) const;
};

}  // namespace pl::sst2
```

### 8.5 偏移量表 (`offset_table.h`)

记录每行各变长子列在数据表中的偏移量和长度，使用 varint 编码以节省空间。

```cpp
namespace pl::sst2 {

class OffsetTableBuilder {
 public:
  void add_entry(uint32_t offset, uint32_t length);
  std::string build();  // varint 编码的偏移量序列
};

class OffsetTableReader {
 public:
  explicit OffsetTableReader(std::span<const std::byte> data);

  struct Entry {
    uint32_t offset;
    uint32_t length;
  };

  Entry get(size_t idx) const;
  size_t count() const;
};

}  // namespace pl::sst2
```

### 8.6 行键位图 (`rowkey_bitmap.h`)

标记块内哪些行与前一行拥有相同的 row_key（row_key 重复时仅存储一次）。

```cpp
namespace pl::sst2 {

class RowKeyBitmapBuilder {
 public:
  void add(bool is_same_as_previous);
  std::string build();  // 位图编码
};

class RowKeyBitmapReader {
 public:
  explicit RowKeyBitmapReader(std::span<const std::byte> data, size_t num_rows);
  bool is_duplicate(size_t row_idx) const;
};

}  // namespace pl::sst2
```

### 8.7 BlockWriter (`block_writer.h`)

```cpp
namespace pl::sst2 {

class BlockWriter {
 public:
  struct Options {
    size_t target_block_size = kDefaultBlockSize;
    CompressionType compression = CompressionType::kSnappy;
  };

  explicit BlockWriter(const InternalSchema& schema, Options opts = {});

  // 添加一行。返回 false 表示块已满，需 flush。
  bool add_row(const InternalRow& row);

  // 完成当前块的构建，返回完整块数据
  absl::StatusOr<std::string> finish();

  // 当前块是否为空
  bool empty() const;

  // 当前块内的行数
  size_t num_rows() const;

  // 获取当前块的 first/last row_key
  std::string_view first_row_key() const;
  std::string_view last_row_key() const;

  // 重置，准备写下一个块
  void reset();
};

}  // namespace pl::sst2
```

### 8.8 BlockReader (`block_reader.h`)

```cpp
namespace pl::sst2 {

class BlockReader {
 public:
  // 从原始块数据（含头部）创建读取器
  static absl::StatusOr<BlockReader> open(std::span<const std::byte> block_data,
                                           const InternalSchema& schema);

  // 块内行数
  size_t num_rows() const;

  // 读取第 row_idx 行的第 sub_col_idx 子列
  absl::StatusOr<Variant> get(size_t row_idx, size_t sub_col_idx) const;

  // 按 row_key 在块内二分查找，返回行索引
  absl::StatusOr<std::optional<size_t>> find_row(std::string_view row_key) const;

  // 获取第 row_idx 行的 row_key
  std::string_view row_key(size_t row_idx) const;

  // 获取块头信息
  const BlockHeader& header() const;
};

}  // namespace pl::sst2
```

---

## 9. 索引模块 (`index/`)

多级 B 树索引，支持快速定位目标数据块。

### 9.1 索引块格式

每个索引块记录一批子节点（数据块或下级索引块）的路由信息：

```
┌─────────────────────────────────┐
│       Index Block Header        │
├─────────────────────────────────┤
│ Entry 0: [last_key | offset | size | sub_column_flags] │
│ Entry 1: [last_key | offset | size | sub_column_flags] │
│ ...                             │
│ Entry N: [last_key | offset | size | sub_column_flags] │
├─────────────────────────────────┤
│         Key Data Area           │
└─────────────────────────────────┘
```

### 9.2 索引树构建器 (`index_tree_builder.h`)

采用后序遍历（post-order）方式构建索引树：数据块写完后，叶级索引逐步收集路由条目，索引块满时向上层提交。

```cpp
namespace pl::sst2 {

class IndexTreeBuilder {
 public:
  struct Options {
    size_t index_block_size = kDefaultIndexBlockSize;
  };

  explicit IndexTreeBuilder(Options opts = {});

  // 数据块写入完成后，添加一条索引条目
  void add_data_block(std::string_view last_key,
                      uint64_t offset,
                      uint32_t size);

  // 结束索引树构建，返回所有索引块及根索引块偏移量
  struct BuildResult {
    std::vector<std::string> index_blocks;  // 后序排列
    uint64_t root_offset;
    uint8_t tree_height;
  };

  BuildResult finish();
};

}  // namespace pl::sst2
```

**后序遍历写入策略：**
1. 数据块依次写入文件
2. 叶级索引收集路由条目，满时写出叶级索引块
3. 叶级索引块的路由信息上推到 Level-1 索引
4. 递归进行直至根节点
5. 最终写入顺序：数据块 → 叶级索引块 → 中间索引块 → 根索引块

此策略确保单遍顺序写入，无需回填指针。

### 9.3 索引迭代器 (`index_iterator.h`)

```cpp
namespace pl::sst2 {

class IndexIterator {
 public:
  // 从 SST 文件的索引根开始创建迭代器
  static absl::StatusOr<IndexIterator> open(
      std::span<const std::byte> file_data,
      uint64_t root_offset,
      uint8_t tree_height);

  // 定位到 >= target_key 的第一个数据块
  absl::Status seek(std::string_view target_key);

  // 移动到下一个数据块
  absl::Status next();

  // 当前是否有效
  bool valid() const;

  // 获取当前数据块的偏移量和大小
  uint64_t current_block_offset() const;
  uint32_t current_block_size() const;
};

}  // namespace pl::sst2
```

---

## 10. 布隆过滤器模块 (`bloom/`)

支持对 row_key 的快速否定查询（point lookup 优化）。

### 10.1 磁盘格式 (`bloom_format.h`)

```
┌───────────────────────────────┐
│      Bloom Filter Header      │
│  - num_keys (uint32)          │
│  - num_bits (uint32)          │
│  - num_hash_funcs (uint8)     │
│  - hash_seed (uint32)         │
├───────────────────────────────┤
│       Bit Array Data          │
└───────────────────────────────┘
```

### 10.2 构建器 (`bloom_builder.h`)

```cpp
namespace pl::sst2 {

class BloomBuilder {
 public:
  explicit BloomBuilder(size_t expected_keys,
                        size_t bits_per_key = kDefaultBloomBitsPerKey);

  void add_key(std::string_view key);
  std::string finish();  // 返回完整的布隆过滤器数据（含头部）
};

}  // namespace pl::sst2
```

### 10.3 读取器 (`bloom_reader.h`)

```cpp
namespace pl::sst2 {

class BloomReader {
 public:
  static absl::StatusOr<BloomReader> open(std::span<const std::byte> data);

  // 返回 true 表示"可能存在"，false 表示"一定不存在"
  bool may_contain(std::string_view key) const;
};

}  // namespace pl::sst2
```

---

## 11. 元数据模块 (`metadata/`)

SST 文件末尾（Tail 之前）的多个元数据段，每段采用统一的封装格式。

### 11.1 通用段格式 (`metadata_section.h`)

每个元数据段的物理布局：

```
┌────────────────────────────────────────┐
│ Section Magic (4B)                     │
│ Section Length (4B, 不含自身)            │
├────────────────────────────────────────┤
│ Key-Value Pairs (TLV 编码)             │
│   [key_len:varint][key][value_len:varint][value] │
│   ...                                  │
├────────────────────────────────────────┤
│ CRC32C Checksum (4B)                   │
└────────────────────────────────────────┘
```

```cpp
namespace pl::sst2 {

class MetadataSection {
 public:
  void put(std::string_view key, std::string_view value);
  void put_uint64(std::string_view key, uint64_t value);
  void put_uint32(std::string_view key, uint32_t value);

  std::optional<std::string_view> get(std::string_view key) const;
  std::optional<uint64_t> get_uint64(std::string_view key) const;

  std::string serialize(uint32_t section_magic) const;
  static absl::StatusOr<MetadataSection> deserialize(
      std::span<const std::byte> data, uint32_t expected_magic);
};

}  // namespace pl::sst2
```

### 11.2 配置段 (`configuration.h`)

记录构建时的配置参数，用于读取时重建上下文：

| 键 | 类型 | 说明 |
|----|------|------|
| `block_size` | uint32 | 目标块大小 |
| `compression` | uint8 | 压缩算法 |
| `bloom_bits_per_key` | uint32 | 布隆过滤器位/键 |
| `value_size_threshold` | uint32 | KV 分离阈值 |
| `max_prefix_rounds` | uint8 | 前缀压缩最大轮数 |

### 11.3 Schema 元数据段 (`schema_meta.h`)

序列化完整的 `ExternalSchema`，使读取器无需外部元数据即可解析文件。

### 11.4 统计信息段 (`statistics.h`)

| 键 | 类型 | 说明 |
|----|------|------|
| `total_rows` | uint64 | 总行数 |
| `total_data_blocks` | uint32 | 数据块数量 |
| `total_index_blocks` | uint32 | 索引块数量 |
| `raw_data_size` | uint64 | 原始数据大小 |
| `compressed_data_size` | uint64 | 压缩后数据大小 |
| `min_row_key` | string | 最小 row_key |
| `max_row_key` | string | 最大 row_key |
| `creation_time` | uint64 | 创建时间戳（微秒） |
| `per_column_stats` | binary | 每列的 min/max/null_count 等 |

### 11.5 兼容性段 (`compatibility.h`)

记录文件的最低可读版本和特性标志，用于版本演进时的前向兼容判断：

| 键 | 类型 | 说明 |
|----|------|------|
| `min_reader_version` | uint16 | 读取该文件的最低 reader 版本 |
| `writer_version` | uint16 | 写入该文件的 writer 版本 |
| `feature_flags` | uint64 | 特性位图 |

### 11.6 用户自定义数据段 (`user_defined.h`)

供上层系统存储任意键值对（例如 Compaction 标记、层级信息等）。

---

## 12. 文件层模块 (`file/`)

顶层模块，组装所有子模块形成完整的 SST 文件。

### 12.1 SST 文件整体布局

```
┌─────────────────────────────────────────┐  偏移量 0
│          Data Block 0                   │
│          Data Block 1                   │
│          ...                            │
│          Data Block N-1                 │
├─────────────────────────────────────────┤
│          Index Block (叶级) 0            │
│          Index Block (叶级) 1            │
│          ...                            │
│          Index Block (中间级)            │
│          Index Block (根)               │
├─────────────────────────────────────────┤
│          Bloom Filter                   │
├─────────────────────────────────────────┤
│          Metadata: Configuration        │
│          Metadata: Schema               │
│          Metadata: Statistics           │
│          Metadata: Compatibility        │
│          Metadata: User Defined         │
├─────────────────────────────────────────┤
│          Locator Section                │
├─────────────────────────────────────────┤
│          Tail (32B)                     │
└─────────────────────────────────────────┘
```

### 12.2 Locator 段 (`locator.h`)

定位各区段在文件中的位置：

```cpp
namespace pl::sst2 {

struct LocatorEntry {
  uint32_t section_type;  // 区段类型标识
  uint64_t offset;        // 在文件中的起始偏移量
  uint64_t size;          // 区段大小
};

class Locator {
 public:
  void add(uint32_t section_type, uint64_t offset, uint64_t size);

  std::optional<LocatorEntry> find(uint32_t section_type) const;

  std::string serialize() const;
  static absl::StatusOr<Locator> deserialize(std::span<const std::byte> data);
};

}  // namespace pl::sst2
```

**区段类型标识：**

| 标识 | 区段 |
|------|------|
| 0x01 | 数据块区域 |
| 0x02 | 索引块区域 |
| 0x03 | 布隆过滤器 |
| 0x10 | 配置元数据 |
| 0x11 | Schema 元数据 |
| 0x12 | 统计信息 |
| 0x13 | 兼容性 |
| 0x14 | 用户自定义 |
| 0x20 | Value 文件引用 |

### 12.3 Tail (32 字节文件尾) (`tail.h`)

```cpp
namespace pl::sst2 {

struct Tail {
  uint32_t magic;             // kSstMagic = 0x53535432
  uint16_t format_version;    // 当前为 1
  uint16_t reserved;          // 保留
  uint64_t locator_offset;    // Locator 段的文件偏移量
  uint32_t locator_size;      // Locator 段的大小
  uint32_t locator_checksum;  // Locator 段的 CRC32C
  uint64_t file_checksum;     // 整个文件（不含 Tail 自身）的校验和

  static constexpr size_t kSize = 32;

  void encode_to(std::byte* dst) const;
  static absl::StatusOr<Tail> decode_from(std::span<const std::byte> src);
};

}  // namespace pl::sst2
```

**读取流程：** Reader 打开文件时，先读取最后 32 字节的 Tail，验证 magic 和 format_version，然后通过 locator_offset 定位 Locator 段，再从 Locator 找到各个区段的位置。

### 12.4 KV 分离：Value 文件 (`value_file_writer.h` / `value_file_reader.h`)

对于超过阈值的大 value（默认 1KB），将其存储在独立的 Value 文件中，SST 文件内仅存储引用。

```cpp
namespace pl::sst2 {

// Value 文件中的引用句柄
struct ValueHandle {
  uint64_t file_id;    // Value 文件标识
  uint64_t offset;     // 在 Value 文件中的偏移量
  uint32_t size;       // 值的大小
  uint32_t checksum;   // 值的 CRC32C
};

class ValueFileWriter {
 public:
  explicit ValueFileWriter(uint64_t file_id, std::string_view path);

  // 追加一个大 value，返回其引用句柄
  absl::StatusOr<ValueHandle> append(std::span<const std::byte> value);

  // 同步并关闭文件
  absl::Status finish();

  uint64_t current_size() const;
};

class ValueFileReader {
 public:
  static absl::StatusOr<ValueFileReader> open(std::string_view path);

  // 根据句柄读取值
  absl::StatusOr<std::string> read(const ValueHandle& handle) const;
};

}  // namespace pl::sst2
```

**KV 分离策略：**
- value 大小 ≤ 阈值：内联存储于数据表
- value 大小 > 阈值：写入 Value 文件，数据表中存储 `ValueHandle` 的序列化形式
- 内联与外置在编码层面通过 Flag 的一个标志位区分

### 12.5 SSTable 构建器 (`sstable_builder.h`)

```cpp
namespace pl::sst2 {

class SSTableBuilder {
 public:
  struct Options {
    size_t block_size = kDefaultBlockSize;
    CompressionType compression = CompressionType::kSnappy;
    size_t bloom_bits_per_key = kDefaultBloomBitsPerKey;
    size_t value_size_threshold = kDefaultValueSizeThreshold;
    std::string value_file_path;  // 若为空则不进行 KV 分离
  };

  SSTableBuilder(const ExternalSchema& schema,
                 std::string_view output_path,
                 Options opts = {});

  // 添加行（必须按 row_key 有序添加）
  absl::Status add(const InternalRow& row);

  // 完成构建，写入所有尾部结构
  absl::Status finish();

  // 中止构建（上层 GC 负责清理不完整文件）
  void abort();

  // 构建过程中的统计信息
  uint64_t total_rows() const;
  uint64_t data_size() const;
};

}  // namespace pl::sst2
```

**写入流程：**
1. 逐行添加数据，BlockWriter 将行拆分为子列并积攒
2. 块满时，BlockWriter 对每个子列执行模式选择+编码，组装完整块，写入文件
3. 每个数据块写入后，向 IndexTreeBuilder 和 BloomBuilder 提供路由信息
4. 所有数据写完后，依次 flush 索引树、布隆过滤器、元数据段、Locator、Tail
5. 写入过程中任何错误导致 `abort()` 被调用时，不完整文件保留在磁盘上，由上层 GC 机制清理

### 12.6 SSTable 读取器 (`sstable_reader.h`)

```cpp
namespace pl::sst2 {

class SSTableReader {
 public:
  // 打开一个 SST 文件
  static absl::StatusOr<SSTableReader> open(std::string_view path);

  // Point lookup：精确查找一个 row_key
  absl::StatusOr<std::optional<InternalRow>> get(std::string_view row_key) const;

  // Range scan：返回 [start_key, end_key) 范围内的迭代器
  absl::StatusOr<std::unique_ptr<Iterator>> scan(
      std::string_view start_key,
      std::string_view end_key) const;

  // 全量扫描
  absl::StatusOr<std::unique_ptr<Iterator>> scan_all() const;

  // 元数据访问
  const ExternalSchema& schema() const;
  const MetadataSection& statistics() const;
  const MetadataSection& configuration() const;

  // 文件验证状态
  bool is_valid() const;

  // 迭代器接口
  class Iterator {
   public:
    virtual ~Iterator() = default;
    virtual bool valid() const = 0;
    virtual absl::Status next() = 0;
    virtual std::string_view row_key() const = 0;
    virtual absl::StatusOr<Variant> column(size_t col_idx) const = 0;
  };
};

}  // namespace pl::sst2
```

**读取流程（Point Lookup）：**
1. 从 Tail 定位 Locator
2. 从 Locator 找到布隆过滤器位置，做否定过滤
3. 若可能存在，从 Locator 找到索引根
4. 沿索引树定位目标数据块
5. 读取数据块，块内二分查找 row_key
6. 若值为外置引用（ValueHandle），从 Value 文件读取

---

## 13. 失败恢复与 GC 机制

### 13.1 设计哲学

SSTableV2 采用"写入幂等 + GC 清理"的容错策略，而非传统的"临时文件 + 原子重命名"方式。理由如下：

- SST 文件一经写入即不可变，不存在部分更新的问题
- 写入中断产生的不完整文件可通过 Tail 校验轻松识别
- 上层系统（Compaction 调度器等）本身需要 GC 机制来管理文件生命周期，复用该机制即可
- 消除临时文件带来的额外文件系统操作和命名复杂度

### 13.2 不完整文件识别

一个 SST 文件是否完整可通过以下条件判断：

1. 文件大小是否 ≥ `kTailSize`（32 字节）
2. 最后 32 字节是否能成功解析为合法 Tail（magic 和 format_version 校验）
3. Tail 中的 `file_checksum` 是否与文件实际内容匹配

任何一条不满足，即判定为不完整文件，GC 可安全删除。

### 13.3 GC 接口契约

上层 GC 机制需实现以下逻辑：

```cpp
namespace pl::sst2 {

// 判断 SST 文件是否完整（供 GC 使用）
absl::StatusOr<bool> is_sst_file_complete(std::string_view path);

}  // namespace pl::sst2
```

GC 调度器定期扫描 SST 目录，对所有未注册在活跃文件清单中的文件调用 `is_sst_file_complete()`。不完整文件直接删除，完整但无引用的文件按正常过期策略回收。

### 13.4 Value 文件的 GC

Value 文件的清理同样依赖 GC：只有当引用该 Value 文件的所有 SST 文件均被删除后，Value 文件才可回收。引用关系通过 Locator 段中的 Value 文件引用条目追踪。

---

## 14. 构建系统与测试策略

### 14.1 Bazel 构建规则

每个子目录一个 `BUILD` 文件，遵循细粒度目标原则：

```python
# types/BUILD
cc_library(
    name = "data_type",
    hdrs = ["data_type.h"],
    deps = ["@abseil-cpp//absl/strings"],
)

cc_library(
    name = "variant",
    srcs = ["variant.cpp"],
    hdrs = ["variant.h"],
    deps = [
        ":data_type",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/types:span",
    ],
)

cc_library(
    name = "schema",
    srcs = ["schema.cpp"],
    hdrs = ["schema.h"],
    deps = [":data_type", ":variant"],
)
```

### 14.2 测试策略

| 层级 | 目标 | 方法 |
|------|------|------|
| 单元测试 | 各模块独立正确性 | 每个编解码器、每种模式独立测试 |
| 属性测试 | 编解码往返一致性 | 随机输入 → encode → decode → 比较 |
| 集成测试 | 端到端写入/读取 | 构建 SST → 读取验证所有行 |
| 压力测试 | 极端场景 | 全 NULL 列、单行块、超大 value、最大索引深度 |
| 模糊测试 | 健壮性 | 对 BlockReader 和 SSTableReader 喂入随机字节 |

### 14.3 性能基准

关键性能指标需持续跟踪：

| 指标 | 目标 |
|------|------|
| 顺序写入吞吐 | ≥ 200 MB/s（单线程） |
| Point lookup 延迟 | ≤ 10 μs（热数据） |
| Range scan 吞吐 | ≥ 500 MB/s（解压后） |
| 空间效率 | 相比原始数据 ≤ 60%（典型工作负载） |

---

## 附录 A：版本演进策略

### 前向兼容

新版本 writer 生成的文件可通过 `Compatibility` 段中的 `min_reader_version` 字段告知旧版本 reader 是否能读取。若旧 reader 版本低于 `min_reader_version`，应拒绝打开并返回明确错误。

### 后向兼容

新版本 reader 必须能读取所有旧版本 writer 生成的文件。通过 `feature_flags` 位图识别文件使用了哪些特性，对未知特性进行安全降级处理。

### 格式变更流程

1. 新增特性时，在 `feature_flags` 中分配新位
2. 若新特性为必需（旧 reader 无法忽略），递增 `min_reader_version`
3. 若新特性为可选（旧 reader 可安全跳过），仅设置 feature flag

---

## 附录 B：C++20 特性使用清单

| 特性 | 用途 |
|------|------|
| `std::span` | 零拷贝数据视图 |
| `std::bit_cast` | 安全类型双关（浮点 ↔ 整数） |
| `std::endian` | 编译期端序检测 |
| Concepts | 模板约束（`std::is_arithmetic_v<T>` 等） |
| `operator<=>` | 三路比较（Variant 排序） |
| Designated initializers | 结构体初始化（Options 等） |
| `[[likely]]` / `[[unlikely]]` | 性能关键路径的分支提示 |

---

## 附录 C：错误处理规范

### 错误码分类

| 错误码 | 含义 | 典型场景 |
|--------|------|----------|
| `kInvalidArgument` | 调用者传入非法参数 | 块大小超出范围 |
| `kDataLoss` | 数据完整性校验失败 | CRC 不匹配 |
| `kCorruption` | 文件结构损坏 | 魔数不匹配、长度溢出 |
| `kNotFound` | 目标不存在 | row_key 未命中 |
| `kResourceExhausted` | 资源不足 | 索引层级超过最大值 |
| `kInternal` | 内部逻辑错误 | 不应到达的代码路径 |

### 错误传播模式

```cpp
// 典型用法
absl::Status DoSomething() {
  ABSL_ASSIGN_OR_RETURN(auto header, BlockHeader::decode_from(data));
  ABSL_RETURN_IF_ERROR(validate_checksum(header, payload));
  // ...
  return absl::OkStatus();
}
```

所有公开 API 返回 `absl::Status` 或 `absl::StatusOr<T>`，内部断言仅用于不变量检查（违反则为 bug）。

---

## 附录 D：线程安全模型

| 组件 | 线程安全性 | 说明 |
|------|-----------|------|
| `SSTableReader` | 线程安全 | 只读操作，无共享可变状态 |
| `SSTableBuilder` | 非线程安全 | 单线程顺序写入 |
| `BlockReader` | 线程安全 | 只读 |
| `BlockWriter` | 非线程安全 | 单线程构建 |
| `BloomReader` | 线程安全 | 只读位数组 |
| `ValueFileReader` | 线程安全 | 只读，pread 访问 |

设计哲学：读取路径天然并发安全（不可变数据），写入路径单线程（无锁设计，最大化单线程吞吐）。
