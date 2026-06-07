# SSTableV2 实现 Review 报告

**Review 日期**: 2026-06-07
**对照基线**: `SSTableV2格式规范_v0.95_20180914.pdf` (67 页, 百度机密)
**实现版本**: Phase 1 (最小可用)
**Review 范围**: `cpp/pl/sstv2/` 全部 30+ 源文件和设计文档

---

## 1. 总体评估

| 维度 | 评分 | 说明 |
|------|------|------|
| 规范符合度 | ⭐⭐⭐⭐ 85% | 核心格式 (Tail/Block/Bloom/Metadata) 与 PDF 高度一致 |
| 代码质量 | ⭐⭐⭐⭐ | 现代 C++17/20, constexpr 密集, header-only 优先 |
| 架构设计 | ⭐⭐⭐⭐⭐ | KV 分离、InternalRow/Row 分离、Schema/InternalSchema 分离 |
| 完备性 | ⭐⭐⭐ 60% | Phase 1 覆盖了最小可用路径, 大量 Phase 2/3 特性待开发 |
| 正确性风险 | ⭐⭐⭐ | 存在 2 个规范偏离和若干边界条件隐患 |

---

## 2. 完全符合规范的部分 ✅

以下 15 项与 PDF 规范 v0.95 逐字节一致, 无需修改:

### 2.1 Tail (尾信息, 32 字节)

| 偏移 | 大小 | 字段 | 状态 |
|------|------|------|------|
| 0 | 8 | checksum (CRC32C, zero-extended) | ✅ `format/tail.cpp:37-46` |
| 8 | 8 | locator_offset | ✅ |
| 16 | 8 | locator_length | ✅ |
| 24 | 4 | version = 2 | ✅ `format/tail.h:32` |
| 28 | 4 | magic = 0x00545353 ("SST\0") | ✅ `format/tail.h:31` |

Checksum 计算流程正确: 先置零 → 编码 → 计算 CRC32C → 回填。

### 2.2 Block Header (52 字节)

```cpp
// block/block.cpp:50-58 — 完全匹配 PDF §4.1.1
Header {
    Kind magic;                    // 4 bytes: DTBK/IXBK/ROOT
    uint64_t flags;                // 8 bytes
    uint64_t row_count;            // 8 bytes
    uint64_t offset_table_offset;  // 8 bytes
    uint64_t uncompressed_block_length; // 8 bytes
    uint64_t compressed_block_length;   // 8 bytes
    uint64_t checksum;             // 8 bytes
};
static constexpr size_t kSize = 52;
```

### 2.3 Block Magic 值

| Magic | 值 | ASCII | 含义 | 状态 |
|-------|-----|-------|------|------|
| `0x4B425444` | — | DTBK | Data Block | ✅ `block/block.h:33` |
| `0x4B425849` | — | IXBK | Index Block | ✅ `block/block.h:34` |
| `0x544F4F52` | — | ROOT | Root Index | ✅ `block/block.h:35` |

### 2.4 Bloom Filter Header (36 字节)

| 偏移 | 大小 | 字段 | 状态 |
|------|------|------|------|
| 0 | 4 | magic = 0x4D4F4C42 ("BLOM") | ✅ `bloom/bloom.h:32` |
| 4 | 4 | version = 1 | ✅ `bloom/bloom.h:33` |
| 8 | 4 | hash_count | ✅ |
| 12 | 8 | bit_count | ✅ |
| 20 | 8 | row_count | ✅ |
| 28 | 8 | checksum | ✅ |

### 2.5 Metadata Section 通用格式

```cpp
// format/section.cpp:36-39
SectionMagic magic;     // 4 bytes LE
uint64_t checksum;      // 8 bytes LE (CRC32C, zero-extended)
varint entry_count;
map entries (key_len + key + type_tag + value);
```

Magic 值完全匹配:

| Magic | 值 | ASCII | 含义 |
|-------|-----|-------|------|
| `0x41434F4C` | — | LOCA | Locator |
| `0x47494643` | — | CFIG | Configuration |
| `0x414D4553` | — | SEMA | Schema |
| `0x54415453` | — | STAT | Statistics |
| `0x504D4F43` | — | COMP | Compatibility |
| `0x52455355` | — | USER | User-Defined |

### 2.6 DataType 枚举

`types/data_type.h:34-59` 与 PDF 表 1 完全一致:

```
0=None, 1=Bool, 2=Int8, 3=Uint8, 4=Int16, 5=Uint16,
6=Int32, 7=Uint32, 8=Int64, 9=Uint64, 10=Float, 11=Double,
12=LongDouble, 13=Time, 14=Version, 15=String, 16=U16String,
17=U32String, 18=Binary, 19=Array, 20=Map,
21=DataBlock (private), 22=IndexBlock (private)
```

### 2.7 ColumnFlag 位布局

`types/column_flag.h:42-45` 与 PDF Fig 20 一致:

```
Bits 0-7  [DT]  DataType enum (0-255)
Bit  8    [C]   Checksum present
Bit  9    [B]   Bool value (when DT=Bool)
Bits 10-63      保留, 必须为 0
```

索引块中 C=0, B=0 的验证逻辑 ✅ (`column_flag.h:112-114`)

### 2.8 Internal Table (M+7 列)

`types/internal_schema.h:160-168` 与 PDF §2.5 一致:

```
Index M     Version (kVersion, kDescending)
Index M+1   OpType  (kUint8, kAscending)
Index M+2   Flag    (kUint64)
Index M+3   Filename (kString)
Index M+4   Offset  (kUint64)
Index M+5   Length  (kUint64)
Index M+6   Checksum (kUint64)
```

### 2.9 Value File 编码

`file/value_codec.cpp:91-168` 完全匹配 PDF §9 和 §2.3.2:

| 类型 | 编码方式 | 状态 |
|------|---------|------|
| Bool | 1 byte (0/1) | ✅ |
| Int8/Uint8 | 1 byte direct | ✅ |
| Uint16/32/64 | varints | ✅ |
| Int16/32/64 | zigzag + varints | ✅ |
| Float/Double | 4/8 bytes direct | ✅ |
| Time | Int64(zigzag) + Uint32(varints) | ✅ |
| Version | Uint64(varints) + Uint64(varints) | ✅ |
| String/Binary | raw bytes (no length prefix) | ✅ |
| Array/Map | count(varints) + variant sequence | ✅ |

### 2.10 Index Block 语义

`index/index_tree.cpp:84-100` 完全匹配 PDF §4.5:

- Index Block 中 Flag 使用私有类型 21(DataBlock)/22(IndexBlock) ✅
- Checksum 列在 Index Block 中存储子树总行数 ✅
- Filename 使用 "@2" (当前 key file) ✅
- RowKey 存储子节点最后一行的 all key ✅

### 2.11 Locator Key 名称

`file/sstable.cpp:53-62` 完全匹配 PDF §7:

```
RootIndex_Offset, RootIndex_Length
Configuration_Offset, Configuration_Length
Schema_Offset, Schema_Length
Statistics_Offset, Statistics_Length
BloomFilter0_Offset, BloomFilter0_Length
```

### 2.12 其他一致性

| 项目 | 状态 | 位置 |
|------|------|------|
| Little-endian 所有多字节字段 | ✅ | `codec/endian.h` |
| CRC32C checksum (zero-extended to uint64) | ✅ | `codec/checksum.h` |
| All-key 排序 (Memcomparable + Version desc + OpType asc) | ✅ | `codec/value_comparable.h:206-216` |
| Varint/LEB128 编码 | ✅ | `codec/varint.h` |
| ZigZag 编码 | ✅ | `codec/varint.h:49-55` |
| Pattern 0: 定长无编码 `[id:1B][row_count:varint][cells]` | ✅ | `pattern/raw.h:122-128` |
| Pattern 100: Compound header `[id:1B][sub_count:varint]` | ✅ | `pattern/compound.h:41-44` |
| Memcomparable: sign-flip+BE, 8-byte escaped groups | ✅ | `codec/comparable.h` |

---

## 3. 🔴 严重问题 (需立即修复)

### 3.1 Block Flags 缺少 PS (Pattern Store) 位

**规范依据**: PDF §4.1.1 (Fig 29), 设计文档 §5.3
> "Bit 0: PS — 模式存储位。1 = 使用 pattern encoding。"

**问题代码**: `compress/compress.h:142-144`
```cpp
[[nodiscard]] constexpr uint64_t encode_block_flag(Codec codec) {
    return static_cast<uint64_t>(codec) << 2;
    // ❌ Bit 0 (PS) 始终为 0, 未设置 Pattern Store 标志
}
```

**影响**: 尽管当前所有 Block 都使用了 Pattern 0 编码, 但 Reader 读取 block header 的 flags 时会看到 PS=0, 认为 block 不使用模式存储。兼容的第三方 Reader 将无法正确解析数据。

**修复方案**:
```cpp
[[nodiscard]] constexpr uint64_t encode_block_flag(Codec codec) {
    // Bit 0: PS (Pattern Store) = 1
    // Bits 2-9: C (Compression codec)
    // Other bits: 0
    return 1ULL | (static_cast<uint64_t>(codec) << 2);
}
```

同时应添加 flags 的完整布局定义 (参照 PDF Fig 29):

```cpp
namespace block_flags {
    static constexpr uint64_t kPatternStore = 1ULL << 0;  // PS
    static constexpr uint64_t kRowKeyBitmap = 1ULL << 1;  // PK
    static constexpr uint8_t  kCompressShift = 2;          // C  (bits 2-9)
    static constexpr uint8_t  kPrefixShift = 10;           // PC (bits 10-13)
    static constexpr uint64_t kCompressMask = 0xFFULL << kCompressShift;
    static constexpr uint64_t kPrefixMask = 0xFULL << kPrefixShift;
}
```

---

### 3.2 Compound Pattern 缺少 Meta 段

**规范依据**: PDF §4.4.6 (Fig 41), 设计文档 §8.7
> "Compound pattern 的 meta 部分记录每个子列的 DataType 和起始偏移"

**问题代码**: `pattern/compound.h:41-44`
```cpp
inline void write_compound_header(size_t sub_count, std::string* dst) {
    dst->push_back(static_cast<char>(static_cast<uint8_t>(PatternId::kCompound)));
    codec::encode_varint(sub_count, dst);
    // ❌ 缺少 Meta 段: 每个子模式应记录 type(1B) + offset(varint)
}
```

**PDF 规范 Fig 41 的布局**:
```
+----------+----------+-------------------+-------------+
| Header   | Meta     | Data              |
+----------+----------+-------------------+-------------+
| id=100   | type[0]  | sub_unit_0 bytes  |
| sub_cnt  | offset[0]| sub_unit_1 bytes  |
|          | type[1]  | ...               |
|          | offset[1]| sub_unit_n bytes   |
|          | ...      |                   |
+----------+----------+-------------------+-------------+
```

**影响**: 当前实现依赖于外部类型系统 (如 CompoundEncoder<DataType::kString> 硬编码为 2 个子列) 来推导子列数量和类型。严格按照规范实现的 Reader 将无法正确解析。

**修复方案**: 在 `finish_into()` 中, 在写入子 unit 数据之前写入 Meta 段:

```cpp
void finish_into(std::string* dst) const {
    detail::write_compound_header(2, dst);

    // --- Meta 段 ---
    // Type of sub-column 0: Uint64
    dst->push_back(static_cast<char>(static_cast<uint8_t>(DataType::kUint64)));
    // Offset of sub-column 0 (relative to end of meta)
    size_t meta_end_marker = dst->size();
    codec::encode_varint(0, dst); // placeholder, will be patched

    // Type of sub-column 1: Uint64
    dst->push_back(static_cast<char>(static_cast<uint8_t>(DataType::kUint64)));
    codec::encode_varint(0, dst); // placeholder

    // Patch offsets
    size_t data_start = dst->size();
    // ... patch offset[0], offset[1] ...

    // --- Data 段 ---
    offsets_.finish_into(dst);
    lengths_.finish_into(dst);
}
```

---

## 4. 🟡 中等问题 (建议近期修复)

### 4.1 BlockBuilder 接口偏差

**设计文档** (`implementation_plan.md:219-239`):
```cpp
// 设计接口:
bool add(const InternalRow& row, std::string_view all_key, std::string_view payload);
// 返回 false 表示 block 已满
```

**实际实现** (`block/block.h:59-60`):
```cpp
absl::Status add(types::InternalRow row);       // 不携带 all_key
absl::Status add(types::InternalRow row, std::string embedded_value); // 不返回满
```

**差异点**:
1. 设计接口接收 `all_key` 和 `payload` 作为外部传入参数; 实际接口不接收 (all_key 由上层管理, payload 被合并为 embedded_value string)
2. 设计接口返回 `bool` 表达 block 容量; 实际接口返回 `absl::Status` 且永不拒绝行
3. 设计接口接受 `string_view` (零拷贝); 实际接口接受 move 语义的 `string`

**影响**: 中等。当前 Phase 1 中 Builder 负责管理行缓冲区, 但接口不表达容量限制, 所有行无条件入队, 缺乏内存保护。

**建议**: 至少实现容量检查逻辑 (见问题 4.5)。

---

### 4.2 BlockBuilder 无内存上限控制

**问题代码**: `block/block.cpp:465-474`
```cpp
absl::Status BlockBuilder::add(types::InternalRow row, std::string embedded_value) {
    // ...
    rows_.push_back(std::move(row));
    embedded_values_.push_back(std::move(embedded_value));
    return absl::OkStatus();
    // ❌ 从不拒绝, 无限累积
}
```

**规范依据**: PDF §6.1 配置信息定义了 `MaxDataBlockSizeInByte_SoftLimit` 和 `MaxDataBlockSizeInByte_HardLimit`。

**修复方案**:
```cpp
absl::Status BlockBuilder::add(types::InternalRow row, std::string embedded_value) {
    // Validate schema
    if (schema_ == nullptr) return ...;
    if (row.columns.size() != schema_->column_count()) return ...;

    // Check size limit
    if (estimated_size_ > max_block_size_) {
        return absl::ResourceExhaustedError("block is full");
    }

    estimated_size_ += estimate_row_size(row, embedded_value);
    rows_.push_back(std::move(row));
    embedded_values_.push_back(std::move(embedded_value));
    return absl::OkStatus();
}
```

---

### 4.3 SSTableBuilder 缺少字节大小的 Flush 触发

**问题代码**: `file/sstable.cpp:288-291`
```cpp
if (pending_rows_.size() >= max_data_block_rows()) {
    return flush_data_block();
}
// ❌ 只按行数触发, 不按字节大小触发
```

**规范依据**: PDF §6.1 定义了软限制和硬限制, 要求 block 大小必须 ≤ 硬限制。

**影响**: 如果行很大 (含大量内嵌数据), Block 可能超过 `MaxDataBlockSizeInByte_HardLimit`, 导致与标准实现不兼容。

**修复方案**: 在 `add()` 中增加字节估算:
```cpp
uint64_t estimated_block_bytes() const {
    // 粗略估算: header(52) + sum(row sizes) + overhead
    uint64_t est = 52;
    for (const auto& row : pending_rows_) {
        // 估算每列编码后的字节数
        est += estimate_row_bytes(row);
    }
    return est;
}

if (estimated_block_bytes() >= options_.configuration.max_data_block_size_soft_limit ||
    pending_rows_.size() >= max_data_block_rows()) {
    return flush_data_block();
}
```

---

### 4.4 Statistics 编码中存在 fragile 的大小耦合

**问题代码**: `file/sstable.cpp:359-399`

```cpp
// Step 1: 编码 statistics (key_file_size = 0)
std::string statistics_section = format::encode_statistics(statistics);
// statistics_section.size() 此时可能较小 (key_file_size=0 的 varint 只需 1 字节)

// Step 2: 计算 key_file_size, 依赖 statistics_section.size()
statistics.key_file_size = files_.key_file.size()
    + statistics_section.size()   // ← 基于旧值
    + locator.size()
    + format::Tail::kSize;

// Step 3: 重编码 statistics (key_file_size 变为实际大值)
statistics_section = format::encode_statistics(statistics);
// 此时 key_file_size 很大, 其 varint 可能跨越字节边界,
// statistics_section.size() 可能改变!
```

**分析**: 如果 `key_file_size` 从 `<128` 变为 `>=128` (单字节 varint → 双字节), 则 `statistics_section.size()` 增加 1 字节, 导致 `key_file_size` 计算少 1 字节。

**缓解因素**: 当前代码在重编码后立刻更新了 locator 中的 Statistics_Length, 并重新编码 locator。所以 reader 能正确找到 Statistics section。但 `key_file_size` 字段本身可能少 1 字节 — 这只是一个统计值, 不影响文件读取。

**修复方案**: 在重新编码 statistics 后重新计算 key_file_size:
```cpp
statistics_section = format::encode_statistics(statistics);
// 重算: 因为 statistics_section 大小可能变了
statistics.key_file_size = files_.key_file.size()
    + statistics_section.size()
    + locator.size()
    + format::Tail::kSize;
statistics_section = format::encode_statistics(statistics); // 再次编码
```

或者使用固定宽度的编码 (如 always 8-byte LE for sizes in STAT) 来消除 varint 变量长度问题。

---

### 4.5 Bool 类型存在冗余存储

**规范依据**: PDF §2.5 (Fig 20 说明)
> "如果数据是 Bool 类型, 布尔值直接存储在 B 位... 该 Bool 变量也不用写入 value 文件"

**问题代码**: `file/sstable.cpp:261-272`
```cpp
const bool has_value = row.value.type() != DataType::kNone;
const bool embedded = has_value && encoded_value->size() <= ...;
// Bool 类型: has_value=true, encoded_value->size()=1
// → 值被写入 value file / data table

if (embedded) {
    embedded_value = std::move(*encoded_value); // 1 字节: 0 或 1
} else {
    files_.value_file.append(*encoded_value);   // 写入 value 文件
}
```

同时 Flag 的 B 位也在 `make_internal_row()` 中被正确设置了 (`file/sstable.cpp:106-107`):
```cpp
internal.columns[schema.flag_index()] = Value::make<DataType::kUint64>(
    ColumnFlag::for_value(row.value.type(), has_value, bool_value).raw());
```

**分析**: Bool 值同时在 B 位 (1 bit) 和 encoded_value (1 byte) 中存储。PDF 规范的设计意图是让 B 位**替代** encoded_value, 而非双重存储。

**修复方案**:
```cpp
// 对于 Bool 类型, 值已在 B 位, 不需要单独写入
const bool is_bool = row.value.type() == DataType::kBool;
const bool has_value = row.value.type() != DataType::kNone && !is_bool;
const uint64_t length = is_bool ? 0 : encoded_value->size();
```

---

## 5. 🔵 轻微问题 / 已知差距 (Phase 2/3 规划中)

### 5.1 Schema 元数据字段不完整

**PDF §6.2 (Fig 50)** 要求 Schema Section 包含以下字段:

| 字段 | 类型 | 说明 | 当前状态 |
|------|------|------|---------|
| `ColumnCount` | Uint64 | 总列数 | ✅ `RowKeyColumnCount` |
| `ColumnN_Type` | Uint8 | 第 N 列数据类型 | ✅ |
| `ColumnN_Order` | Bool/Uint64 | 第 N 列排序方向 | ✅ |
| `ColumnN_Name` | String | 第 N 列名字 | ✅ |
| `ChecksumKey` | Binary(Uint64[]) | 参与校验和的 key 列 | ❌ 缺失 |
| `SplitKey` | Uint64 | 分片键列下标 | ❌ 缺失 |
| `VersionKey` | Uint64 | 版本列下标 | ❌ 缺失 |
| `SystemKey` | Uint64 | 系统 key 列数 | ❌ 缺失 |
| `NonKey` | Uint64 | key/value 分界下标 | ❌ 缺失 |

**建议**: 在 `format/metadata.h` 的 `schema_entries()` 和 `schema_from_entries()` 中添加这些字段的编解码。

---

### 5.2 缺少 Index Block 独立配置项

**PDF §6.1**:
- `MaxIndexBlockSizeInByte_SoftLimit` (Uint64)
- `MaxIndexBlockSizeInByte_HardLimit` (Uint64)
- `MaxIndexBlockRowCount` (Uint64)

**当前实现**: `format/metadata.h:31-36` 的 `Configuration` 结构体只定义了数据块的配置:
```cpp
struct Configuration {
    uint64_t max_embedded_value_size = 0;
    uint64_t max_data_block_size_soft_limit = 64 * 1024;
    uint64_t max_data_block_size_hard_limit = 128 * 1024;
    uint64_t max_data_block_row_count = 4096;
    // ❌ 缺少索引块配置
};
```

---

### 5.3 模块划分与设计文档的差异

| 设计文档规划 | 实际实现 |
|-------------|---------|
| `metadata/` 独立模块 (`section.h`, `configuration.h`, `schema_section.h`, `statistics.h`) | 合并到 `format/metadata.h/.cpp` |
| `file/tail.h`, `file/locator.h` 独立文件 | 分别对应 `format/tail.h` 和 `format/section.h` |
| `block/header.h`, `block/data_table.h`, `block/offset_table.h` 独立 | 统一在 `block/block.h/.cpp` |

**影响**: 组织层面的差异, 不影响功能。合并后的 `format/` 模块更加内聚, 反而是一种优化。

---

### 5.4 Phase 2/3 待开发特性 (已知差距)

| 特性 | PDF 章节 | 规划 Phase | 优先级 |
|------|---------|-----------|--------|
| Pattern 1: Stream VByte | §4.4.2 | 2 | 高 — 减少整数列存储 |
| Pattern 2: PFOR | §4.4.3 | 3 | 中 |
| Pattern 3: Dictionary | §4.4.4 | 3 | 中 |
| Pattern 4/5: Constant Stride | §4.4.5 | 2 | 高 — 对时序数据有效 |
| 多轮前缀压缩 (PC bits) | §4.2 | 2 | 高 — 对字符串数据有效 |
| RowKey Bitmap (PK bit) | §4.1.4 | 2 | 中 |
| 块级压缩 (LZ4/Zstd) | §16 | 2 | 高 — 压缩框架已就绪 |
| 多级索引树 (IXBK) | §4.5 | 3 | 中 |
| 多个 Bloom Filter | §5 | 3 | 低 |
| COMP section | §6.4 | — | 低 |
| USER section | §6.5 | — | 低 |
| CFIG 完整字段 (RowCountKey 等) | §6.1 | — | 低 |
| STAT 完整字段 (VersionMin/Max 等) | §6.3 | — | 低 |

---

### 5.5 其他小问题

#### 5.5.1 RawEncoder::add() 的有符号整数的位宽不匹配

`block/block.cpp:84-109` 中的 `add_raw_cell`:
```cpp
template <size_t CellSize, typename T>
void add_raw_cell(pattern::RawEncoder<CellSize>* encoder, T value) {
    uint8_t buf[CellSize];
    if constexpr (CellSize == 1) {
        buf[0] = static_cast<uint8_t>(value); // lossy for int8
    }
    // ...
}
```

当 CellSize=1 且 T=int8_t 时, `static_cast<uint8_t>` 先执行模运算, 然后 reinterpreting 回 int8_t — 对于 memcomparable 编码, 这**可能不是问题** (因为我们存储原始 bits), 但不够透明。建议使用 `std::memcpy` 替代 `static_cast`。

#### 5.5.2 Value::compare_values 的浮点数比较

`types/value.h:753`: `compare_scalar(lhs.as_float(), rhs.as_float())` 对 NaN 的行为是未定义的 (NaN != NaN, 不可比较)。虽然 SSTableV2 中 key 不应包含 NaN, 但这是隐患。

#### 5.5.3 缺少文件级别的 CRC 验证后设

PDF §8 和 §4.1.1 规定 Block checksum 在压缩前计算。当前实现 (`block/block.cpp:529`):
```cpp
h.checksum = codec::crc32c_u64(block); // 在未压缩的 block 上计算 ✅
```
这是正确的。

---

## 6. 架构与设计质量评价

### 6.1 亮点

1. **类型安全**: Value 使用 `std::variant` + 模板 traits, 编译期类型检查; ColumnFlag 全链路 constexpr
2. **关注点分离**: InternalRow (纯 key 侧) / Row (用户接口) / Value (类型安全的 tagged union) 三层隔离
3. **零拷贝设计**: RawDecoder 持有指向输入 buffer 的指针, StringRef 引用 data table
4. **自底向上构建**: codec → types → pattern → block → index → file, 依赖方向单向
5. **CRC32C 一致性**: 所有 checksum 计算遵循 "置零 → 计算 → 回填" 规范流程
6. **Header-only 优先**: 大量代码是 header-only, 仅在必要时分离 .cpp

### 6.2 架构对比: 设计文档 vs 实现

```
设计文档接口 (BlockBuilder):
  输入: InternalRow + all_key + value_payload (string_view)
  输出: Block bytes
  状态: add() 返回 bool (容量检查)

实际实现:
  输入: InternalRow + embedded_value (string)
  输出: Block bytes
  状态: add() 返回 absl::Status (不检查容量)
```

差异原因: Phase 1 中 all_key 管理和容量检查上移到 SSTableBuilder 层, BlockBuilder 更加简单。这是合理的 Phase 1 简化, 但需要在后续阶段加强。

---

## 7. 修复优先级建议

### P0 — 立即修复 (影响兼容性)
| # | 问题 | 文件 | 工作量 |
|---|------|------|--------|
| 3.1 | Block Flags PS 位缺失 | `compress/compress.h` | 1 行 |
| 3.2 | Compound Pattern Meta 段缺失 | `pattern/compound.h` | ~20 行 |

### P1 — 近期修复 (影响正确性)
| # | 问题 | 文件 | 工作量 |
|---|------|------|--------|
| 4.5 | Bool 类型冗余存储 | `file/sstable.cpp` | ~10 行 |
| 4.1/4.2 | BlockBuilder 容量检查 | `block/block.h/.cpp` | ~30 行 |
| 4.3 | SSTableBuilder 字节限制 | `file/sstable.cpp` | ~20 行 |
| 4.4 | Statistics 大小耦合 | `file/sstable.cpp` | ~15 行 |

### P2 — Phase 2 排期
| # | 问题 | 工作量 |
|---|------|--------|
| 5.1 | Schema 完整元数据字段 | ~50 行 |
| 5.2 | Index Block 配置项 | ~30 行 |
| 5.4 | Pattern 1/4/5 + 前缀压缩 + 块压缩 | 大 |

---

## 8. 测试覆盖建议

当前测试 (`cpp/pl/sstv2/ut/`) 覆盖了 codec/ 和 types/ 模块。建议增加:

| 测试 | 验证内容 |
|------|---------|
| `block_test.cpp` | Block 编解码 round-trip, 各种 DataType 组合 |
| `index_test.cpp` | 索引树构建 2+ 层, 后序遍历顺序, 查找精度 |
| `bloom_test.cpp` | 假阳性率, 序列化 round-trip |
| `format_test.cpp` | Tail/Locator/Section 编解码 |
| `sstable_test.cpp` | 端到端 write→read, KV 分离, 大 value |
| `regression_test.cpp` | 与参考实现的二进制兼容性 |

---

## 附录 A: 文档勘误

| 位置 | 问题 | 建议 |
|------|------|------|
| design.md §5.3 Flags 位定义 | PK 写为 "RowKey bitmap: 1 = block 包含重复 rowkey bitmap" | 应改为 "1 = 包含 RowKey Bitmap" (与 PDF 一致) |
| design.md §9.1 Data Table 布局 | "多轮前缀压缩数据" 在上, "内嵌数据" 在下 | ✅ 与 PDF Fig 34 一致 |
| design.md §13.2 Value 编码 | "Version: 两个 UInt64, 均 varints 编码" | ✅ 与 PDF §9 一致 |
| PDF §4.4.2 Stream VByte | 章节标题写 "模式 1: Stream VByte" 但 Fig 38 注释写 "模式 3: PFOR" | PDF Fig 38 标题与 Fig 36 标题可能混淆, 实际上 Fig 38 确实是 PFOR |

---

## 附录 B: 代码行数统计

| 模块 | 头文件 | 实现文件 | 测试文件 | 总计 |
|------|--------|---------|---------|------|
| codec/ | 5 文件 | 2 文件 | 1 文件 | ~800 行 |
| types/ | 6 文件 | 0 文件 | 3 文件 | ~1200 行 |
| pattern/ | 3 文件 | 0 文件 | 0 文件 | ~400 行 |
| compress/ | 1 文件 | 1 文件 | 1 文件 | ~350 行 |
| block/ | 1 文件 | 1 文件 | 0 文件 | ~700 行 |
| bloom/ | 1 文件 | 1 文件 | 0 文件 | ~250 行 |
| index/ | 1 文件 | 1 文件 | 0 文件 | ~400 行 |
| format/ | 3 文件 | 3 文件 | 0 文件 | ~600 行 |
| file/ | 3 文件 | 3 文件 | 1 文件 | ~900 行 |
| **总计** | **24 文件** | **12 文件** | **6 文件** | **~5600 行** |

---

> **结论**: SSTableV2 Phase 1 实现了与 PDF 规范高度一致的核心框架, 15 项格式检查全部通过。发现 2 个需要立即修复的兼容性问题 (PS 位, Compound Meta 段) 及 5 个建议修复的正确性/完整性改进。预计 4-6 小时完成 P0+P1 修复。

---

## 附录 C: Follow-up Review (2026-06-07)

基于第一轮 Review 的修复验证和额外改进。

### 已修复的 P0 问题
| # | 问题 | 修复方式 |
|---|------|---------|
| 3.1 | Block Flags PS 位缺失 | `encode_block_flag()` 设置 bit 0 = 1 |
| 3.2 | Compound Pattern Meta 段缺失 | `write_compound_units<N>()` 写入 type + offset meta |

### 已修复的 P1 问题
| # | 问题 | 修复方式 |
|---|------|---------|
| 4.1/4.2 | BlockBuilder 容量检查 | `add()` 检查行数限制；Builder 层负责 size 检查 |
| 4.3 | SSTableBuilder 字节 Flush | `encoded_data_block_size_with()` 精确测量后触发 flush |
| 4.4 | Statistics 编码大小耦合 | 收敛循环 (max 16 iterations) |
| 4.5 | Bool 类型冗余存储 | B 位替代 encoded_value，`materialize_row()` 从 B 位还原 |

### 新增修复
| # | 问题 | 修复方式 |
|---|------|---------|
| — | ChecksumKey 语义错误 | 改为空 Binary（仅 value 参与校验和，PDF §2.5 允许） |
| — | 单行硬限制后门 | 按 PDF §6.1 允许单行超限写入，移除 `BlockBuilder::finish()` 硬限制检查 |
| — | Schema 引用 → shared_ptr | `Schema&` / `InternalSchema&` 参数全部改为 `ConstPtr` 传递 |

### 关于 bytes_consumed_ 调整的更正

初版 Review 标记 `bytes_consumed_` 调整为"冗余"——此判断**有误**。
`parse_compound_units` 将最后一个 sub-unit 的切片延伸到输入末尾 (包含 trailing bytes)，
但 RawDecoder 只解析实际编码数据。`bytes_consumed_` 调整将 trailing bytes 从计数中排除，
对 `TrailingBytesIgnored` 测试用例至关重要。**已恢复该调整**。

### 最终测试结果
```
12/12 tests pass ✅
```
