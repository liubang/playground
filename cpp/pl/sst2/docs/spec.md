# SSTableV2 Design Specification

> **Version**: 1.0
> **Implementation**: C++20, clean-slate in `cpp/pl/sst2/`

---

## 1. Design Goals & Principles

| Principle | Description |
|-----------|-------------|
| **Clean abstraction** | Each module has a single, well-defined responsibility. Interfaces are minimal and orthogonal. |
| **Zero-copy where possible** | `std::span<const std::byte>` and `std::string_view` for reading; structured builders for writing. |
| **No exceptions for control flow** | `absl::Status` and `absl::StatusOr<T>` for all recoverable errors. |
| **Immutable after write** | SST files are immutable once written. Readers are inherently thread-safe. Builders are single-threaded. |
| **Pattern as fundamental encoding** | Pattern storage is NOT an optimization afterthought — it is how column data is stored. Every column store unit is pattern-encoded. |
| **Schema-driven everything** | Column layout, sub-column decomposition, and comparison semantics all derive from the schema. |

### What This Spec Covers

This document provides a complete module-level design for implementing the SSTableV2 format. It defines module boundaries, key class interfaces, data flow, and build system structure.

It does **not** cover MemTable, WAL, or Compaction — those are consumers of SSTableV2.

### Relationship to `cpp/pl/sst/`

This is a **clean-slate implementation**. No code is reused from the existing SST V1. The V1 format is a LevelDB-style row-store; V2 is a column-store with fundamentally different architecture.

---

## 2. Library Selection: Abseil vs Folly

### Choice: **Abseil** (`abseil-cpp`)

### Rationale

**1. `absl::Cord` Is Uniquely Valuable for Data Table Construction**

The multi-round prefix compression algorithm produces strings as fragmented pieces (prefix + suffix fragments). `absl::Cord` is purpose-built for efficiently representing concatenated string fragments without copying — this maps directly to the data table's physical layout where a single logical value may be scattered across multiple compression rounds. Folly has `folly::IOBuf` for I/O buffer chains, but `absl::Cord` provides the string-level abstraction needed.

**2. `absl::Status` / `absl::StatusOr` Has Richer Error Semantics**

`absl::Status` supports error codes, messages, and typed payloads via `SetPayload()`. This allows propagating structured error information (e.g., "CRC mismatch at offset X") through the call stack. Paired with `ABSL_RETURN_IF_ERROR` and `ABSL_ASSIGN_OR_RETURN` macros, error handling is concise.

**3. Stability Guarantees for a Storage Format**

Abseil provides formal LTS releases with 3-year compatibility guarantees. A storage format library must be stable — file format bugs are unrecoverable.

**4. Modular Link-time Footprint**

Abseil is designed for granular linking: `//absl/status`, `//absl/strings`, `//absl/container/*` are independent targets. Folly's dependency graph is more intertwined.

**5. Alignment with the Storage Ecosystem**

Abseil is the foundation library for Protocol Buffers, gRPC, and the Google storage stack. Its design patterns (`Cord`, `Span`, `StatusOr`) are battle-tested for storage system use cases.

### What We Use from Abseil

| Component | Purpose in SSTableV2 |
|-----------|---------------------|
| `absl::Status` / `absl::StatusOr<T>` | All error handling |
| `absl::Cord` | Data table buffer management |
| `absl::Span<T>` | Zero-copy views over encoded data |
| `absl::InlinedVector<T, N>` | Small-size-optimized vectors (block-level buffers) |
| `absl::FixedArray<T>` | Stack-allocated temporary arrays |
| `absl::flat_hash_map` / `absl::btree_map` | Metadata storage, index lookups |
| `absl::bit_cast` | Safe type-punning for integer encoding |
| `absl::crc32c` | CRC32C checksum |

### Dependency

```python
# MODULE.bazel
bazel_dep(name = "abseil-cpp", version = "20250127.1")
```

---

## 3. Directory Structure

```
cpp/pl/sst2/
├── BUILD
├── docs/
│   └── design_spec.md
│
├── types/                         # Type system: Variant, Schema, constants
│   ├── BUILD
│   ├── data_type.h                # DataType enum + classification traits
│   ├── variant.h                  # Variant class declaration
│   ├── variant.cpp
│   ├── schema.h                   # ExternalSchema, InternalSchema
│   ├── schema.cpp
│   ├── internal_row.h             # InternalRow: typed wrapper over a row
│   ├── constants.h                # All magic numbers, version constants
│   └── flag.h                     # Flag bitfield (DataType + C + B bits)
│
├── encode/                        # Integer encoding layer
│   ├── BUILD
│   ├── varints.h                  # Varints + ZigZag encoder/decoder
│   ├── varints.cpp
│   ├── stream_vbyte.h             # Stream VByte codec (used by Pattern 1)
│   ├── stream_vbyte.cpp
│   └── fixed.h                    # Fixed-size LE encoding (memcpy-based)
│
├── pattern/                       # Pattern storage: 7 encoding modes
│   ├── BUILD
│   ├── pattern_encoder.h          # Abstract PatternEncoder interface
│   ├── pattern_decoder.h          # Abstract PatternDecoder interface
│   ├── pattern_selector.h         # Auto-select best pattern for a column
│   ├── pattern_selector.cpp
│   ├── pattern_none.h/cpp         # Mode 0: No encoding
│   ├── pattern_stream_vbyte.h/cpp # Mode 1: Stream VByte
│   ├── pattern_pfor.h/cpp         # Mode 2: PFOR
│   ├── pattern_dict.h/cpp         # Mode 3: Dictionary encoding
│   ├── pattern_delta.h/cpp        # Mode 4 & 5: Equal-step increment/decrement
│   └── pattern_compound.h/cpp     # Compound pattern (for composite types)
│
├── compress/                      # String and block compression
│   ├── BUILD
│   ├── multi_prefix.h             # Multi-round prefix compression
│   ├── multi_prefix.cpp
│   ├── block_compressor.h         # Thin wrapper over Snappy/ZSTD
│   └── block_compressor.cpp
│
├── block/                         # Block construction and reading
│   ├── BUILD
│   ├── block_header.h/cpp         # 52-byte block header
│   ├── data_table.h/cpp           # Data table (variable-length area)
│   ├── column_store.h/cpp         # Column store area (pattern-encoded)
│   ├── offset_table.h/cpp         # Offset table (varints-encoded)
│   ├── rowkey_bitmap.h/cpp        # Row key repetition bitmap
│   ├── block_writer.h/cpp         # BlockWriter: assembles a complete block
│   └── block_reader.h/cpp         # BlockReader: parses and queries a block
│
├── index/                         # Index tree: multi-level B-tree index
│   ├── BUILD
│   ├── index_block_writer.h/cpp   # Index block writer
│   ├── index_tree_builder.h/cpp   # Index tree builder (post-order)
│   └── index_iterator.h/cpp       # Two-level iterator for range scans
│
├── bloom/                         # Bloom filter
│   ├── BUILD
│   ├── bloom_format.h             # On-disk bloom filter format
│   ├── bloom_builder.h/cpp        # Bloom filter builder
│   └── bloom_reader.h/cpp         # Bloom filter reader
│
├── metadata/                      # Metadata sections
│   ├── BUILD
│   ├── metadata_section.h/cpp     # Common section format (magic + checksum + map)
│   ├── configuration.h/cpp        # Configuration section
│   ├── schema_meta.h/cpp          # Schema metadata section
│   ├── statistics.h/cpp           # Statistics section
│   ├── compatibility.h/cpp        # Compatibility section
│   └── user_defined.h/cpp         # User-defined data section
│
├── file/                          # File layer: builder, reader, locator, tail
│   ├── BUILD
│   ├── locator.h/cpp              # Locator section
│   ├── tail.h/cpp                 # Tail (32-byte footer)
│   ├── value_file_writer.h/cpp    # Value file write logic
│   ├── value_file_reader.h/cpp    # Value file read logic
│   ├── sstable_builder.h/cpp      # Top-level SST file builder
│   └── sstable_reader.h/cpp       # Top-level SST file reader
│
└── ut/                            # Unit tests
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
        └── sstable_test.cpp       # Integration: build + read round-trip
```

### Dependency Graph

```
                          ┌─────────────┐
                          │   file/     │  (top-level builder/reader)
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
                        │    types/     │  (leaf module, no internal deps)
                        └───────────────┘
```

**Rules:**
- `types/` depends on nothing (except Abseil)
- `encode/` depends only on `types/`
- `pattern/` depends on `types/` and `encode/`
- `compress/` depends only on `types/`
- `block/` depends on `types/`, `encode/`, `pattern/`, `compress/`
- `index/` depends on `block/`
- `bloom/` depends on `types/`
- `metadata/` depends on `types/` and `encode/`
- `file/` depends on all other modules

---

## 4. Type System Module (`types/`)

The leaf module. Everything depends on it.

### 4.1 `DataType` Enum (`data_type.h`)

```cpp
namespace pl::sst2 {

enum class DataType : uint8_t {
  // None type
  kNone = 0,

  // Boolean
  kBool = 1,

  // Signed integers
  kInt8 = 2, kInt16 = 4, kInt32 = 6, kInt64 = 8,

  // Unsigned integers
  kUint8 = 3, kUint16 = 5, kUint32 = 7, kUint64 = 9,

  // Floating point
  kFloat = 10, kDouble = 11, kLongDouble = 12,

  // Time types
  kTime = 13, kVersion = 14,

  // String types (variable-length)
  kString = 15, kU16String = 16, kU32String = 17, kBinary = 18,

  // Compound types
  kArray = 19, kMap = 20,

  // Private types (internal use only, not in user schema)
  kDataBlock = 21, kIndexBlock = 22,
};

// === Classification ===

constexpr bool is_none(DataType t)        { return t == DataType::kNone; }
constexpr bool is_bool(DataType t)        { return t == DataType::kBool; }
constexpr bool is_signed_integer(DataType t);
constexpr bool is_unsigned_integer(DataType t);
constexpr bool is_integral(DataType t);
constexpr bool is_floating_point(DataType t);
constexpr bool is_fixed_size(DataType t);    // All arithmetic + Bool + Time + Version
constexpr bool is_variable_size(DataType t);  // String types + Binary
constexpr bool is_string_type(DataType t);
constexpr bool is_compound(DataType t);      // Array, Map
constexpr bool is_private(DataType t);       // DataBlock, IndexBlock
constexpr bool is_public(DataType t);        // Not None and not private

// Returns the fixed size in bytes for fixed-size types, 0 otherwise
constexpr size_t fixed_size_in_bytes(DataType t);

// Human-readable name for debugging
constexpr std::string_view data_type_name(DataType t);

}  // namespace pl::sst2
```

### 4.2 `Variant` Class (`variant.h`)

`Variant` is the universal value container. It can hold any of the 23 types.

**Storage model:**
- Fixed-size types (Bool, integers, floats, Time, Version): stored inline in a union
- Variable-length types (String, U16String, U32String, Binary): data is **copied** (owned), with pointer + length stored inline
- Compound types (Array, Map): heap-allocated via unique_ptr

```cpp
namespace pl::sst2 {

// Internal storage for variable-length types
struct PointerAndLength {
  const char* pointer = nullptr;
  uint64_t length = 0;
};

class Variant {
 public:
  // === Construction ===
  Variant();  // Default: kNone type

  // Factory methods — one per supported type
  static Variant None();
  static Variant Bool(bool v);
  static Variant Int8(int8_t v);
  static Variant Uint8(uint8_t v);
  static Variant Int16(int16_t v);
  static Variant Uint16(uint16_t v);
  static Variant Int32(int32_t v);
  static Variant Uint32(uint32_t v);
  static Variant Int64(int64_t v);
  static Variant Uint64(uint64_t v);
  static Variant Float(float v);
  static Variant Double(double v);
  static Variant LongDouble(long double v);

  struct TimeValue { int64_t seconds; uint32_t nanoseconds; };
  static Variant Time(TimeValue v);

  struct VersionValue { uint64_t major; uint64_t minor; };
  static Variant Version(VersionValue v);

  // Variable-length: takes ownership (copies data)
  static Variant String(std::string_view v);
  static Variant U16String(std::u16string_view v);
  static Variant U32String(std::u32string_view v);
  static Variant Binary(std::string_view v);

  // Compound types
  using ArrayType = std::vector<Variant>;
  using MapType = std::vector<std::pair<Variant, Variant>>;  // Sorted by key
  static Variant Array(ArrayType v);
  static Variant Map(MapType v);

  // === Accessors ===
  DataType type() const { return type_; }
  bool is_none() const { return type_ == DataType::kNone; }

  // Typed accessors — CHECK-fail if type mismatches
  bool                as_bool() const;
  int8_t              as_int8() const;
  uint8_t             as_uint8() const;
  int16_t             as_int16() const;
  uint16_t            as_uint16() const;
  int32_t             as_int32() const;
  uint32_t            as_uint32() const;
  int64_t             as_int64() const;
  uint64_t            as_uint64() const;
  float               as_float() const;
  double              as_double() const;
  long double         as_long_double() const;
  TimeValue           as_time() const;
  VersionValue        as_version() const;
  std::string_view    as_string() const;
  std::u16string_view as_u16string() const;
  std::u32string_view as_u32string() const;
  std::string_view    as_binary() const;
  const ArrayType&    as_array() const;
  const MapType&      as_map() const;

  // === Comparison ===
  //
  // Follows spec Section 2.3.5:
  //   - None == None; Bool: false < true
  //   - Integer/float: by value; String/Binary: memcmp, shorter < longer if prefix matches
  //   - Array: lexicographic by element, then by length
  //   - Map: key-then-value lexicographic
  int compare(const Variant& other) const;

  bool operator==(const Variant& other) const { return compare(other) == 0; }
  bool operator!=(const Variant& other) const { return compare(other) != 0; }
  bool operator<(const Variant& other) const  { return compare(other) < 0; }

  // === Serialization (per spec Section 2.3) ===
  //
  // Fixed-size: [type_byte][value...]
  //   - Integers > 8-bit: varints (unsigned) or zigzag+varints (signed) encoding
  //   - Float/Double/LongDouble: direct LE storage
  //   - Time: zigzag+varints seconds + varints nanoseconds
  //   - Version: varints major + varints minor
  // Variable-size: [type_byte][length(varints)][data...]
  // Compound: [type_byte][count(varints)][element0][element1]...
  void encode_to(std::string* out) const;
  std::string encode() const;
  static absl::StatusOr<Variant> decode_from(std::string_view* in);

  // Estimated encoded size for buffer planning
  size_t encoded_size_estimate() const;

 private:
  DataType type_ = DataType::kNone;

  struct HeapData {
    std::unique_ptr<char[]> string_data;     // Owns variable-length data
    std::unique_ptr<ArrayType> array_data;
    std::unique_ptr<MapType> map_data;
  };

  struct alignas(16) Storage {
    union {
      bool bool_val;
      int8_t int8_val;       uint8_t uint8_val;
      int16_t int16_val;     uint16_t uint16_val;
      int32_t int32_val;     uint32_t uint32_val;
      int64_t int64_val;     uint64_t uint64_val;
      float float_val;       double double_val;
      long double long_double_val;
      TimeValue time_val;
      VersionValue version_val;
      PointerAndLength str_val;     // Pointer+length for variable-length data
    };
    std::unique_ptr<HeapData> heap;
  } storage_;
};

}  // namespace pl::sst2
```

**Design decisions:**
- String/binary data is **copied** into the Variant (ownership). This avoids lifetime issues when source buffers from memtable iterators become invalid.
- Compound types are stored by value, flat (no nested compound types — matches spec restriction).
- The comparison logic is centralized here; schema-based comparisons delegate to `Variant::compare()`.

### 4.3 Schema (`schema.h`)

The schema system is the central abstraction that defines how user data maps to the internal columnar representation.

```
External Schema (user-facing)          Internal Schema (storage-facing)
┌────────────────────────┐            ┌──────────────────────────────┐
│ Key1    (UInt32)       │            │ Key1         (UInt32)        │
│ Key2    (String)       │            │ Key2.offset  (UInt64)        │
│ Version (Version)      │            │ Key2.length  (UInt64)        │
│ OpType  (UInt8)        │            │ Version.major (UInt64)       │
│ Value   (any)          │            │ Version.minor (UInt64)       │
└────────────────────────┘            │ OpType       (UInt8)         │
                                      │ Flag         (UInt64)        │
                                      │ Filename.offset (UInt64)     │
                                      │ Filename.length (UInt64)     │
                                      │ Offset       (UInt64)        │
                                      │ Length       (UInt64)        │
                                      │ Checksum     (UInt64)        │
                                      └──────────────────────────────┘
                                              ↑
                                     All sub-columns are arithmetic types
                                     (fixed-size), even for variable-length
                                     source columns.
```

```cpp
namespace pl::sst2 {

// === Column Definition ===
struct ColumnDef {
  std::string name;
  DataType type;
  bool ascending = true;  // Sort order
};

// === External Schema ===
//
// Describes the user's table:
//   [0..R-1]  Row key columns (≥1, user-defined types)
//   [R]       Version column (kVersion)
//   [R+1]     OpType column (kUint8)
//   [R+2]     Value column (no fixed type, always last)
//
class ExternalSchema {
 public:
  class Builder;

  size_t column_count() const;              // Total including value
  size_t row_key_count() const;             // Number of row key columns
  size_t version_column_index() const;       // = row_key_count
  size_t op_type_column_index() const;       // = row_key_count + 1
  size_t all_key_column_count() const;       // = row_key_count + 2
  size_t value_column_index() const;         // = all_key_column_count

  const ColumnDef& column(size_t index) const;
  absl::Span<const ColumnDef> row_key_columns() const;
  absl::Span<const size_t> checksum_key_indices() const;
  const std::string& name() const;

  absl::Status validate() const;

 private:
  ExternalSchema();
  friend class Builder;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class ExternalSchema::Builder {
 public:
  Builder& set_name(std::string name);
  Builder& add_row_key_column(ColumnDef col);
  Builder& set_version_column();         // Auto-adds Version
  Builder& set_op_type_column();         // Auto-adds UInt8 OpType
  Builder& set_checksum_keys(std::vector<size_t> indices);
  absl::StatusOr<ExternalSchema> build();
};

// === Internal Schema ===
//
// Derived from ExternalSchema. Defines the internal table structure
// with logical columns and their sub-column decomposition.
//
// Logical columns (total = all_key_count + 5):
//   [0..R-1]          Row key columns (same as external)
//   [R]               Version
//   [R+1]             OpType
//   [R+2]             Flag      (UInt64)
//   [R+3]             Filename  (String)
//   [R+4]             Offset    (UInt64)
//   [R+5]             Length    (UInt64)
//   [R+6]             Checksum  (UInt64)
//
// Sub-columns: composite types (Version, Time) decompose into atomic
// arithmetic-type sub-columns. Variable-length types get offset+length
// sub-columns. After N rounds of multi-prefix compression, each variable-length
// column gets (N+1) pairs of offset/length sub-columns.
//
class InternalSchema {
 public:
  // Logical column count
  size_t logical_column_count() const;
  size_t all_key_column_count() const;

  // Fixed indices
  size_t flag_column_index() const;        // = all_key_column_count
  size_t filename_column_index() const;
  size_t offset_column_index() const;
  size_t length_column_index() const;
  size_t checksum_column_index() const;

  // === Sub-column Decomposition ===

  struct SubColumn {
    size_t logical_column_index;   // Parent logical column
    std::string name;              // e.g., "version.major", "key2.offset0"
    DataType type;                 // Always an arithmetic (fixed-size) type
  };

  size_t sub_column_count() const;
  const SubColumn& sub_column(size_t index) const;
  absl::Span<const SubColumn> sub_columns() const;

  struct SubColumnRange {
    size_t begin_index;  // First sub-column index
    size_t count;        // Number of sub-columns for this logical column
  };
  SubColumnRange sub_column_range(size_t logical_column_index) const;

  // Create from external schema
  static absl::StatusOr<InternalSchema> from_external(
      const ExternalSchema& external);

  // Create a variant with additional offset/length pairs for
  // multi-prefix compression (N rounds → N additional offset/length pairs).
  InternalSchema with_compression_rounds(size_t num_rounds) const;

  absl::Span<const size_t> checksum_key_indices() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pl::sst2
```

### 4.4 `Flag` (`flag.h`)

The Flag is a 64-bit bitfield stored in the internal table (spec Figure 20):

```
bits [0:4]   DT — DataType (5 bits, 0-31)
bit  [5]     B  — Bool value (only meaningful if DT == kBool)
bit  [6]     C  — Checksum: 0 = no checksum, 1 = present
bits [7:9]   —  — Reserved, must be 0
bits [10:63] —  — Reserved, must be 0
```

```cpp
namespace pl::sst2 {

class Flag {
 public:
  static constexpr uint64_t kDataTypeMask = 0x1F;
  static constexpr uint64_t kBoolBit      = (1ULL << 5);
  static constexpr uint64_t kChecksumBit  = (1ULL << 6);

  Flag() = default;
  explicit Flag(uint64_t raw) : raw_(raw) {}

  static Flag from_data_type(DataType dt);

  DataType data_type() const;
  bool bool_value() const;
  bool has_checksum() const;

  void set_data_type(DataType dt);
  void set_bool_value(bool v);
  void set_has_checksum(bool v);

  uint64_t raw() const { return raw_; }
  bool is_valid() const;  // Reserved bits must be 0

 private:
  uint64_t raw_ = 0;
};

}  // namespace pl::sst2
```

### 4.5 `InternalRow` (`internal_row.h`)

A structured view over one row's column values in the internal table:

```cpp
namespace pl::sst2 {

class InternalRow {
 public:
  explicit InternalRow(const InternalSchema& schema);

  // Access by logical column index
  const Variant& column(size_t index) const;
  void set_column(size_t index, Variant value);

  // Convenience for fixed internal columns
  const Variant& flag() const;
  const Variant& filename() const;
  const Variant& offset() const;
  const Variant& length() const;
  const Variant& checksum() const;

  // All-key columns
  absl::Span<const Variant> all_key_columns() const;
  size_t all_key_column_count() const;

  // Row comparison using schema-specified sort order
  int compare_all_key(const InternalRow& other) const;

 private:
  const InternalSchema& schema_;
  std::vector<Variant> columns_;
};

}  // namespace pl::sst2
```

### 4.6 Constants (`constants.h`)

```cpp
namespace pl::sst2::constants {

// Block magic codes
inline constexpr uint32_t kDataBlockMagic   = 0x4B425444;  // "DTBK"
inline constexpr uint32_t kIndexBlockMagic  = 0x4B425849;  // "IXBK"
inline constexpr uint32_t kRootIndexMagic   = 0x544F4F52;  // "ROOT"

// Bloom filter
inline constexpr uint32_t kBloomMagic       = 0x4D4F4C42;  // "BLOM"

// Metadata section magic codes
inline constexpr uint32_t kConfigMagic      = 0x47494643;  // "CFIG"
inline constexpr uint32_t kSchemaMetaMagic  = 0x414D4553;  // "SEMA"
inline constexpr uint32_t kStatisticsMagic  = 0x54415453;  // "STAT"
inline constexpr uint32_t kCompatibilityMagic = 0x504D4F43;  // "COMP"
inline constexpr uint32_t kUserDefinedMagic = 0x52455355;  // "USER"

// Locator and SST file
inline constexpr uint32_t kLocatorMagic     = 0x41434F4C;  // "LOCA"
inline constexpr uint32_t kSSTFileMagic     = 0x00545353;  // "SST\0"

// Version
inline constexpr uint32_t kSSTableV2Version = 2;

// Limits
inline constexpr size_t kBlockHeaderSize    = 52;
inline constexpr size_t kTailSize           = 32;
inline constexpr size_t kMaxPrefixRounds    = 15;

// Special filenames
inline constexpr const char* kInlineValueMarker = "@1";
inline constexpr const char* kCurrentFileMarker = "@2";

}  // namespace pl::sst2::constants
```

---

## 5. Integer Encoding Module (`encode/`)

### 5.1 Varints + ZigZag (`varints.h`)

```cpp
namespace pl::sst2::varints {

// === Varints (7-bit encoding) ===
//
// Each byte: 7 data bits + 1 continuation bit (MSB).
// MSB=1: more bytes follow. MSB=0: last byte.
//
// Max sizes: uint64 → 9 bytes, uint32 → 5 bytes, uint16 → 3 bytes

std::string encode_u64(uint64_t value);
std::string encode_u32(uint32_t value);
std::string encode_u16(uint16_t value);

// Decode from string_view, advancing the pointer.
// Returns kOutOfRange if value overflows.
absl::StatusOr<uint64_t> decode_u64(std::string_view* in);
absl::StatusOr<uint32_t> decode_u32(std::string_view* in);

// === ZigZag (signed → unsigned mapping) ===
//
//  0 → 0, -1 → 1, 1 → 2, -2 → 3, 2 → 4, ...

uint64_t zigzag_encode(int64_t value);
int64_t  zigzag_decode(uint64_t encoded);

// Convenience
inline std::string encode_i64(int64_t value) {
  return encode_u64(zigzag_encode(value));
}
inline absl::StatusOr<int64_t> decode_i64(std::string_view* in) {
  auto u = decode_u64(in);
  if (!u.ok()) return u.status();
  return zigzag_decode(*u);
}

// Direct buffer write (returns bytes written)
size_t encode_u64_to_buffer(uint64_t value, char* out);

// Maximum encoded sizes
inline constexpr size_t kMaxVarintsU64Size = 10;
inline constexpr size_t kMaxVarintsU32Size = 5;

}  // namespace pl::sst2::varints
```

### 5.2 Fixed-Size Encoding (`fixed.h`)

```cpp
namespace pl::sst2::fixed {

// Encode trivially-copyable T as little-endian bytes.
// On LE platforms: direct memcpy. On BE: byte-reversed memcpy.
template <typename T>
  requires std::is_trivially_copyable_v<T>
void encode_le(T value, char* out);

template <typename T>
  requires std::is_trivially_copyable_v<T>
T decode_le(const char* in);

// Convenience aliases
inline void encode_u64_le(uint64_t v, char* out) { encode_le(v, out); }
inline uint64_t decode_u64_le(const char* in) { return decode_le<uint64_t>(in); }
inline void encode_u32_le(uint32_t v, char* out) { encode_le(v, out); }
inline uint32_t decode_u32_le(const char* in) { return decode_le<uint32_t>(in); }

// Read from string_view, advancing it
template <typename T>
  requires std::is_trivially_copyable_v<T>
T read_le(std::string_view* in);

}  // namespace pl::sst2::fixed
```

### 5.3 Stream VByte (`stream_vbyte.h`)

Used by Pattern 1. Encodes a sequence of uint32 or uint64 values with fast sequential decode + random access via skip table.

```cpp
namespace pl::sst2::stream_vbyte {

// Encode a sequence of uint32 values.
// Format: [control_stream][data_stream]
// Each value's byte-length (1-4 for uint32) is encoded as 2 bits in control stream.
std::string encode_u32(absl::Span<const uint32_t> values);
std::string encode_u64(absl::Span<const uint64_t> values);

// Decoder with random access support
class DecoderU32 {
 public:
  explicit DecoderU32(std::string_view data);
  size_t size() const;
  uint32_t get(size_t i) const;

 private:
  std::string_view control_;
  std::string_view data_;
  size_t size_;
  std::vector<size_t> skip_table_;  // Every 128 values: cumulative data offset
};

class DecoderU64 {
 public:
  explicit DecoderU64(std::string_view data);
  size_t size() const;
  uint64_t get(size_t i) const;
};

// Small-value optimization: if all values fit in 1 byte, control stream is omitted
bool is_small_optimized_u32(absl::Span<const uint32_t> values);

}  // namespace pl::sst2::stream_vbyte
```

---

## 6. Pattern Storage Module (`pattern/`)

Pattern storage is the encoding system for column store units. Each column's values are encoded using exactly one pattern. The pattern **must** support random access (get the i-th value without decoding the entire sequence).

### 6.1 Pattern Interfaces

```cpp
namespace pl::sst2 {

enum class PatternType : uint8_t {
  kNone        = 0,    // Raw storage
  kStreamVByte = 1,    // Stream VByte varints
  kPfor        = 2,    // Patched Frame of Reference
  kDict        = 3,    // Dictionary encoding
  kDeltaInc    = 4,    // Equal-step increment
  kDeltaDec    = 5,    // Equal-step decrement
  kCompound    = 100,  // Compound pattern (contains sub-patterns)
};

// === Encoder ===
//
// Stateful: append values, then call finish().
// After finish(), the encoder is consumed.
class PatternEncoder {
 public:
  virtual ~PatternEncoder() = default;

  virtual absl::Status append(Variant value) = 0;
  virtual absl::StatusOr<std::string> finish() = 0;
  virtual size_t size() const = 0;
  virtual DataType value_type() const = 0;
  virtual PatternType pattern_type() const = 0;

  // Factory: create encoder for a specific pattern type + value type
  static absl::StatusOr<std::unique_ptr<PatternEncoder>> create(
      PatternType ptype, DataType value_type);
};

// === Decoder ===
//
// Immutable once constructed from encoded bytes.
class PatternDecoder {
 public:
  virtual ~PatternDecoder() = default;

  // Get the i-th value. O(1) or O(log n) depending on pattern.
  virtual absl::StatusOr<Variant> get(size_t index) const = 0;
  virtual size_t size() const = 0;
  virtual DataType value_type() const = 0;
  virtual PatternType pattern_type() const = 0;

  // Factory: create from encoded bytes (first byte = pattern type)
  static absl::StatusOr<std::unique_ptr<PatternDecoder>> create(
      std::string_view encoded);
};

}  // namespace pl::sst2
```

### 6.2 Pattern Details

**Pattern 0 — None (raw storage):**
- Format: `[type=0][length(varints)][value0(raw)][value1(raw)]...`
- Random access: O(1) — `offset = 1 + varint_size(length) + index * fixed_size`
- Use cases: Float/Double columns, checksums, fallback for incompressible data

**Pattern 1 — Stream VByte:**
- Format: `[type=1][length(varints)][min][max][control_stream][data_stream]`
- Optimization: if all values ≤ 255, control stream is omitted
- Random access: O(1) via skip table (every 128 values)
- Use cases: General-purpose integer columns (default choice)

**Pattern 2 — PFOR (Patched Frame of Reference):**
- Format: `[type=2][length][min][max][group_size][exception_count][base_bw][data_bw][exc_bw1][exc_bw2][base_stream][data_stream][exception_stream]`
- Groups of N integers (default 128), each with a base value
- Deltas from base are bit-packed; outliers stored in exception table
- Random access: O(1) to find group + scan within group + binary search exception table
- Use cases: Integer columns with small local variance

**Pattern 3 — Dictionary:**
- Format: `[type=3][length][dict_size][dict_entry0(raw)]...[bit_packed_indices]`
- Dictionary entries stored in ascending order (raw, no encoding) for fast recovery
- Bitwidth = `ceil(log2(dict_size))`
- Random access: O(1) — direct bit-offset into index array
- Use cases: Low-cardinality columns (e.g., OpType, Flag.DT, Bool columns)

**Pattern 4/5 — Equal-step Delta:**
- Format: `[type=4or5][length][subsequence_length][first_subsequence(raw)][step]`
- Pattern 4: ascending `value[i] = first[i % sl] + step * (i / sl)`
- Pattern 5: descending `value[i] = first[i % sl] - step * (i / sl)`
- Random access: O(1) — arithmetic computation
- Use cases: Auto-increment IDs, timestamps, Version sequences

**Pattern 100 — Compound:**
- Format: `[type=100][num_sub_patterns(varints)][meta_section][data_section]`
- Meta section: for each sub-pattern: `[data_type(1B)][offset(varints)]`
- Data section: concatenated independently-encoded sub-patterns
- Use cases: Version (major + minor), variable-length columns after multi-prefix compression (multiple offset+length sub-columns)

### 6.3 Pattern Selector (`pattern_selector.h`)

Automatically selects the best pattern for a sequence of values:

```cpp
namespace pl::sst2 {

// Selection strategy:
//   1. All values identical → Pattern 4 (step=0)
//   2. Arithmetic progression → Pattern 4/5
//   3. ≤256 distinct values → Pattern 3 (dictionary)
//   4. Small local variance → Pattern 2 (PFOR)
//   5. Default → Pattern 1 (Stream VByte)
//   6. Non-integral types (Float/Double) → Pattern 0
//   7. Bool type → always Pattern 3
//
// Tries candidates and picks the smallest encoded result.
absl::StatusOr<std::string> select_and_encode(
    absl::Span<const Variant> values);

}  // namespace pl::sst2
```

---

## 7. Multi-Prefix Compression Module (`compress/`)

### 7.1 Multi-Round Prefix Compression (`multi_prefix.h`)

This is the compression algorithm for the data table (spec Section 4.2).

**Algorithm outline:**
```
Input: lexicographically sorted list of strings S[0..N-1]

For each round i (0 ≤ i < max_rounds):
  1. Compute common-prefix-length array:
       cpl[j] = |LCP(S[j], S[j-1])| for j > 0, else 0
  2. Find split points: local minima in cpl, or cpl[j] == 0
  3. For each group between split points:
       prefix = LCP of all strings in group
       For each string: store (prefix_index, prefix_length, suffix)
       Suffix becomes input for next round
  4. If compression ratio > threshold OR max_rounds: stop

Output:
  - compressed data: concatenated prefix fragments + final suffixes
  - fragments per original string: list of (offset, length) pairs
```

```cpp
namespace pl::sst2 {

class MultiPrefixCompressor {
 public:
  struct Fragment {
    uint64_t offset;  // Offset within the compressed data area
    uint64_t length;  // Length of this fragment
  };

  struct Result {
    std::string compressed_data;  // The compressed data table content
    // fragments[i] = list of (offset, length) for the i-th original string
    // Always (num_rounds + 1) fragments per string
    std::vector<std::vector<Fragment>> fragments;
    size_t num_rounds;
  };

  // Compress sorted strings.
  // Parameters:
  //   max_rounds: 1-15, maximum compression rounds
  //   min_compress_ratio: stop if (new_size / old_size) > ratio (default 0.8)
  //
  // Precondition: strings are sorted lexicographically
  static absl::StatusOr<Result> compress(
      absl::Span<const std::string_view> strings,
      size_t max_rounds = 15,
      double min_compress_ratio = 0.8);
};

// Reconstruct original string from its fragments
class MultiPrefixDecompressor {
 public:
  static absl::StatusOr<std::string> reconstruct(
      absl::string_view compressed_data,
      absl::Span<const MultiPrefixCompressor::Fragment> fragments);
};

}  // namespace pl::sst2
```

### 7.2 Block Compressor (`block_compressor.h`)

Thin wrapper for block-level compression:

```cpp
namespace pl::sst2 {

enum class BlockCompressionType : uint8_t {
  kNone   = 0,
  kSnappy = 1,
  kZstd   = 2,
  kIsal   = 3,  // Reserved, NYI
};

class BlockCompressor {
 public:
  static absl::StatusOr<std::string> compress(
      std::string_view data, BlockCompressionType type);

  // original_size needed for buffer allocation
  static absl::StatusOr<std::string> uncompress(
      std::string_view compressed, size_t original_size,
      BlockCompressionType type);
};

}  // namespace pl::sst2
```

---

## 8. Block Module (`block/`)

The block module is the heart of the format. It assembles and reads individual data or index blocks.

### 8.1 Block Layout

```
┌──────────────────────┐  offset 0
│  Block Header (52B)  │  Fixed-size, magic + flags + counts + positions
├──────────────────────┤
│  Data Table          │  Variable-length strings (multi-prefix compressed)
│                      │  + embedded value data
├──────────────────────┤
│  Column Store        │  [unit_0_encoded][unit_1_encoded]...
│                      │  Each unit = one sub-column, pattern-encoded
├──────────────────────┤
│  Row Key Bitmap      │  Optional: marks rowkey changes
├──────────────────────┤
│  Offset Table        │  Varints-encoded offsets to each column store unit
└──────────────────────┘
```

### 8.2 Block Header (`block_header.h`)

```
 0-3:   magic_code (uint32 LE)
 4-11:  flags (uint64 LE)
          bits 0-3:   PK — prefix compression rounds (0-15)
          bits 4-7:   PS — pattern storage indicator
          bits 8-15:  C  — compression algorithm
          bit  16:    RK — rowkey bitmap present
          bit  17:    PC — (reserved)
          bits 18-63: reserved
12-19:  row_count (uint64 LE)
20-27:  uncompressed_size (uint64 LE)
28-35:  compressed_size (uint64 LE, 0=uncompressed)
36-43:  offset_table_position (uint64 LE)
44-51:  checksum (uint64 LE, CRC32C)
```

```cpp
namespace pl::sst2 {

class BlockHeader {
 public:
  static constexpr size_t kSize = 52;

  // Field accessors
  uint32_t magic_code() const;
  void set_magic_code(uint32_t magic);

  uint64_t flags() const;
  void set_flags(uint64_t f);

  uint64_t row_count() const;
  void set_row_count(uint64_t n);

  uint64_t uncompressed_size() const;
  void set_uncompressed_size(uint64_t sz);

  uint64_t compressed_size() const;   // 0 = no compression
  void set_compressed_size(uint64_t sz);

  uint64_t offset_table_position() const;
  void set_offset_table_position(uint64_t pos);

  uint64_t checksum() const;
  void set_checksum(uint64_t crc);

  // Flag sub-fields
  uint8_t prefix_rounds() const;
  void set_prefix_rounds(uint8_t n);

  BlockCompressionType compression_type() const;
  void set_compression_type(BlockCompressionType t);

  bool has_rowkey_bitmap() const;
  void set_has_rowkey_bitmap(bool v);

  // Validation
  bool is_data_block() const;    // magic == "DTBK"
  bool is_index_block() const;   // magic == "IXBK"
  bool is_root_index() const;    // magic == "ROOT"

  // Serialization
  std::string encode() const;
  static absl::StatusOr<BlockHeader> decode(std::string_view data);
};

}  // namespace pl::sst2
```

### 8.3 Data Table (`data_table.h`)

The data table stores variable-length data in two sections:

1. **Compressed section**: result of multi-prefix compression on all-key variable-length column strings
2. **Embedded section**: small value data that didn't exceed the length threshold

```
[compressed_string_data...][embedded_fixed_length_data(sorted)][embedded_variable_length_data(sorted+deduped)]
```

```cpp
namespace pl::sst2 {

class DataTableBuilder {
 public:
  // Set the multi-prefix compressed data
  void set_compressed_section(MultiPrefixCompressor::Result compressed);

  // Add an embedded value (fixed-size or variable-size)
  void add_embedded_value(Variant value);

  absl::StatusOr<std::string> build() const;
  size_t estimated_size() const;

 private:
  MultiPrefixCompressor::Result compressed_;
  std::vector<Variant> fixed_embedded_;     // Sorted by type, then by value
  std::vector<Variant> variable_embedded_;  // Sorted + deduped
};

class DataTableReader {
 public:
  explicit DataTableReader(std::string_view data,
                           size_t compressed_data_end);

  // Read a fragment from the compressed section
  std::string_view read_fragment(uint64_t offset, uint64_t length) const;

  // Read an embedded value at a given offset
  absl::StatusOr<Variant> read_embedded_value(uint64_t offset) const;

 private:
  std::string_view data_;
  size_t compressed_end_;
};

}  // namespace pl::sst2
```

### 8.4 Column Store (`column_store.h`)

```cpp
namespace pl::sst2 {

// Builder for the column store area.
// For each sub-column, accumulates values and pattern-encodes them on finish().
class ColumnStoreBuilder {
 public:
  // Initialize with sub-column types from the internal schema
  void initialize(absl::Span<const DataType> sub_column_types);

  // Append a value to a specific sub-column
  absl::Status append_value(size_t sub_column_index, Variant value);

  // Build the column store.
  // Returns: [unit0_encoded][unit1_encoded]...
  // Also returns the offset of each unit (for the offset table).
  struct BuildResult {
    std::string data;
    std::vector<uint64_t> unit_offsets;  // Relative to block start (offset 0)
  };
  absl::StatusOr<BuildResult> build();

  size_t num_units() const;

 private:
  struct Unit {
    DataType type;
    std::vector<Variant> values;  // Accumulated until build()
  };
  std::vector<Unit> units_;
};

// Reader for the column store. Provides random access to sub-column values.
class ColumnStoreReader {
 public:
  // Initialize from encoded column store bytes + offset table entries
  absl::Status initialize(
      std::string_view data,
      absl::Span<const uint64_t> unit_offsets,
      absl::Span<const DataType> unit_types);

  absl::StatusOr<Variant> get(size_t unit_index, size_t row_index) const;
  size_t num_units() const;
  size_t num_rows() const;

 private:
  struct Unit {
    std::unique_ptr<PatternDecoder> decoder;
  };
  std::vector<Unit> units_;
};

}  // namespace pl::sst2
```

### 8.5 Offset Table (`offset_table.h`)

Located at the end of each block. Varints-encoded uint64 offsets to each column store unit (+ optionally the rowkey bitmap start).

```cpp
namespace pl::sst2 {

class OffsetTableBuilder {
 public:
  void add_offset(uint64_t offset);
  std::string build() const;  // Varints-encoded
  size_t entry_count() const;
 private:
  std::vector<uint64_t> offsets_;
};

class OffsetTableReader {
 public:
  // Decode from bytes, advancing `data`. Verifies count matches expected.
  static absl::StatusOr<std::vector<uint64_t>> decode(
      std::string_view* data, size_t expected_count);
};

}  // namespace pl::sst2
```

### 8.6 Row Key Bitmap (`rowkey_bitmap.h`)

Bit i (1-based): 1 if row i's rowkey differs from row i-1's. Bit 0 is always 1.

```cpp
namespace pl::sst2 {

class RowKeyBitmapBuilder {
 public:
  // Call for each row; `same_rowkey` = true if rowkey unchanged from previous
  void add_row(bool same_rowkey);
  std::string build() const;
  size_t row_count() const;
};

class RowKeyBitmapReader {
 public:
  explicit RowKeyBitmapReader(std::string_view data);
  bool is_new_rowkey(size_t i) const;          // True if row i starts new rowkey
  size_t next_distinct_rowkey(size_t i) const;  // First row >= i+1 with new rowkey
  size_t row_count() const;
};

}  // namespace pl::sst2
```

### 8.7 Block Writer (`block_writer.h`)

Assembles a complete data or index block:

```cpp
namespace pl::sst2 {

struct BlockWriterOptions {
  uint32_t magic_code = constants::kDataBlockMagic;
  BlockCompressionType compression_type = BlockCompressionType::kNone;
  uint8_t max_prefix_rounds = 15;
  bool enable_rowkey_bitmap = true;

  // Size/row limits (0 = no limit) — used to determine when to flush
  uint64_t soft_size_limit = 0;
  uint64_t hard_size_limit = 0;
  uint64_t max_row_count = 0;
};

class BlockWriter {
 public:
  BlockWriter(const InternalSchema& schema, BlockWriterOptions options);

  // Append a row. Returns false if the block is full (based on limits).
  absl::StatusOr<bool> append_row(const InternalRow& row);

  // Finish and produce the complete block bytes.
  //
  // Build steps:
  //   1. For all variable-length columns:
  //      a. Collect string values → sort → multi-prefix compress
  //      b. Update sub-column decomposition for compression rounds
  //   2. For each sub-column:
  //      a. Collect values → pattern select + encode → unit bytes
  //   3. Build offset table
  //   4. Assemble: [header] [data_table] [column_store_units] [bitmap?] [offset_table]
  //   5. Optionally compress entire block
  //   6. Compute CRC32C checksum → backfill header
  absl::StatusOr<std::string> finish();

  // State
  size_t row_count() const;
  size_t estimated_size() const;
  bool is_full() const;

  // Access accumulated rows (for index block specialization)
  const std::vector<InternalRow>& rows() const { return rows_; }

 private:
  const InternalSchema& schema_;
  BlockWriterOptions options_;
  BlockHeader header_;

  std::vector<InternalRow> rows_;
  std::vector<std::vector<Variant>> sub_column_values_;  // [sub_col][row]
  std::vector<std::string_view> variable_length_strings_;  // For compression
  std::optional<std::vector<Variant>> last_rowkey_;
};

}  // namespace pl::sst2
```

### 8.8 Block Reader (`block_reader.h`)

```cpp
namespace pl::sst2 {

class BlockReader {
 public:
  // Parse a block from raw bytes.
  // Performs: CRC verification, decompression (if needed), section parsing.
  static absl::StatusOr<BlockReader> parse(std::string_view raw_block);

  // Metadata
  const BlockHeader& header() const;
  size_t row_count() const;

  // Row access
  absl::StatusOr<Variant> get_value(size_t sub_column_index,
                                    size_t row_index) const;
  absl::StatusOr<InternalRow> get_row(size_t row_index) const;

  // Rowkey bitmap
  bool has_rowkey_bitmap() const;
  bool is_new_rowkey(size_t row_index) const;
  size_t next_distinct_rowkey(size_t row_index) const;

  const ColumnStoreReader& column_store() const;

 private:
  BlockHeader header_;
  std::string uncompressed_data_;  // Owns decompressed block bytes
  DataTableReader data_table_;
  ColumnStoreReader column_store_;
  std::optional<RowKeyBitmapReader> rowkey_bitmap_;
};

}  // namespace pl::sst2
```

---

## 9. Index Tree Module (`index/`)

### 9.1 Index Block Entries

Index blocks have the same physical format as data blocks but a different logical schema.

```
Low-level index (pointing to data blocks):
  All-key  → last row's all-key in the target data block
  Flag     → DataType::kDataBlock (21)
  Filename → "@2" (current key file)
  Offset   → uint64 offset of the data block
  Length   → uint64 length of the data block
  Checksum → total row count in the data block (repurposed field)

High-level index (pointing to lower index blocks):
  Same structure, but Flag = DataType::kIndexBlock (22)
  Checksum = total row count of all data blocks in the subtree
```

### 9.2 Index Tree Builder (`index_tree_builder.h`)

This is the most architecturally complex component. It builds a multi-level, potentially unbalanced B-tree index with post-order emission.

```cpp
namespace pl::sst2 {

struct IndexEntry {
  std::vector<Variant> all_key;   // Last row's all-key in target block
  DataType entry_type;            // kDataBlock or kIndexBlock
  std::string filename;           // "@2" for current key file
  uint64_t offset;                // Block offset in key file
  uint64_t length;                // Block length
  uint64_t subtree_row_count;     // Row count in target (or subtree)
};

class IndexTreeBuilder {
 public:
  struct Options {
    uint64_t data_block_soft_limit  = 64 * 1024;
    uint64_t data_block_hard_limit  = 256 * 1024;
    uint64_t data_block_max_rows    = 0;
    uint64_t index_block_soft_limit = 64 * 1024;
    uint64_t index_block_hard_limit = 256 * 1024;
    uint64_t index_block_max_rows   = 0;
    BlockCompressionType compression_type = BlockCompressionType::kNone;
    uint8_t max_prefix_rounds = 15;
  };

  IndexTreeBuilder(const InternalSchema& schema, Options options);

  // Append a row in all-key order. Buffers into data blocks; flushes
  // blocks when they're full. Returns true if row added.
  absl::Status append_row(const InternalRow& row);

  // Finish the tree. Flushes last data block, recursively closes all
  // index levels.
  //
  // Returns blocks in post-order:
  //   [data_blocks..., index_level1..., ..., root_index]
  //
  // The caller must assign file offsets to each block.
  struct FinishedBlock {
    std::string data;
    uint64_t offset = 0;  // Assigned by caller after finish()
  };
  absl::StatusOr<std::vector<FinishedBlock>> finish();

  // Statistics
  size_t total_data_blocks() const;
  size_t total_index_blocks() const;
  size_t total_rows() const;
  size_t tree_height() const;

 private:
  // Recursive level closure
  // Level 0 = data blocks, Level 1+ = index blocks
  absl::Status flush_level(size_t level, bool is_final);

  const InternalSchema& schema_;
  Options options_;

  struct LevelState {
    std::unique_ptr<BlockWriter> data_block_writer;       // Level 0 only
    std::unique_ptr<IndexBlockWriter> index_block_writer; // Level 1+
    std::vector<IndexEntry> pending_entries;
    std::vector<FinishedBlock> emitted_blocks;
  };
  std::vector<LevelState> levels_;
  size_t total_rows_ = 0;
};

}  // namespace pl::sst2
```

**Post-order emission algorithm:**

```
append_row(row):
  level[0].block_writer.append_row(row)
  if level[0].block_writer.is_full():
    block_data = level[0].block_writer.finish()
    blocks.append(block_data)              // Emit data block now
    entry = IndexEntry{last_key, kDataBlock, "@2", offset, length, row_count}
    level[1].add_entry(entry)              // Promote to level 1
    if level[1].index_writer.is_full():
      flush_level(1)
      if level[2].is_full():
        flush_level(2)
        ...  // Recurse upward

finish():
  flush_level(0, is_final=true)   // Flush last data block
  flush_level(1, is_final=true)   // Build first-level index
  flush_level(2, is_final=true)   // Build second-level index
  ... until root                   // Root = highest level with entries
```

**Why this produces an unbalanced tree naturally:** If a data block contains a single very large row (exceeding even the hard limit), it becomes a leaf directly under the root, while other leaves may have intermediate index levels. This matches the spec's allowance (Figure 25).

### 9.3 Index Iterator (`index_iterator.h`)

Two-level iterator for range scans:

```cpp
namespace pl::sst2 {

class IndexIterator {
 public:
  // Seek to the first row with all_key >= target
  absl::Status seek(const std::vector<Variant>& target_all_key);

  // Advance to next row
  absl::Status next();

  bool valid() const;
  const InternalRow& current_row() const;

 private:
  // Outer: walks index blocks; Inner: walks rows within a data block
  // Lazily loads data blocks as needed
};

}  // namespace pl::sst2
```

---

## 10. Bloom Filter Module (`bloom/`)

### 10.1 Format (`bloom_format.h`)

```
 0-3:   magic_code (uint32 LE) = 0x4D4F4C42 ("BLOM")
 4-7:   version (uint32 LE)
 8-11:  num_hash_functions (uint32 LE)
12-19:  num_bits (uint64 LE)
20-27:  num_keys (uint64 LE)
28-35:  checksum (uint64 LE)
36..:   bit array (ceil(num_bits / 8) bytes)
```

### 10.2 Builder & Reader

```cpp
namespace pl::sst2 {

// Builds a bloom filter for a specific set of bloom filter keys.
// Each bloom filter key is a subset of all-key columns (per Configuration).
class BloomFilterBuilder {
 public:
  BloomFilterBuilder(uint64_t num_keys_estimate,
                     double bits_per_key = 10.0);

  // Add a serialized bloom filter key (subset of all-key columns)
  void add_key(std::string_view bloom_filter_key);

  // Produce the encoded bloom filter block
  std::string finish() const;

  size_t num_keys() const;
};

class BloomFilterReader {
 public:
  static absl::StatusOr<BloomFilterReader> parse(std::string_view data);

  bool key_may_match(std::string_view bloom_filter_key) const;
  size_t num_keys() const;
  uint32_t num_hash_functions() const;
};

}  // namespace pl::sst2
```

---

## 11. Metadata Module (`metadata/`)

All metadata sections share the same physical format: `[magic(4B)][checksum(8B)][Map<String, Variant>]`

### 11.1 Generic Section (`metadata_section.h`)

```cpp
namespace pl::sst2 {

class MetadataSectionWriter {
 public:
  explicit MetadataSectionWriter(uint32_t magic);

  void set(std::string key, Variant value);
  absl::StatusOr<std::string> finish() const;

 private:
  uint32_t magic_;
  std::map<std::string, Variant> fields_;  // Sorted for deterministic output
};

class MetadataSectionReader {
 public:
  static absl::StatusOr<MetadataSectionReader> parse(std::string_view data);

  uint32_t magic() const;
  absl::StatusOr<Variant> get(const std::string& key) const;
  absl::StatusOr<uint64_t> get_uint64(const std::string& key) const;
  absl::StatusOr<std::string> get_string(const std::string& key) const;
  bool has(const std::string& key) const;

 private:
  uint32_t magic_;
  std::map<std::string, Variant> fields_;
};

}  // namespace pl::sst2
```

### 11.2 Configuration (`configuration.h`)

Keys per spec Section 6.1: `MaxEmbeddedValueSizeInByte`, `MaxDataBlockSizeInByte_SoftLimit`/`_HardLimit`, `MaxDataBlockRowCount`, `MaxIndexBlockSizeInByte_SoftLimit`/`_HardLimit`, `MaxIndexBlockRowCount`, `RowCountKey0`..`RowCountKeyN`, `MinMaxKey0`..`MinMaxKeyN`, `BloomFilterKey0`..`BloomFilterKeyN`, `RowSizeKey0`..`RowSizeKeyN`.

### 11.3 Statistics (`statistics.h`)

Keys per spec Section 6.3: `RowCount`, `RowCountByKey0`.., `VersionMin`/`VersionMax`, `KeyMin`/`KeyMax`, `KeyMinByKey0`.., `KeyMaxByKey0`.., `RowSizeByKey0`.., `IndexTreeHeight`, `ValueFile0`.., `PerValueFileDataSizeInByte0`.., `DataBlockCount`, `DataBlockSizeInByte`, `IndexBlockCount`, `IndexBlockSizeInByte`, `FileCreationStartTime`/`EndTime`, `FileCreatorName`, `FileCreatorMachineName`.

### 11.4 Compatibility (`compatibility.h`)

Records `sizeof` and `std::is_signed` for all fixed-size types. Checked at load time to detect cross-platform incompatibility.

---

## 12. File Layer Module (`file/`)

### 12.1 Locator (`locator.h`)

Central directory mapping section names to (offset, length) pairs. Format: `[magic="LOCA"][checksum][Map<String, UInt64>]`.

Keys: `RootIndex_Offset`, `RootIndex_Length`, `BloomFilter0_Offset`, `BloomFilter0_Length`, ..., `Configuration_Offset`, `Configuration_Length`, `Schema_Offset`, `Schema_Length`, `Statistics_Offset`, `Statistics_Length`, `Compatibility_Offset`, `Compatibility_Length`, `UserDefinedData_Offset`, `UserDefinedData_Length`.

### 12.2 Tail (`tail.h`)

Fixed 32-byte footer at the end of every key file:

```
 0-7:   checksum (uint64 LE)         — CRC32C over bytes 8-31
 8-15:  locator_offset (uint64 LE)   — Byte offset of Locator
16-23:  locator_length (uint64 LE)   — Byte length of Locator
24-27:  version (uint32 LE)          — Always 2
28-31:  magic_code (uint32 LE)       — 0x00545353 ("SST\0")
```

```cpp
namespace pl::sst2 {

class Tail {
 public:
  static constexpr size_t kSize = 32;

  uint64_t checksum() const;
  void set_checksum(uint64_t crc);

  uint64_t locator_offset() const;
  void set_locator_offset(uint64_t offset);

  uint64_t locator_length() const;
  void set_locator_length(uint64_t length);

  uint32_t version() const;       // Always 2
  uint32_t magic_code() const;    // Always 0x00545353

  std::string encode() const;
  static absl::StatusOr<Tail> decode(std::string_view data);
};

}  // namespace pl::sst2
```

### 12.3 Value File (`value_file_writer.h`, `value_file_reader.h`)

Values are stored in insertion order (no sorting/dedup). Storage format follows spec Section 9:

- Bool: 1 byte
- Int8/UInt8: 1 byte, direct
- UInt16/32/64: varints
- Int16/32/64: zigzag + varints
- Float/Double/LongDouble: direct LE
- Time: zigzag+varints seconds + varints nanoseconds
- String/U16String/U32String/Binary: raw data only (length from internal table Length column)
- Array/Map: encoded per spec Section 2.3.3

```cpp
namespace pl::sst2 {

class ValueFileWriter {
 public:
  explicit ValueFileWriter(std::string filepath);
  absl::StatusOr<uint64_t> append(const Variant& value);  // Returns offset
  absl::Status finish();
  const std::string& filepath() const;
};

class ValueFileReader {
 public:
  explicit ValueFileReader(std::string filepath);
  absl::StatusOr<Variant> read(uint64_t offset, uint64_t length) const;
};

}  // namespace pl::sst2
```

### 12.4 SSTable Builder (`sstable_builder.h`)

The top-level builder. Orchestrates the entire SST file creation:

```cpp
namespace pl::sst2 {

class SSTableBuilder {
 public:
  struct Options {
    std::string key_file_path;
    std::string value_file_path;
    Configuration configuration;
    BlockCompressionType compression_type = BlockCompressionType::kSnappy;
  };

  SSTableBuilder(ExternalSchema schema, Options options);

  // Append a row in all-key order. `all_key_values` must be in
  // external schema order. `value` is the user's value column.
  absl::Status append_row(absl::Span<const Variant> all_key_values,
                          Variant value);

  // Finish and produce key file + value file(s).
  absl::Status finish();

  // Accessors for post-build statistics
  uint64_t total_rows() const;
  uint64_t total_data_blocks() const;
  uint64_t total_index_blocks() const;

 private:
  absl::StatusOr<InternalRow> to_internal_row(
      absl::Span<const Variant> all_key_values, Variant value);

  ExternalSchema external_schema_;
  InternalSchema internal_schema_;
  Options options_;

  std::unique_ptr<IndexTreeBuilder> index_tree_;
  std::unique_ptr<ValueFileWriter> value_file_;
  std::vector<std::unique_ptr<BloomFilterBuilder>> bloom_builders_;

  // Accumulated for statistics
  Statistics stats_;
};

}  // namespace pl::sst2
```

### 12.5 SSTable Reader (`sstable_reader.h`)

```cpp
namespace pl::sst2 {

class SSTableReader {
 public:
  // Open a key file. Reads Tail → Locator → Metadata → Bloom → Root Index.
  static absl::StatusOr<std::unique_ptr<SSTableReader>> open(
      const std::string& key_file_path);

  // Point query: look up by all-key. Returns NotFound if absent.
  absl::StatusOr<Variant> get(absl::Span<const Variant> all_key) const;

  // Range scan: create iterator positioned at start_key
  absl::StatusOr<std::unique_ptr<IndexIterator>> seek(
      absl::Span<const Variant> start_key) const;

  // Full scan: sequential read from offset 0
  std::unique_ptr<IndexIterator> begin() const;

  // Metadata
  const ExternalSchema& schema() const;
  const Configuration& configuration() const;
  const Statistics& statistics() const;

  // Bloom filter fast rejection
  bool key_may_match(size_t bloom_filter_index,
                     std::string_view bloom_filter_key) const;

 private:
  SSTableReader() = default;
  // ...internal state: schema, config, bloom readers, root block location,
  //    file handle, value file readers
};

}  // namespace pl::sst2
```

---

## 13. Data Flow

### 13.1 Build Flow

```
User: ExternalSchema + rows of (all_key, value)

SSTableBuilder::append_row():
  1. Compute internal columns:
     Flag     ← DataType of value
     Length   ← serialized value length
     If length ≤ threshold:
       Filename ← "@1", Offset ← deferred (set during block flush)
     Else:
       ValueFile.append(value) → Offset
       Filename ← value_file_path
     Checksum ← 0 (or CRC if C bit in Flag set)
  2. InternalRow{all_key + Flag + Filename + Offset + Length + Checksum}
  3. IndexTreeBuilder.append_row(internal_row)
     → Buffers into data blocks, flushes full blocks, builds index levels

SSTableBuilder::finish():
  1. IndexTreeBuilder.finish() → blocks in post-order
  2. For each bloom filter key definition:
       Build + encode BloomFilter
  3. Build metadata sections (Configuration, Schema, Statistics, Compatibility)
  4. Build Locator (with all section offsets/lengths)
  5. Build Tail
  6. Write key file:
     [block_0][block_1]...[root_index]      ← Data & Index (contiguous, no padding)
     [padding?]
     [bloom_0][bloom_1]...
     [padding?]
     [configuration][schema][statistics][compatibility]
     [padding?]
     [locator]
     [tail(32B)]
  7. fsync

Key file layout:
  ┌──────────────────┐ offset 0
  │  Data Block 0     │
  │  Data Block 1     │
  │  Index Block 1    │  ← level-1 index for blocks 0-2
  │  Data Block 3     │
  │  ...              │
  │  Root Index       │  ← post-order: always last block in data+index area
  ├──────────────────┤
  │  [padding]        │
  ├──────────────────┤
  │  Bloom Filter 0   │
  │  Bloom Filter 1   │
  ├──────────────────┤
  │  [padding]        │
  ├──────────────────┤
  │  Configuration    │
  │  Schema           │
  │  Statistics       │
  │  Compatibility    │
  ├──────────────────┤
  │  Locator          │
  ├──────────────────┤
  │  Tail (32B)       │ ← file_end - 32
  └──────────────────┘
```

### 13.2 Point Query Flow

```
SSTableReader::open(key_file):
  1. Read last 32 bytes → Tail
  2. Read Locator (from tail.locator_offset)
  3. Read all Metadata sections (from locator entries)
  4. Read Bloom Filters (from locator entries)
  5. Read Root Index block (from locator.root_index)
  → SSTableReader is ready

SSTableReader::get(all_key):
  1. For each bloom filter:
     bloom_filter_key = extract columns from all_key per configuration
     if !bloom_filter[i].key_may_match(bloom_filter_key):
       return NotFound (fast rejection)
  2. current_block = load(RootIndex)
  3. while current_block.is_index_block():
     entry = binary_search(current_block, all_key)
     current_block = load(entry.offset, entry.length)
  4. // current_block is a data block
  5. row = binary_search(current_block, all_key)
  6. if not found: return NotFound
  7. if row.filename == "@1":
       value = data_table.read_embedded_value(row.offset)
     else:
       value = value_file_reader.read(row.offset, row.length)
  8. return value
```

### 13.3 Full Scan Flow

```
SSTableReader::begin():
  1. Read key file sequentially from offset 0
  2. Parse each block header to determine type (data/index)
  3. Skip index blocks (magic == "IXBK" or "ROOT")
  4. For each data block: iterate all rows
  5. Stop at root index magic ("ROOT")
  → Produces all rows in key order without using the index
```

---

## 14. Implementation Plan

### Phase 1: Foundation (~15 files, ~3000 LOC)

| Step | Deliverables |
|------|-------------|
| 1.1 | `DataType` enum, `Variant` class — construction, comparison, serialization |
| 1.2 | `ExternalSchema`, `InternalSchema` — sub-column decomposition |
| 1.3 | `Flag`, `InternalRow`, `constants.h` |
| 1.4 | Varints + ZigZag encoding |
| 1.5 | Fixed-size encoding, Stream VByte codec |

**Tests:** Variant round-trip serialization, schema sub-column computation, varints boundary cases

### Phase 2: Pattern Storage (~14 files, ~2500 LOC)

| Step | Deliverables |
|------|-------------|
| 2.1 | Abstract `PatternEncoder`/`PatternDecoder` interfaces |
| 2.2 | Pattern 0 (None), Pattern 3 (Dictionary) |
| 2.3 | Pattern 1 (Stream VByte), Pattern 4/5 (Delta) |
| 2.4 | Pattern 2 (PFOR), Compound pattern |
| 2.5 | `PatternSelector` auto-selection |

**Tests:** Each pattern encoder/decoder round-trip, random access correctness, edge cases

### Phase 3: Compression & Block Layer (~12 files, ~3000 LOC)

| Step | Deliverables |
|------|-------------|
| 3.1 | Multi-prefix compression algorithm |
| 3.2 | Block compressor (Snappy/ZSTD wrapper) |
| 3.3 | `BlockHeader`, `DataTableBuilder`, `OffsetTable` |
| 3.4 | `ColumnStoreBuilder`, `RowKeyBitmap` |
| 3.5 | `BlockWriter` — full block assembly |
| 3.6 | `BlockReader` — full block parsing |

**Tests:** Multi-prefix compression with known inputs, block writer→reader round-trip

### Phase 4: Index & Bloom (~8 files, ~2000 LOC)

| Step | Deliverables |
|------|-------------|
| 4.1 | `IndexBlockWriter` |
| 4.2 | `IndexTreeBuilder` — multi-level tree with post-order emission |
| 4.3 | `IndexIterator` — two-level lazy iterator |
| 4.4 | `BloomFilterBuilder` + `BloomFilterReader` |

**Tests:** Index tree generation, bloom filter false positive rate

### Phase 5: File Layer & Metadata (~14 files, ~2500 LOC)

| Step | Deliverables |
|------|-------------|
| 5.1 | `MetadataSectionWriter`/`Reader` — generic section format |
| 5.2 | Configuration, Schema Meta, Statistics, Compatibility sections |
| 5.3 | `Locator`, `Tail` |
| 5.4 | `ValueFileWriter`, `ValueFileReader` |
| 5.5 | `SSTableBuilder` — end-to-end file creation |
| 5.6 | `SSTableReader` — point query, full scan, column scan |

**Tests:** End-to-end round-trip with various schemas and data sizes, KV separation

### Phase 6: Performance & Polish

- Block cache (LRU) for frequently accessed blocks
- Tune pattern selection heuristics
- File-level fuzz testing
- Benchmarks

---

## Appendices

### A. Magic Code Reference

| Name | Value | ASCII | Location |
|------|-------|-------|----------|
| Data Block | `0x4B425444` | DTBK | Block header |
| Index Block | `0x4B425849` | IXBK | Block header |
| Root Index | `0x544F4F52` | ROOT | Block header |
| Bloom Filter | `0x4D4F4C42` | BLOM | Bloom block |
| Configuration | `0x47494643` | CFIG | Metadata |
| Schema | `0x414D4553` | SEMA | Metadata |
| Statistics | `0x54415453` | STAT | Metadata |
| Compatibility | `0x504D4F43` | COMP | Metadata |
| User Defined | `0x52455355` | USER | Metadata |
| Locator | `0x41434F4C` | LOCA | Locator |
| SST File | `0x00545353` | SST\0 | Tail |

### B. Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Abseil over Folly | `Cord` for data table, `StatusOr` for errors, LTS stability, modular footprint |
| Patterns as fundamental encoding | Patterns are how column data IS stored — not optional compression applied later |
| Sub-column decomposition in schema | Reduces all column store values to arithmetic types, simplifying pattern encoding |
| Post-order index emission | Enables full scan without index; child always before parent in file |
| Immutable SST files | Lock-free readers; simpler design |
| Variant owns string data | Avoids lifetime issues from transient source buffers |
| Pattern selection at flush time | All values visible → optimal pattern choice |
| Block header at front, offset table at end | Full scan needs block length first; offset table has variable varints size |
| Fixed block header size | Enables backfill without reserved-space guesswork |

### C. C++20 Feature Usage

| Feature | Usage |
|---------|-------|
| `std::span<T>` | Zero-copy buffer views |
| `std::bit_cast` | Safe type-punning |
| `std::endian` | Endianness detection |
| Concepts (`requires`) | Template constraints on encoding functions |
| Designated initializers | Config structs |
