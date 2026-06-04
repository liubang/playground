# SSTableV2 模块集成详细设计

## 1. 背景与目标

当前 SSTableV2 各模块（Schema、BlockWriter、ColumnStore、Pattern、MultiPrefixCompressor、IndexTree、Bloom、ValueFile、Metadata）
均已独立实现并通过单元测试，但顶层的 `SSTableBuilder` 和 `SSTableReader` 仅是占位实现，未将各模块串联起来。

本次改造目标：**实现强 Schema、Block 级列存的完整写入/读取管线**。

---

## 2. 文件格式（最终）

**核心设计：Data Blocks 与 Index Blocks 交错排列。**

IndexTreeBuilder 是增量式的：每当 leaf-level index block 攒满（超过 `index_block_size`），
就立即 flush 到文件流中，其 offset 就是当时的写入位置。这导致 index blocks
自然地穿插在 data blocks 之间。高层 index block 在更低层 flush 时才生成，最终 root
index block 在 `finish()` 时作为最后一个 index block 写出。

```
+---------------------------------------------------+
|  Data Block 0                                     |
|  Data Block 1                                     |
|  ...                                              |
|  Data Block K-1                                   |
|  Index Block (leaf, points to DB 0..K-1)          |  <- leaf full, flush immediately
|  Data Block K                                     |
|  Data Block K+1                                   |
|  ...                                              |
|  Index Block (leaf, points to DB K..M-1)          |  <- leaf full again
|  Index Block (level-1, points to prev 2 leaves)   |  <- level-1 full
|  Data Block M                                     |
|  ...                                              |
|  (Data Blocks and Index Blocks interleaved)       |
|  ...                                              |
|  Index Block (root)                               |  <- written at finish()
+---------------------------------------------------+
|  Bloom Filter                                     |
+---------------------------------------------------+
|  Metadata: Configuration                          |
|  Metadata: Schema                                 |
|  Metadata: Statistics                             |
+---------------------------------------------------+
|  Locator (section_type -> offset/size directory)  |
+---------------------------------------------------+
|  Tail (32 bytes, fixed at file end)               |
+---------------------------------------------------+
```

**Locator 中记录的 section 含义：**

- `kSectionDataBlocks`: 整个 "data + index 交错区域" 的起止 (offset=0, size=交错区域总大小)
- `kSectionIndexBlocks`: root index block 的 offset + size（快速定位 root）
- `kSectionBloomFilter`: bloom filter 的 offset/size
- 各 Metadata sections: 各自的 offset/size

Value File（可选，KV 分离时使用）是独立的 `.vlog` 文件，不在主文件内。

---

## 3. I/O 抽象层设计

### 3.1 设计原则

文件读写抽象为流式接口，分离具体实现（内存 buffer、本地文件、mmap、远程文件系统等）。
核心要求：顺序写高效（零拷贝 append）、随机读高效（按 offset/size 读取 span）。

### 3.2 WritableFile 接口

```cpp
namespace pl::sstv2::io {

// 顺序写入抽象。所有写入操作都是 append-only。
class WritableFile {
public:
    virtual ~WritableFile() = default;

    // Append data, returns the starting offset where data was written
    virtual absl::StatusOr<uint64_t> append(std::string_view data) = 0;

    // Current total bytes written (= offset of next append)
    virtual uint64_t size() const = 0;

    // Flush to storage + close
    virtual absl::Status finish() = 0;

    // Abort writing (optional: delete partial output)
    virtual void abort() = 0;
};

} // namespace pl::sstv2::io
```

### 3.3 ReadableFile 接口

```cpp
namespace pl::sstv2::io {

// 随机读取抽象。
class ReadableFile {
public:
    virtual ~ReadableFile() = default;

    // Read [offset, offset+size) range
    virtual absl::StatusOr<std::string> read(uint64_t offset, size_t size) const = 0;

    // Zero-copy view (mmap can return direct mapping, memory impl can too)
    // Returns nullopt if zero-copy not supported; caller falls back to read()
    virtual std::optional<std::string_view> view(uint64_t offset, size_t size) const {
        return std::nullopt;
    }

    // Total file size
    virtual uint64_t size() const = 0;
};

} // namespace pl::sstv2::io
```

### 3.4 FileSystem 接口

```cpp
namespace pl::sstv2::io {

// 文件系统抽象。可以是本地文件系统，也可以是 HDFS/S3 等。
class FileSystem {
public:
    virtual ~FileSystem() = default;

    // Create a writable file (sequential append)
    virtual absl::StatusOr<std::unique_ptr<WritableFile>>
    create_writable(std::string_view path) = 0;

    // Open a readable file (random access)
    virtual absl::StatusOr<std::unique_ptr<ReadableFile>>
    open_readable(std::string_view path) = 0;

    // Delete file
    virtual absl::Status remove(std::string_view path) = 0;

    // Check existence
    virtual absl::StatusOr<bool> exists(std::string_view path) = 0;

    // Get file size
    virtual absl::StatusOr<uint64_t> file_size(std::string_view path) = 0;
};

} // namespace pl::sstv2::io
```

### 3.5 内置实现

V1 提供两种实现：

**1. InMemoryWritableFile / InMemoryReadableFile（用于测试 + 小文件场景）**

内部持有 `std::string` buffer。`append()` 就是 `string::append`，零 syscall。
`view()` 返回 string_view 直接指向内部 buffer。适合单元测试和小规模数据。

**2. PosixWritableFile / PosixReadableFile + PosixFileSystem（生产默认）**

- `PosixWritableFile`: 内部维护对齐 buffer（如 64KB），批量 write 减少 syscall，finish() 时 fsync
- `PosixReadableFile`: 小读取使用 pread，view() 返回 nullopt（不支持零拷贝）
- 后续可扩展 `MmapReadableFile`，view() 返回 mmap 映射区域

**性能考量：**

- WritableFile::append 接受 string_view，不强制 copy（PosixWritableFile 内部 buffer 会 copy，但避免多余分配）
- InMemoryWritableFile 直接 append 到连续内存，测试场景下跑得跟内存操作一样快
- ReadableFile::view() 允许 mmap 场景下直接返回内存映射指针，避免拷贝
- FileSystem 本身无状态，仅负责 open/create/delete，不持有文件句柄

### 3.6 使用方式

```cpp
// SSTableBuilder 构造时接收 FileSystem 引用
SSTableBuilder(const types::InternalSchema& schema,
               std::string_view output_path,
               io::FileSystem& fs,
               SSTableBuilderOptions opts = {});

// SSTableReader 同理
static absl::StatusOr<SSTableReader> open(std::string_view path, io::FileSystem& fs);

// 默认提供全局 PosixFileSystem 单例（便利函数）
FileSystem& default_filesystem();
```

---

## 4. Schema、RowKey 与 InternalRow 重新设计

### 4.1 核心变更：复合主键

当前设计假设 row_key 为单列 string。但在强 schema 设计下，主键（row_key）可以由**多个 typed 列**构成，
例如 `PRIMARY KEY (tenant_id: Uint64, user_id: String, timestamp: Int64)`。

这意味着：
- Schema 需要明确标记哪些列构成主键，以及主键列的排序方向
- row_key 不再是一个 `std::string_view`，而是结构化的多列值组合
- seek/比较 需要按列逐列进行，且支持前缀匹配（只提供前 K 列）
- 底层存储仍然将 row_key 编码为有序字节序列（memcomparable format），但上层提供结构化访问

### 4.2 ExternalSchema 重新设计

```cpp
namespace pl::sstv2::types {

// Sort direction for key columns
enum class SortOrder : uint8_t {
    kAscending = 0,
    kDescending = 1,
};

struct ColumnDef {
    std::string name;
    DataType type;
    bool nullable = false;
    std::optional<DataType> element_type;  // Array element type
    std::optional<DataType> key_type;      // Map key type
    std::optional<DataType> value_type;    // Map value type
};

struct KeyColumnDef {
    size_t column_index;     // index into ExternalSchema::columns_
    SortOrder order = SortOrder::kAscending;
};

class ExternalSchema {
public:
    // columns: all columns (key + value columns mixed)
    // key_columns: ordered list of columns forming the primary key
    ExternalSchema(std::vector<ColumnDef> columns, std::vector<KeyColumnDef> key_columns);

    [[nodiscard]] size_t num_columns() const;
    [[nodiscard]] const ColumnDef& column(size_t idx) const;
    [[nodiscard]] std::optional<size_t> find_column(std::string_view name) const;

    // Primary key definition
    [[nodiscard]] size_t num_key_columns() const;
    [[nodiscard]] const KeyColumnDef& key_column(size_t key_idx) const;
    [[nodiscard]] const std::vector<KeyColumnDef>& key_columns() const;

    // Value columns = all columns not in key_columns
    [[nodiscard]] std::vector<size_t> value_column_indices() const;

private:
    std::vector<ColumnDef> columns_;
    std::vector<KeyColumnDef> key_columns_;
};

} // namespace pl::sstv2::types
```

### 4.3 RowKey 类型

`RowKey` **持有 schema 引用**，因此它知道自己是完整 key 还是前缀 key，也能自行完成编码和比较。

设计理由：
- RowKey 必须能回答 `is_prefix()`——这需要知道 schema 定义了几列主键
- 编码（memcomparable）和比较都依赖 sort order 信息——由 schema 提供
- 持有引用避免每次操作都手动传入 schema，接口更自然
- schema 的生命周期由 SSTableBuilder/Reader 保证，远长于 RowKey

```cpp
namespace pl::sstv2::types {

// A structured row key consisting of 1..N typed column values.
// Holds a reference to InternalSchema for self-aware encoding/comparison/validation.
// The schema must outlive the RowKey.
class RowKey {
public:
    // --- Construction (with schema validation) ---

    // Construct a full key or prefix key.
    // Validates:
    //   1. columns.size() > 0 && columns.size() <= schema.num_key_columns()
    //   2. Each column's type matches the corresponding key column type in schema
    //   3. Key columns do not allow null (or nullable handling per schema)
    // Returns error if validation fails.
    static absl::StatusOr<RowKey> create(const InternalSchema& schema,
                                         std::vector<Variant> columns);

    // Unchecked construction (for internal use where validation is already done,
    // e.g., decoding from known-good encoded bytes).
    // Caller guarantees type safety.
    static RowKey create_unchecked(const InternalSchema& schema,
                                   std::vector<Variant> columns);

    // --- Accessors ---

    [[nodiscard]] size_t num_columns() const;
    [[nodiscard]] const Variant& column(size_t idx) const;
    [[nodiscard]] const Variant& operator[](size_t idx) const;
    [[nodiscard]] bool empty() const;

    // Is this a prefix key (fewer columns than schema defines)?
    [[nodiscard]] bool is_prefix() const;

    // Is this a full key (all key columns present)?
    [[nodiscard]] bool is_full() const;

    [[nodiscard]] const InternalSchema& schema() const;

    // --- Comparison (respects sort orders from schema) ---

    // Compare with another RowKey.
    // Both must reference the same schema (or compatible key definitions).
    // Prefix semantics: when all overlapping columns are equal, prefix < full.
    [[nodiscard]] std::strong_ordering compare(const RowKey& other) const;

    bool operator==(const RowKey& other) const;
    bool operator<(const RowKey& other) const;
    bool operator<=(const RowKey& other) const;
    bool operator>(const RowKey& other) const;
    bool operator>=(const RowKey& other) const;

    // Check if this key is a prefix of `other` (all our columns match other's)
    [[nodiscard]] bool is_prefix_of(const RowKey& other) const;

    // --- Encoding / Decoding (memcomparable format) ---

    // Encode to memcomparable bytes. Prefix key produces shorter bytes
    // that sort before any full key with the same prefix.
    void encode_to(std::string& out) const;
    [[nodiscard]] std::string encode() const;

    // Decode from memcomparable bytes back to structured RowKey.
    // The decoded RowKey is full if encoded contains all key columns,
    // or prefix if encoded is shorter (partial decode).
    static absl::StatusOr<RowKey> decode(const InternalSchema& schema,
                                         std::string_view encoded);

private:
    RowKey(const InternalSchema& schema, std::vector<Variant> columns);

    const InternalSchema* schema_;
    std::vector<Variant> columns_;
};

} // namespace pl::sstv2::types
```

**构造时的强校验（`create()`）：**

```
RowKey::create(schema, columns):
    1. columns.size() == 0 -> error: "RowKey must have at least one column"
    2. columns.size() > schema.num_key_columns() -> error: "too many key columns"
    3. for i in 0..columns.size():
       expected_type = schema.key_column_types()[i]
       actual_type = columns[i].type()
       if actual_type != expected_type:
           error: "key column {i} type mismatch: expected {expected}, got {actual}"
       if columns[i].is_none() && !schema allows null for key col i:
           error: "key column {i} does not allow null"
    4. All checks pass -> RowKey(schema, std::move(columns))
```

这样在用户代码中：
```cpp
// 编译期无法保证类型安全，但运行时 create() 严格校验
auto key = RowKey::create(schema, {Variant::uint64(123), Variant::string("user_abc")});
if (!key.ok()) { /* type mismatch or column count error */ }

// 前缀查询：只提供第一列
auto prefix = RowKey::create(schema, {Variant::uint64(123)});
assert(prefix->is_prefix());

// 内部解码路径（已知类型安全）用 create_unchecked 避免重复校验
auto decoded = RowKey::create_unchecked(schema, decoded_columns);
```

**比较语义：**

- 两个完整 RowKey：按列逐一比较（Variant::operator<=>），遇到不等即返回，DESC 列取反
- RowKey vs 前缀 RowKey：只比较 min(lhs.num_columns(), rhs.num_columns()) 列。全部相等时，列数少的 < 列数多的（lower_bound 语义）
- SortOrder::kDescending 时，该列比较结果取反
- 两个 RowKey 必须引用相同（或兼容）的 schema，否则行为未定义

### 4.4 RowKey 的存储格式（Memcomparable Encoding）

为了让底层 BlockWriter/Index 可以使用简单的字节比较，RowKey 在写入前会编码为 memcomparable bytes：

```
Encoding rules per column:
  - Uint8/16/32/64:  big-endian bytes (natural memcmp order)
  - Int8/16/32/64:   flip sign bit + big-endian (so -1 < 0 < 1 in memcmp)
  - Float/Double:    IEEE 754 ordered encoding (flip all bits if negative, else flip sign bit)
  - String/Binary:   escaped encoding (0x00 -> 0x00 0xFF, terminated by 0x00 0x00)
  - Bool:            0x00 (false) / 0x01 (true)
  - Time/Version:    same as Int64/Uint64

  For SortOrder::kDescending: bitwise NOT of the encoded bytes for that column.
```

**重要：** 底层 BlockWriter 存储的仍然是编码后的 bytes（`std::string_view`）。
结构化的 RowKey 只在 SSTableBuilder（编码）和 SSTableReader/Iterator（解码）层面使用。
这样 BlockWriter/BlockReader/IndexTreeBuilder 都不需要改动——它们只看 bytes。

### 4.5 InternalSchema 适配

InternalSchema 需要记录主键信息，以便在读写时正确编解码：

```cpp
class InternalSchema {
public:
    static InternalSchema from_external(const ExternalSchema& ext);

    // --- 已有接口不变 ---
    [[nodiscard]] size_t num_sub_columns() const;
    [[nodiscard]] Flag flag(size_t sub_col_idx) const;
    [[nodiscard]] std::string_view sub_column_name(size_t sub_col_idx) const;
    [[nodiscard]] std::pair<size_t, size_t> sub_column_range(size_t ext_col_idx) const;

    // --- 新增主键相关 ---
    [[nodiscard]] size_t num_key_columns() const;
    [[nodiscard]] const std::vector<KeyColumnDef>& key_columns() const;
    [[nodiscard]] std::vector<DataType> key_column_types() const;
    [[nodiscard]] std::vector<SortOrder> key_sort_orders() const;

    // Value column indices (external column indices that are NOT part of primary key)
    [[nodiscard]] const std::vector<size_t>& value_column_indices() const;

private:
    // ... existing fields ...
    std::vector<KeyColumnDef> key_columns_;
    std::vector<size_t> value_column_indices_;
};
```

### 4.6 InternalRow 重新设计

InternalRow 现在明确区分 key part 和 value part：

```cpp
class InternalRow {
public:
    explicit InternalRow(const InternalSchema& schema);

    // --- Key access ---
    [[nodiscard]] RowKey row_key() const;
    void set_key_column(size_t key_col_idx, Variant value);

    // --- Value access (by external column index) ---
    void set(size_t ext_col_idx, Variant value);
    void set_null(size_t ext_col_idx);
    [[nodiscard]] const Variant& get(size_t ext_col_idx) const;
    [[nodiscard]] bool is_null(size_t ext_col_idx) const;

    // --- Encoded row_key (computed on demand, cached) ---
    [[nodiscard]] std::string_view encoded_row_key() const;

    // --- Bulk access for BlockWriter ---
    [[nodiscard]] std::span<const uint64_t> fixed_values() const;
    [[nodiscard]] std::span<const std::string_view> var_values() const;

private:
    const InternalSchema& schema_;
    std::vector<Variant> key_values_;     // key columns (ordered by key_columns_)
    std::vector<Variant> values_;         // all external columns by index
    std::vector<bool> null_flags_;        // per external column

    // Cached encoded key (lazily computed)
    mutable std::string encoded_key_cache_;
    mutable bool encoded_key_dirty_ = true;
};
```

### 4.7 对底层存储的影响

**BlockWriter/BlockReader/IndexTreeBuilder 不需要改动。** 它们继续接收编码后的 row_key bytes。

变更集中在上层：
- SSTableBuilder: 调用 `row.encoded_row_key()` 得到 bytes 传给 BlockWriter
- SSTableReader/Iterator: 从 BlockReader 拿到 encoded bytes 后解码为 RowKey
- Iterator::seek(): 接收 `RowKey`，编码后传给底层 block seek

---

## 5. 写入管线设计（SSTableBuilder 重写）

### 5.1 新接口

```cpp
namespace pl::sstv2::file {

struct SSTableBuilderOptions {
    size_t block_size = 64 * 1024;
    compress::CompressionType compression = compress::CompressionType::kNone;
    size_t bloom_bits_per_key = 10;
    size_t value_size_threshold = 0;     // 0 = disable KV separation (V1 reserved)
    std::string value_file_path;         // non-empty enables KV separation (V1 reserved)
    index::IndexTreeOptions index_opts;
};

class SSTableBuilder {
public:
    SSTableBuilder(const types::InternalSchema& schema,
                   std::string_view output_path,
                   io::FileSystem& fs,
                   SSTableBuilderOptions opts = {});

    // Add a row (caller guarantees row_key ordering)
    absl::Status add(const types::InternalRow& row);

    // Complete writing
    absl::Status finish();

    void abort();

    uint64_t total_rows() const;
    uint64_t data_size() const;

private:
    absl::Status flush_block();
    absl::Status write_metadata();
    void decompose_row(const types::InternalRow& row);

    const types::InternalSchema& schema_;
    SSTableBuilderOptions opts_;

    // I/O
    std::unique_ptr<io::WritableFile> file_;

    // Block layer
    std::unique_ptr<block::BlockWriter> block_writer_;

    // Index layer (write_fn bound to file_->append)
    index::IndexTreeBuilder index_builder_;

    // Bloom layer
    std::unique_ptr<bloom::BloomBuilder> bloom_builder_;

    // Schema-derived mapping (precomputed at construction)
    std::vector<size_t> fixed_col_indices_;  // schema sub-col idx for each fixed col
    std::vector<size_t> var_col_indices_;    // schema sub-col idx for each var col
    // Reusable buffers (avoid per-row allocation)
    std::vector<uint64_t> fixed_values_buf_;
    std::vector<std::string_view> var_values_buf_;

    // Statistics
    uint64_t total_rows_ = 0;
    uint64_t total_blocks_ = 0;
    uint64_t raw_key_bytes_ = 0;
    uint64_t raw_value_bytes_ = 0;
    std::string first_key_;
    std::string last_key_;

    bool finished_ = false;
    bool aborted_ = false;
};

} // namespace pl::sstv2::file
```

### 4.2 InternalRow -> BlockWriter 的映射

**决策确认：**
- String 列的 `.len` sub-column **不写入** ColumnStore（由 OffsetTable 恢复）
- 拆解在 SSTableBuilder 中完成，BlockWriter 保持 low-level 接口不变

**映射逻辑：**

在构造时扫描 schema 的 sub_col 1..N（跳过 sub_col 0 即 row_key），按规则分类：
- 跳过所有名称以 `.len` 结尾的 sub-column
- `is_fixed_size(flag.type)` -> 计入 fixed_col_indices_
- `is_variable_size(flag.type)` -> 计入 var_col_indices_

```cpp
void SSTableBuilder::decompose_row(const InternalRow& row) {
    fixed_values_buf_.clear();
    var_values_buf_.clear();

    for (size_t idx : fixed_col_indices_) {
        if (row.is_null(idx)) {
            fixed_values_buf_.push_back(0);  // null -> 0, bitmap records nullity
        } else {
            fixed_values_buf_.push_back(row.get(idx).as_uint());
        }
    }

    for (size_t idx : var_col_indices_) {
        if (row.is_null(idx)) {
            var_values_buf_.push_back({});
        } else {
            var_values_buf_.push_back(row.get(idx).as_string());
        }
    }
}
```

### 4.3 写入流程

**IndexTreeBuilder 通过回调式 flush 实现交错写入：**

```
SSTableBuilder construction:
    file_ = fs.create_writable(output_path)
    // IndexTreeBuilder write_fn bound to file_->append()
    auto write_fn = [this](std::string_view data) -> uint64_t {
        return *file_->append(data);  // returns the offset where data was written
    };
    index_builder_ = IndexTreeBuilder(opts.index_opts, write_fn);

SSTableBuilder::add(row):
    1. row_key ordering check (last_key_ < row.row_key(), or empty for first)
    2. decompose_row(row)
    3. bloom_builder_->add_key(row.row_key())
    4. bool ok = block_writer_->add_row(
           row.row_key(), fixed_values_buf_, var_values_buf_)
       if !ok (block full):
         flush_block()
         block_writer_->add_row(...)  // retry, must succeed on fresh block
    5. total_rows_++, update raw_key_bytes_ / raw_value_bytes_
    6. update first_key_ / last_key_

SSTableBuilder::flush_block():
    1. auto block_data = block_writer_->finish()
    2. uint64_t block_offset = *file_->append(block_data)    // write data block
    3. index_builder_.add_data_block(                         // notify index
           block_writer_->last_row_key(),
           block_offset,
           block_data.size())
       // If leaf index block is full, IndexTreeBuilder internally calls write_fn
       // to flush index block (interleaved after data block automatically)
    4. total_blocks_++
    5. block_writer_->reset()

SSTableBuilder::finish():
    1. if block_writer_ has pending rows -> flush_block()
    2. auto index_result = index_builder_.finish()
       // finish() flushes remaining index blocks via write_fn
       record root_offset = index_result.root_offset
       record root_size = index_result.root_size
    3. uint64_t data_index_end = file_->size()
    4. Write bloom:
       auto bloom_data = bloom_builder_->finish()
       uint64_t bloom_offset = *file_->append(bloom_data)
    5. Write metadata sections:
       write_metadata()  // each section via file_->append
    6. Build Locator:
       locator.add(kSectionDataBlocks, 0, data_index_end)
       locator.add(kSectionIndexBlocks, root_offset, root_size)
       locator.add(kSectionBloomFilter, bloom_offset, bloom_size)
       locator.add(kSectionConfiguration, config_offset, config_size)
       locator.add(kSectionSchema, schema_offset, schema_size)
       locator.add(kSectionStatistics, stats_offset, stats_size)
    7. file_->append(locator.serialize())
    8. Compute file_checksum, locator_checksum
       Build Tail, file_->append(tail_bytes)
    9. file_->finish()  // flush + close
```

### 4.4 Metadata 内容

**Configuration section:**
```
block_size, compression_type, bloom_bits_per_key, value_size_threshold,
index_block_size, format_version
```

**Schema section:**
```
num_external_columns, for each column:
  name(varint_len + bytes), type(uint8), nullable(uint8),
  element_type(uint8, optional), key_type(uint8, optional), value_type(uint8, optional)
```

**Statistics section:**
```
total_rows, total_blocks, raw_key_bytes, raw_value_bytes,
index_height, bloom_num_keys, first_key, last_key,
file_size, creation_timestamp
```

### 4.5 KV 分离（V1 预留，不实现）

Options 中保留 `value_size_threshold` 和 `value_file_path` 字段。
SSTableBuilder 在 V1 中忽略这两个字段（或当 threshold > 0 时返回 Unimplemented）。
预留的扩展点：

```cpp
// V2 extension: in add() check:
// if (opts_.value_size_threshold > 0 && var_total_size > opts_.value_size_threshold) {
//     ValueHandle handle = value_file_writer_->append(large_value);
//     replace var_values with serialized handle
// }
```

Locator 中预留 `kSectionValueFile = 0x20` section type。

---

## 6. Iterator 抽象与读取管线设计

### 6.1 Iterator 抽象接口

所有读取遍历（block 内、SSTable 级、后续可能的 merge iterator 等）统一实现此接口：

```cpp
namespace pl::sstv2 {

// Unified iterator interface for all levels of the storage stack.
// Supports forward iteration, seek (lower_bound), and prefix scan.
class Iterator {
public:
    virtual ~Iterator() = default;

    // --- Positioning ---

    // Seek to the first row whose row_key >= target.
    // target can be a full RowKey or a prefix RowKey (first K columns only).
    // Prefix seek: positions to the first row whose key columns match the prefix
    // in the first K columns, with remaining columns unconstrained.
    virtual void seek(const types::RowKey& target) = 0;

    // Seek to the last row whose row_key <= target, then position on it.
    // Useful for reverse iteration starting point or upper_bound logic.
    virtual void seek_for_prev(const types::RowKey& target) = 0;

    // Seek to the very first row.
    virtual void seek_to_first() = 0;

    // Seek to the very last row.
    virtual void seek_to_last() = 0;

    // --- Traversal ---

    // Move to the next row. Requires valid() == true before call.
    virtual void next() = 0;

    // Move to the previous row. Requires valid() == true before call.
    virtual void prev() = 0;

    // --- State ---

    // Returns true if the iterator is positioned on a valid row.
    virtual bool valid() const = 0;

    // Returns the current structured row_key. Requires valid() == true.
    virtual types::RowKey row_key() const = 0;

    // Returns the full current row (all columns). Requires valid() == true.
    virtual types::InternalRow row() const = 0;

    // Returns any error encountered during iteration.
    // If status is not OK, valid() must return false.
    virtual absl::Status status() const = 0;
};

} // namespace pl::sstv2
```

**设计要点：**

- `seek(RowKey)` 实现 lower_bound 语义：定位到第一个 >= target 的行
- `seek_for_prev(RowKey)` 实现反向定位：定位到最后一个 <= target 的行
- target 可以是完整 RowKey（N 列）也可以是前缀 RowKey（前 K 列）——前缀 RowKey encode 后天然短于完整 key，memcmp lower_bound 正确
- `prev()` 支持反向遍历，便于实现 upper_bound 后的反向扫描
- `row_key()` 返回结构化的 `RowKey`（从 encoded bytes 解码），调用方可按列访问
- `status()` 分离错误状态与"无更多数据"——valid()==false 时需检查 status() 区分正常结束与 I/O 错误

**内部实现细节：** Iterator 内部实际使用 encoded bytes 做 seek（传给 BlockReader），
但对外暴露结构化 RowKey。encode/decode 在 Iterator 实现层完成，不暴露给调用方。

### 6.2 BlockIterator（block 内遍历）

```cpp
namespace pl::sstv2::block {

// Iterates rows within a single block.
// Lightweight, does not own block data (caller must keep data alive).
class BlockIterator : public Iterator {
public:
    // Construct from an already-opened BlockReader.
    // The BlockReader (and its underlying data) must outlive this iterator.
    explicit BlockIterator(const BlockReader& reader, const types::InternalSchema& schema);

    void seek(const types::RowKey& target) override;
    void seek_for_prev(const types::RowKey& target) override;
    void seek_to_first() override;
    void seek_to_last() override;
    void next() override;
    void prev() override;
    bool valid() const override;
    types::RowKey row_key() const override;
    types::InternalRow row() const override;
    absl::Status status() const override;

private:
    const BlockReader& reader_;
    const types::InternalSchema& schema_;
    size_t current_row_idx_ = 0;
    size_t num_rows_ = 0;
    bool valid_ = false;

    // Cached encoded target for binary search
    std::string encoded_target_;
};

} // namespace pl::sstv2::block
```

**seek 实现（block 内 lower_bound）：**
```
BlockIterator::seek(target):
    1. Encode target RowKey to memcomparable bytes -> encoded_target_
       (prefix RowKey encodes to shorter bytes, which is naturally < any full key with same prefix)
    2. Binary search over [0, num_rows_) using reader_.row_key(i) (encoded bytes)
       Find first i where reader_.row_key(i) >= encoded_target_
    3. If found: current_row_idx_ = i, valid_ = true
       Else: valid_ = false
```

**seek_for_prev 实现（block 内反向定位）：**
```
BlockIterator::seek_for_prev(target):
    1. Encode target RowKey to memcomparable bytes -> encoded_target_
    2. Binary search over [0, num_rows_) using reader_.row_key(i)
       Find last i where reader_.row_key(i) <= encoded_target_
    3. If found: current_row_idx_ = i, valid_ = true
       Else: valid_ = false
```

**row_key() 实现：**
```
BlockIterator::row_key():
    Get encoded bytes: reader_.row_key(current_row_idx_)
    Decode using schema_.key_column_types() + schema_.key_sort_orders()
    Return decoded RowKey
```

### 5.3 SSTableIterator（跨 block 遍历）

```cpp
namespace pl::sstv2::file {

// Iterates rows across all blocks in an SSTable.
// Implements seek/scan/prefix-scan via index tree navigation.
class SSTableIterator : public Iterator {
public:
    // Constructed by SSTableReader. Does not own the reader.
    explicit SSTableIterator(const SSTableReader& reader);

    void seek(std::string_view target) override;
    void seek_for_prev(std::string_view target) override;
    void seek_to_first() override;
    void seek_to_last() override;
    void next() override;
    void prev() override;
    bool valid() const override;
    std::string_view row_key() const override;
    types::InternalRow row() const override;
    absl::Status status() const override;

private:
    // Navigate index tree to find the data block containing target
    void seek_to_block(std::string_view target);
    void seek_to_block_for_prev(std::string_view target);

    // Load block at given offset/size, construct BlockIterator
    void load_block(uint64_t offset, uint32_t size);

    // Move to next/prev data block via index navigation
    void advance_block();
    void retreat_block();

    const SSTableReader& reader_;
    absl::Status status_ = absl::OkStatus();

    // Current block state
    std::string current_block_data_;  // keeps block bytes alive
    std::optional<block::BlockReader> current_block_reader_;
    std::optional<block::BlockIterator> current_block_iter_;

    // Index navigation state (tracks position in leaf index blocks)
    struct BlockRef { uint64_t offset; uint32_t size; std::string last_key; };
    std::vector<BlockRef> leaf_refs_;  // entries from current leaf index block
    size_t current_leaf_ref_idx_ = 0;

    // For navigating between leaf index blocks
    struct LeafIndexPos {
        uint64_t leaf_offset;
        uint32_t leaf_size;
    };
    std::vector<LeafIndexPos> leaf_positions_;  // all leaf index blocks (lazy populated)
    size_t current_leaf_idx_ = 0;
};

} // namespace pl::sstv2::file
```

### 5.4 SSTableIterator 核心流程

**seek(target) -- lower_bound 语义：**
```
SSTableIterator::seek(target):
    1. Navigate index tree from root:
       At each level, binary search for first entry whose last_key >= target
       Descend to that child block
       Repeat until reaching leaf level
    2. In leaf index block:
       Find first data block ref whose last_key >= target
       Load that data block
    3. Within the data block:
       current_block_iter_->seek(target)
       If !current_block_iter_->valid():
         // target > all keys in this block (edge case: index last_key was equal)
         advance_block()
         if valid(): current_block_iter_->seek_to_first()
```

**seek_for_prev(target) -- 定位到 <= target 的最后一行：**
```
SSTableIterator::seek_for_prev(target):
    1. Navigate index tree: find the last data block whose first_key <= target
       (or equivalently, find the block containing target via lower_bound,
        then check if we need the previous block)
    2. Within the data block:
       current_block_iter_->seek_for_prev(target)
       If !current_block_iter_->valid():
         retreat_block()
         if valid(): current_block_iter_->seek_to_last()
```

**next():**
```
SSTableIterator::next():
    current_block_iter_->next()
    if !current_block_iter_->valid():
        advance_block()
        if valid(): current_block_iter_->seek_to_first()
```

**prev():**
```
SSTableIterator::prev():
    current_block_iter_->prev()
    if !current_block_iter_->valid():
        retreat_block()
        if valid(): current_block_iter_->seek_to_last()
```

### 5.5 基于 Iterator 的上层 API

SSTableReader 使用 Iterator 抽象来实现所有读取操作：

```cpp
class SSTableReader {
public:
    static absl::StatusOr<SSTableReader> open(std::string_view path, io::FileSystem& fs);

    const types::InternalSchema& schema() const;

    // Create an iterator over the entire SSTable.
    // Caller uses seek/seek_to_first/next to navigate.
    std::unique_ptr<Iterator> new_iterator() const;

    // Point lookup (convenience, uses bloom + iterator internally)
    absl::StatusOr<std::optional<types::InternalRow>> get(std::string_view row_key) const;

    // Metadata
    const Tail& tail() const;
    const Locator& locator() const;

private:
    friend class SSTableIterator;

    absl::StatusOr<std::string> read_block_data(uint64_t offset, uint32_t size) const;
    types::InternalRow read_row(const block::BlockReader& br, size_t row_idx) const;

    std::unique_ptr<io::ReadableFile> file_;
    Tail tail_;
    Locator locator_;
    types::InternalSchema schema_;

    uint64_t index_root_offset_ = 0;
    uint32_t index_root_size_ = 0;
    uint8_t index_height_ = 0;

    std::optional<bloom::BloomReader> bloom_reader_;

    std::vector<size_t> fixed_col_indices_;
    std::vector<size_t> var_col_indices_;
};
```

**上层使用模式：**

```cpp
// Full scan
auto iter = reader.new_iterator();
for (iter->seek_to_first(); iter->valid(); iter->next()) {
    process(iter->row());
}

// Range scan [start, end)
auto iter = reader.new_iterator();
for (iter->seek(start_key); iter->valid() && iter->row_key() < end_key; iter->next()) {
    process(iter->row());
}

// Prefix scan (all rows with given prefix)
auto iter = reader.new_iterator();
for (iter->seek(prefix); iter->valid() && starts_with(iter->row_key(), prefix); iter->next()) {
    process(iter->row());
}

// Seek to exact key (lower_bound)
auto iter = reader.new_iterator();
iter->seek(target_key);
if (iter->valid() && iter->row_key() == target_key) { /* found */ }

// Reverse scan from upper_bound
auto iter = reader.new_iterator();
iter->seek_for_prev(upper_bound);
for (; iter->valid() && iter->row_key() >= lower_bound; iter->prev()) {
    process(iter->row());
}
```

### 5.6 get() 实现（基于 Iterator + Bloom）

```cpp
absl::StatusOr<std::optional<InternalRow>> SSTableReader::get(std::string_view key) const {
    // Bloom filter fast path
    if (bloom_reader_ && !bloom_reader_->may_contain(key)) {
        return std::nullopt;
    }

    auto iter = new_iterator();
    iter->seek(key);

    if (!iter->status().ok()) return iter->status();
    if (!iter->valid() || iter->row_key() != key) return std::nullopt;

    return iter->row();
}
```

### 5.7 BlockReader -> InternalRow 恢复

```cpp
types::InternalRow SSTableReader::read_row(const block::BlockReader& br, size_t row_idx) const {
    InternalRow row(schema_);

    // sub_col 0 = row_key
    row.set(0, Variant::string(br.row_key(row_idx)));

    for (size_t i = 0; i < fixed_col_indices_.size(); ++i) {
        size_t schema_idx = fixed_col_indices_[i];
        if (br.is_fixed_null(row_idx, i)) {
            row.set_null(schema_idx);
        } else {
            row.set(schema_idx, Variant::uint64(br.get_fixed(row_idx, i)));
        }
    }

    for (size_t i = 0; i < var_col_indices_.size(); ++i) {
        size_t schema_idx = var_col_indices_[i];
        if (br.is_var_null(row_idx, i)) {
            row.set_null(schema_idx);
        } else {
            row.set(schema_idx, Variant::string(br.get_var(row_idx, i)));
        }
    }

    return row;
}
```

### 5.8 open() 流程

```
SSTableReader::open(path, fs):
    1. file_ = fs.open_readable(path)
    2. Read last 32B -> decode Tail -> verify magic + checksum
    3. Read locator section -> decode Locator
    4. Deserialize Schema section -> ExternalSchema -> InternalSchema
    5. Record index root_offset / root_size / height from Locator
    6. Construct BloomReader from Bloom section (if present)
    7. Precompute fixed_col_indices_ / var_col_indices_
```

---

## 6. MultiPrefixCompressor 集成

### 6.1 定位

MultiPrefixCompressor **只用于 row_key 列**（V1）。原因：row_key 在 block 内天然有序，
前缀压缩效果最好。其他 var sub-column 的值在 block 内按 row_key 顺序排列，不保证值自身有序，
前缀压缩收益不确定。

其他 fixed sub-column 靠 **Pattern 编码**（Constant/Delta/StreamVByte/Dict/PFor 等）压缩——
这已经在 ColumnStore 中自动完成（PatternSelector 选择最优 pattern）。
其他 var sub-column 暂时存原始值，后续可通过 "值排序 + 重映射" 方式引入前缀压缩。

### 6.2 集成位置：BlockWriter 内部

在 `BlockWriter::finish()` 时，对已收集的 row_key 序列做 MultiPrefixCompress：

```cpp
struct BlockWriterOptions {
    ...
    bool compress_row_keys = true;  // enable row_key prefix compression
    compress::MultiPrefixConfig multi_prefix_config;
};
```

**写入路径：**
```
BlockWriter::finish():
    1. if compress_row_keys:
       Collect all row_keys -> already sorted
       MultiPrefixCompressor::compress(row_keys) -> compressed_data + prefix_dir
       Replace original row_key entries in DataTable with compressed_data
       Serialize prefix_dir and append to payload tail
    2. Rest unchanged (ColumnStore, OffsetTable, RowKeyBitmap)
```

### 6.3 BlockHeader 扩展

```cpp
struct BlockHeader {
    ...
    uint32_t prefix_dir_size = 0;  // 0 means MultiPrefix not used
};
```

Payload layout becomes: `[DataTable | ColumnStore | OffsetTable | RowKeyBitmap | PrefixDir]`

### 6.4 读取路径

```
BlockReader::open():
    if header.prefix_dir_size > 0:
        Parse PrefixDir
        row_key(i) / get_var() calls decompress_one() to recover original value
```

---

## 7. IndexTreeBuilder 改造详设

### 7.1 当前问题

```cpp
IndexTreeBuilder::IndexTreeBuilder(IndexTreeOptions opts)
    : opts_(opts), next_index_offset_(opts.index_region_offset) {}
```

`index_region_offset` 假设所有 index block 存放在一个连续区域，
意味着 data blocks 必须全写完才能开始写 index——不符合交错布局。

### 7.2 改造方案（回调式）

```cpp
using WriteBlockFn = std::function<uint64_t(std::string_view block_data)>;
// Semantics: write block_data to storage, return its actual file offset.

class IndexTreeBuilder {
public:
    explicit IndexTreeBuilder(IndexTreeOptions opts, WriteBlockFn write_fn);

    void add_data_block(std::string_view last_key, uint64_t offset, uint32_t size);

    struct Result {
        uint64_t root_offset = 0;
        uint32_t root_size = 0;
        uint8_t tree_height = 0;
    };
    Result finish();

private:
    void flush_level(size_t level);
    void add_to_level(size_t level, std::string_view last_key, uint64_t offset, uint32_t size);

    IndexTreeOptions opts_;
    WriteBlockFn write_fn_;
    std::vector<Level> levels_;
    bool finished_ = false;
    bool has_data_ = false;
    uint64_t last_flushed_offset_ = 0;
    uint32_t last_flushed_size_ = 0;
};
```

### 7.3 flush_level 改造

```cpp
void IndexTreeBuilder::flush_level(size_t level) {
    auto& writer = levels_[level].writer;
    if (writer.empty()) return;

    std::string block = writer.finish();
    std::string promote_key(writer.last_key());

    uint64_t block_offset = write_fn_(block);  // write via callback, get actual offset
    auto block_size = static_cast<uint32_t>(block.size());

    last_flushed_offset_ = block_offset;
    last_flushed_size_ = block_size;
    writer.reset();

    add_to_level(level + 1, promote_key, block_offset, block_size);
}
```

### 7.4 finish() 改造

```cpp
IndexTreeBuilder::Result IndexTreeBuilder::finish() {
    Result result;
    if (finished_ || !has_data_) { finished_ = true; return result; }
    finished_ = true;

    for (size_t level = 0; level < levels_.size(); ++level) {
        auto& writer = levels_[level].writer;
        if (writer.empty()) continue;

        bool is_top = (level + 1 == levels_.size());
        if (is_top && writer.count() == 1) {
            std::string block = writer.finish();
            uint64_t root_offset = write_fn_(block);
            result.root_offset = root_offset;
            result.root_size = static_cast<uint32_t>(block.size());
            result.tree_height = static_cast<uint8_t>(level + 1);
            return result;
        }
        flush_level(level);
    }

    // Fallthrough: last flush was root
    result.root_offset = last_flushed_offset_;
    result.root_size = last_flushed_size_;
    result.tree_height = static_cast<uint8_t>(levels_.size());
    return result;
}
```

### 7.5 对已有测试的影响

已有的 `index_tree_builder_test` 需要适配：提供 mock write_fn：

```cpp
std::vector<std::string> written_blocks;
uint64_t next_offset = 0;

auto write_fn = [&](std::string_view data) -> uint64_t {
    uint64_t offset = next_offset;
    written_blocks.emplace_back(data);
    next_offset += data.size();
    return offset;
};

IndexTreeBuilder builder(opts, write_fn);
```

---

## 8. 设计决策记录

| # | 决策点 | 结论 | 理由 |
|---|--------|------|------|
| D1 | String `.len` sub-column | 不写入 ColumnStore | OffsetTable 已冗余记录 length |
| D2 | KV 分离 | V1 不实现，预留扩展 | 列存下单列 var 值不大，通过 Options + ValueFileWriter 接口预留 |
| D3 | MultiPrefixCompressor 范围 | 只压缩 row_key 列 | row_key 天然有序效果好；其他 var column 走 Pattern 编码 |
| D4 | InternalRow 拆解位置 | SSTableBuilder 中做 | BlockWriter 保持 low-level，职责单一 |
| D5 | 文件写入 | 抽象 WritableFile 接口 | 支持内存/本地文件/分布式文件系统，高效流式 append |
| D6 | 文件读取 | 抽象 ReadableFile 接口 | 支持 pread/mmap/分布式读，零拷贝 view() |
| D7 | 文件系统 | 抽象 FileSystem 接口 | 本地 fs 为默认，后续可扩展 HDFS/S3 等 |
| D8 | Iterator 设计 | 统一抽象接口 + 分层实现 | BlockIterator/SSTableIterator/未来 MergeIterator 共享接口，支持 seek/prev 双向遍历 |

---

## 9. 实施计划

```
Phase 0: I/O 抽象层
  - io/writable_file.h: WritableFile 抽象接口
  - io/readable_file.h: ReadableFile 抽象接口
  - io/file_system.h: FileSystem 抽象接口
  - io/memory_file.h/.cpp: InMemoryWritableFile + InMemoryReadableFile (for tests)
  - io/posix_file.h/.cpp: PosixWritableFile + PosixReadableFile + PosixFileSystem
  - Unit tests for both implementations

Phase 1: Schema 序列化 + IndexTreeBuilder 改造
  - types/schema.h 新增 serialize/deserialize
  - IndexTreeBuilder 注入 WriteBlockFn 回调
  - flush_level() 通过回调写出，回调返回实际 offset
  - finish() 将剩余 blocks 通过回调写出
  - Result 精简为 root_offset + root_size + tree_height
  - 适配已有 index 单元测试
  - Schema 序列化单元测试

Phase 2: SSTableBuilder 重写
  - 构造接收 InternalSchema + FileSystem
  - InternalRow 拆解：跳过 .len sub-column，分离 fixed/var
  - 接入 BlockWriter + flush_block
  - 接入 IndexTreeBuilder（回调绑定到 WritableFile::append）
  - 接入 BloomBuilder
  - 写 Metadata sections（Configuration, Schema, Statistics）
  - 写 Locator + Tail
  - 单元测试（使用 InMemoryWritableFile）

Phase 3: Iterator 抽象 + SSTableReader 重写
  - iterator/iterator.h: Iterator 抽象接口（seek/seek_for_prev/next/prev/valid/row_key/row/status）
  - block/block_iterator.h/.cpp: BlockIterator 实现（block 内二分 seek + 双向遍历）
  - file/sstable_iterator.h/.cpp: SSTableIterator 实现（跨 block，index tree 导航）
  - file/sstable_reader.h/.cpp: SSTableReader 重写
    - open(): 解析 Tail -> Locator -> Schema -> Bloom -> Index root
    - new_iterator(): 返回 SSTableIterator
    - get(): bloom 过滤 + iterator seek
  - 单元测试（使用 InMemoryReadableFile）
    - BlockIterator: seek/seek_for_prev/next/prev 验证
    - SSTableIterator: 跨 block seek、prefix scan、reverse scan
    - get(): bloom 命中/未命中、精确匹配/未找到

Phase 4: MultiPrefix 集成 (row_key only)
  - BlockWriter 增加 row_key 前缀压缩选项
  - BlockReader 对应解压
  - 单元测试

Phase 5: 端到端集成测试
  - 写入 N 行 -> 读取验证
  - 边界：空表、单行、单 block、多 block
  - 大 key/value、null 值、全类型覆盖
  - 验证 data block 和 index block 交错正确性
  - FileSystem 抽象验证（InMemory / Posix 双跑）
```

---

## 10. 约束与不变量

1. **row_key 必须有序** -- SSTableBuilder::add() 强制检查
2. **一个 block 至少 1 行** -- BlockWriter 已保证（第一行永远成功）
3. **InternalSchema 不可变** -- 一个 SST 文件绑定一个 schema，写入后不可修改
4. **Locator 必须是文件中倒数第二个结构**（Tail 是最后）
5. **所有 offset 都是相对文件起始的绝对偏移**
6. **block 内 row_key 可重复**（RowKeyBitmap 标记重复行）
7. **WritableFile::append 接受 string_view** -- 不强制额外 copy
8. **FileSystem 无状态** -- 仅负责 open/create/delete，不持有文件句柄
9. **ReadableFile 生命周期内文件不可变** -- 不支持并发写+读同一文件
10. **Iterator valid()==false 时必须检查 status()** -- 区分正常结束与 I/O 错误
11. **Iterator 不拥有底层数据** -- BlockIterator 不拥有 BlockReader，SSTableIterator 不拥有 SSTableReader
12. **seek(prefix) 等价于 lower_bound(prefix)** -- row_key 有序保证前缀查询正确性
