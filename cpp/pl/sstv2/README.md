# SSTable v2

SSTable v2 是 LSM-tree SSTable 存储引擎的完全重写版本。采用模块化、类型安全的架构，
各子系统职责清晰分离：块编码、索引、键管理、压缩、布隆过滤器、文件 I/O 各自独立。

## 架构总览

```
file/        -- I/O Layer (Builder: writes key+value; Reader: get+scan)
  |
  +-- block/     -- Block encode/decode (BlockBuilder, BlockReader)
  +-- index/     -- Multi-level index tree (TreeBuilder, TreeReader)
  +-- bloom/     -- Bloom filter (Builder, Reader)
  +-- format/    -- File metadata (metadata, section, tail)
  |
  +-- types/     -- Data model (Value, Schema, Key, InternalRow, ...)
        |
        +-- codec/       -- Memcomparable encoding (scalar, value, fixed, varint)
        +-- compress/    -- Block compression (zstd, snappy, lz4, none)
        +-- pattern/     -- Pattern-based row encoding (raw, compound)
```

## 模块详解

### `types/` — 数据模型与键类型系统

基础层。定义所有值类型、Schema、Row、键以及比较器。

| 文件                | 用途                                                                                                                                                          |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `value.h`           | 标签联合体 `Value`，支持 25+ 数据类型（整数、浮点、字符串、时间、版本、数组、Map）。包含 `compare_values()`，提供类型检查的字典序比较。                       |
| `data_type.h`       | `DataType` 枚举与 `SortOrder`（升序/降序）。                                                                                                                  |
| `schema.h`          | 用户侧 `Schema`（列名、类型、排序键）+ `SchemaBuilder`。                                                                                                      |
| `internal_schema.h` | `InternalSchema` — 将用户 schema 映射到固定位置内部列，追加系统列（version, op_type, flags, filename, offset, length, checksum）。                            |
| `internal_row.h`    | `InternalRow` — 按内部 schema 列索引对齐的定长 `Value` 数组。                                                                                                 |
| `row.h`             | 用户侧 `Row` — 将列名映射到 `Value`，通过 `Schema` 校验。                                                                                                     |
| `key.h`             | **键类型系统** — 模板化的 `LogicalKey<Tag>`、`KeyView<Tag>`、`EncodedKey<Tag>`，三种标签作用域：`RowKey`、`AllKey`（全排序键）、`PrefixKey`（范围扫描边界）。 |
| `key_tags.h`        | 标签类型：`RowKeyTag`、`AllKeyTag`、`PrefixKeyTag`。                                                                                                          |
| `key_prefix.h`      | `KeyPrefix` 结构体 — 用于范围扫描边界的部分键（key_columns、可选 version、可选 op_type）。                                                                    |
| `key_comparator.h`  | `KeyComparator` — 使用 C++20 concept 约束的字典序比较器。支持精确匹配与前缀匹配两种模式，升序/降序排列。                                                      |
| `key_factory.h`     | 工厂函数：`make_all_key()`、`make_all_key_view()`、`make_prefix_key()` — 从 Row/Schema 构造键并做类型校验。                                                   |
| `value_codec.h`     | `encode_value()` / `decode_value()` — 将 `Value` 序列化为 memcomparable 字节。                                                                                |
| `op_type.h`         | `OpType` 枚举（kPut、kDelete 等）。                                                                                                                           |
| `column_flag.h`     | `ColumnFlag` — 索引条目位图标记（区分数据块与索引块）。                                                                                                       |

**键类型系统（3 x 3 矩阵）：**

每种键作用域有三种表达形式：

| 作用域                | 持有所有权 (`LogicalKey<Tag>`) | 零拷贝视图 (`KeyView<Tag>`) | 预编码 (`EncodedKey<Tag>`) |
| --------------------- | ------------------------------ | --------------------------- | -------------------------- |
| RowKey                | `RowKey`                       | `RowKeyView`                | `EncodedRowKey`            |
| AllKey（全排序键）    | `AllKey`                       | `AllKeyView`                | `EncodedAllKey`            |
| PrefixKey（范围边界） | `PrefixKey`                    | `PrefixKeyView`             | `EncodedPrefixKey`         |

`KeyComparator` 利用 C++20 concept 确保类型安全的比较 — 只有标签兼容的键才能编译通过：

```cpp
template <typename Lhs, typename Rhs>
    requires KeyWithTag<Lhs, AllKeyTag> && KeyWithTag<Rhs, AllKeyTag>
StatusOr<int> compare_all_key(const Lhs& lhs, const Rhs& rhs) const;
```

### `codec/` — 编解码层

双层编码体系：

| 文件                    | 用途                                                                                                                        |
| ----------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `scalar_comparable.h`   | 标量类型的底层 memcomparable 编码（整数、浮点、字符串、时间）。支持升序和降序。编码后的字节按位比较即得到原始值的正确排序。 |
| `scalar_comparable.cpp` | 实现：整数（大端 + 符号位翻转）、浮点（IEEE 754 符号/指数位操作）、字符串（0x00 字节转义）。                                |
| `value_comparable.h`    | 针对 `Value` 的高层编码 — 按 `DataType` 分发到标量编码。处理数组、Map 和可空类型。                                          |
| `fixed.h`               | 定长整数读写辅助函数。                                                                                                      |
| `endian.h`              | 字节序检测与字节交换工具。                                                                                                  |
| `varint.h`              | 变长整数编码（LEB128）。                                                                                                    |
| `checksum.h`            | xxHash64 校验和计算。                                                                                                       |

**Memcomparable 编码的核心设计思路：**

编码后 `memcmp(encode(a), encode(b))` 的结果与 `a` 和 `b` 的自然比较顺序完全一致，
因此编码后的键可直接用原始字节比较 — 这对布隆过滤器以及索引块中二分查找至关重要。

```
Integer (int64):   flip sign bit -> big-endian -> [8 bytes]
Float (double):    IEEE 754 -> invert sign+exponent bits for negatives -> [8 bytes]
String:            escape \x00 -> \x00\xFF, escape \xFF -> \xFF\x00, terminator \x00\x01
```

### `block/` — 数据与索引块编码

| 类             | 用途                                                                                                       |
| -------------- | ---------------------------------------------------------------------------------------------------------- |
| `BlockBuilder` | 收集 `InternalRow`，压缩编码为带偏移表的 Block，支持 O(1) 随机访问。                                       |
| `BlockReader`  | 打开已编码的 Block，解压，解析偏移表，按索引访问行。                                                       |
| `Header`       | 52 字节块头：魔数、标志位、行数、偏移表位置、压缩/未压缩长度、校验和。                                     |
| `Options`      | Block 类型（kData/kIndex/kRootIndex）、压缩配置、大小限制（软限制 64KB，硬限制 128KB）、最大行数（4096）。 |

**Block 磁盘布局：**

```
+--------------+------------------------+---------------------+------------------+
| Header (52B) | Compressed Payload     | Offset Table        | Footer (16B)     |
|              | (rows + embedded vals) | (4B per row)        | checksum + len   |
+--------------+------------------------+---------------------+------------------+
```

三种 Block 类型：

- **kData** (`DTBK`)：存储用户行数据及可选的嵌入值
- **kIndex** (`IXBK`)：存储索引条目（fence 行 + Block 引用）
- **kRootIndex** (`ROOT`)：顶层索引块

### `index/` — 多级索引树

| 类            | 用途                                                                                                                                                        |
| ------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `TreeBuilder` | 构建基于扇出（fanout）的多级索引树。每层非叶子节点存储指向子块的 fence 行。满层自动向上刷写。                                                               |
| `TreeReader`  | 遍历索引树：`scan_data_blocks()`（全扫描）、`scan_data_blocks_from()`（前缀扫描）、`scan_data_blocks_in_range()`（范围扫描）、`find_data_block()`（点查）。 |

**树结构：**

```
                         +--------------+
                         |  ROOT block  |  (kRootIndex)
                         +------+-------+
                                |
              +-----------------+-----------------+
              |                 |                 |
         +----v----+       +----v----+       +----v----+
         |  IXBK   |       |  IXBK   |       |  IXBK   |   Level 2 (kIndex)
         +----+----+       +----+----+       +----+----+
              |                 |                 |
        +-----+----+      +----+---+       +-----+----+
        |          |      |        |       |          |
   +----v-+   +----v-+  +-v--+  +--v-+  +--v-+   +---v-+
   | DTBK |   | DTBK |  |DTBK|  |DTBK|  |DTBK|   |DTBK |   Level 0 (kData)
   +------+   +------+  +----+  +----+  +----+   +-----+
```

索引条目是带列标记的 `InternalRow`，标记该条目指向数据块还是索引块。索引树与数据块内联存储在 key 文件中。

### `bloom/` — 布隆过滤器

| 类        | 用途                                                                       |
| --------- | -------------------------------------------------------------------------- |
| `Builder` | 收集编码后的 all-key，构建布隆过滤器位数组，可配置每键位数（默认 10 位）。 |
| `Reader`  | 查询布隆过滤器，判断某键是否**可能**存在于该 SSTable 中。                  |

直接使用编码后的 `AllKey` 字节作为布隆键，避免重复编码。

### `compress/` — 压缩

| 算法   | 适配器                     |
| ------ | -------------------------- |
| None   | 透传（无压缩）             |
| Zstd   | `ZstdCompressionAdapter`   |
| Snappy | `SnappyCompressionAdapter` |
| LZ4    | `Lz4CompressionAdapter`    |

所有适配器实现 `compress -> string` / `uncompress` 接口。通过 `Options` 对每个 Block 单独配置。

### `pattern/` — 基于模式的行编码

利用数据规律的实验性编码模式：

| 文件         | 用途                                                     |
| ------------ | -------------------------------------------------------- |
| `raw.h`      | 原始编码 — 将值按定长单元格（1/2/4/8/16 字节）原样存储。 |
| `compound.h` | 复合编码 — 基于字典的 Run-Length 和 Delta 变体。         |
| `pattern.h`  | `Pattern` 类型 — 编码策略的标签联合体。                  |

### `format/` — 文件级元数据

| 文件                          | 用途                                                                                                            |
| ----------------------------- | --------------------------------------------------------------------------------------------------------------- |
| `metadata.h` / `metadata.cpp` | `Configuration`（Block 大小、压缩、Bloom 配置）与 `Statistics`（行数、字节数、时间戳）。以 section 形式序列化。 |
| `section.h` / `section.cpp`   | `Section` — 前缀长度、带校验和的数据段。每个文件段（Bloom、元数据、根索引）均封装为 section。                   |
| `tail.h` / `tail.cpp`         | `Tail` — 文件尾部，含魔数、各 section 偏移量及 schema 引用。                                                    |

**文件磁盘布局：**

```
Key File (*.sstv2):                   Value File (*.sstv2):
+------------------+                  +------------------+
|   Data Block 0   |                  | Embedded Value 0 |
|   Data Block 1   |                  | Embedded Value 1 |
|   ...            |                  | ...              |
|   Index Blocks   |                  +------------------+
|   (inline)       |
+------------------+
|  Bloom Section   |
+------------------+
| Metadata Section |
+------------------+
|  Tail (footer)   |
+------------------+
```

### `file/` — SSTable 读写器

顶层 I/O 模块，协调所有子系统：

| 类            | 用途                                                                    |
| ------------- | ----------------------------------------------------------------------- |
| `Builder`     | 接受 `Row`，分区为数据块，构建索引树与布隆过滤器，写入 key+value 文件。 |
| `Reader`      | 打开 key+value 文件，校验各 section，提供 `scan()` 和 `get()` 操作。    |
| `ScanOptions` | 范围扫描配置：`start`（下界前缀）、`limit`（上界前缀）。                |

## 用法示例

### 构建

```bash
bazel build //cpp/pl/sstv2/...
bazel test //cpp/pl/sstv2/...
```

### 写入 SSTable

```cpp
#include "cpp/pl/sstv2/file/sstable.h"
#include "cpp/pl/sstv2/types/schema.h"

using namespace pl::sstv2;

// 1. define schema
auto schema_result = types::SchemaBuilder()
    .add_column("tenant", types::DataType::kString)
    .add_column("user_id", types::DataType::kUint64)
    .add_sort_key("tenant", types::SortOrder::kAscending)
    .add_sort_key("user_id", types::SortOrder::kAscending)
    .build();

auto schema = std::make_shared<const types::Schema>(std::move(*schema_result));

// 2. build SSTable
file::Builder builder(schema);

builder.add(types::Row::make(schema, {
    {"tenant", types::Value::make<types::DataType::kString>("acme")},
    {"user_id", types::Value::make<types::DataType::kUint64>(42)},
}));

auto files = builder.finish();
// -> Files{.key_file = "00001.sstv2", .value_file = "value.sstv2"}
```

### 读取 SSTable

```cpp
// 1. open
auto reader = *file::Reader::open("00001.sstv2", "value.sstv2");

// 2. full scan
auto rows = reader.scan();

// 3. range scan with prefix
auto rows = reader.scan(file::ScanOptions{
    .start = file::KeyPrefix{
        .key_columns = {types::Value::make<types::DataType::kString>("acme")}
    }
});

// 4. point lookup
auto row = reader.get(
    {types::Value::make<types::DataType::kString>("acme"),
     types::Value::make<types::DataType::kUint64>(42)},
    types::Version{.major = 10}
);
```

### 使用 KeyComparator

```cpp
#include "cpp/pl/sstv2/types/key_comparator.h"
#include "cpp/pl/sstv2/types/key_factory.h"

auto internal_schema = types::InternalSchema::make(schema);
types::KeyComparator cmp(internal_schema);

// compare two all-keys
auto all_key_a = types::make_all_key(row_a, internal_schema);
auto all_key_b = types::make_all_key(row_b, internal_schema);
auto result = cmp.compare_all_key(*all_key_a, *all_key_b);
if (result.ok() && *result < 0) {
    // all_key_a < all_key_b
}

// check if all-key falls within a prefix range
auto prefix = types::make_prefix_key(
    types::KeyPrefix{.key_columns = {types::Value::make<types::DataType::kString>("acme")}},
    schema, internal_schema);
auto less = cmp.all_key_less_than_prefix(*all_key_a, *prefix);
```

### 直接使用 Codec

```cpp
#include "cpp/pl/sstv2/codec/scalar_comparable.h"
#include "cpp/pl/sstv2/codec/value_comparable.h"

// encode a Value to memcomparable bytes
types::Value val = types::Value::make<types::DataType::kInt64>(-42);
std::string encoded;
codec::encode_value(val, types::SortOrder::kAscending, &encoded);

// encode an InternalRow to memcomparable AllKey
std::string all_key_bytes;
codec::encode_all_key(row, internal_schema, &all_key_bytes);

// fixed-width integer helpers
uint32_t v = codec::read_fixed32(data, offset);
codec::write_fixed64(&buf, some_uint64);

// variable-length integer
std::string varint_buf;
codec::encode_varint(123456, &varint_buf);
auto [value, bytes_read] = codec::decode_varint(varint_buf);
```

## 关键设计决策

### 为什么选择 memcomparable 编码？

传统 SSTable 在迭代时通过解析 Block 中的 key 并调用 comparator 来比较键。SSTable v2 则将键预先编码为 memcomparable 字节表示。这意味着：

- **布隆过滤器**可以直接哈希编码后的字节（无需重复编码）
- **索引二分查找**使用原始 `memcmp` 替代类型感知比较
- **编码后的键**既是存储格式又是比较格式

代价是写入时的编码开销，这在写入路径上被摊销，在读路径上得到回报。

### 为什么需要类型化键系统？

基于 C++20 concept 的模板化键系统在编译期防止类别错误。你不会意外地将 `RowKey` 与 `AllKey` 做比较 — 编译器直接拒绝。三种表达形式（持有所有权、视图、编码）让调用者能够按各自场景选择最合适的内存模型。

### 为什么需要独立的 InternalSchema？

`InternalSchema` 将用户列名映射到固定位置的列索引，并追加系统列（version、op_type、文件位置、校验和）。
这使得存储层与用户侧列模型解耦，并在热路径中实现定长访问模式。

## 依赖

- **Abseil**（`absl::Status`、`absl::StatusOr`、`absl::StrCat`）
- **zstd** / **snappy** / **lz4**（压缩）
- **xxHash**（校验和）
- **GoogleTest**（测试）

## 文件扩展名

| 扩展名        | 内容                                           |
| ------------- | ---------------------------------------------- |
| `*.sstv2`     | Key 文件 — 数据块、索引块、Bloom、元数据、tail |
| `value.sstv2` | Value 文件 — 嵌入值（大对象行外存储）          |

## 测试

```bash
# all sstv2 tests
bazel test //cpp/pl/sstv2/...

# specific module
bazel test //cpp/pl/sstv2/ut/types:key_test
bazel test //cpp/pl/sstv2/ut/file:sstable_test --test_output=all
```

## 相关项目

- `cpp/pl/sst/` — SSTable v1（参考实现，更简单的 Block 格式）
- `cpp/pl/minidfs/` — 使用 SSTable 存储元数据的分布式文件系统
