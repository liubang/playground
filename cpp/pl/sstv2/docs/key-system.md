# 键类型系统

SSTable v2 使用模板化、标签分派的键类型系统，在编译期确保键比较的安全性。
三种键作用域 × 三种表达形式 = 九种键类型，由 C++20 concept 统一约束。

## 键作用域（Tag）

定义于 `key_tags.h`：

```cpp
struct RowKeyTag {};     // 仅用户定义的 row key 列
struct AllKeyTag {};     // 全排序键（row key + version + op_type）
struct PrefixKeyTag {};  // 用于范围扫描边界的部分键
```

| Tag            | 作用域   | 列组成                                  | 使用场景             |
| -------------- | -------- | --------------------------------------- | -------------------- |
| `RowKeyTag`    | 用户行键 | `[col₀, ..., colₖ₋₁]`                   | 行级标识             |
| `AllKeyTag`    | 全排序键 | `[col₀, ..., colₖ₋₁, version, op_type]` | 全序排列、布隆过滤器 |
| `PrefixKeyTag` | 范围边界 | `[col₀, ..., colₘ]` 其中 m ≤ k          | 扫描起止边界         |

## 三种表达形式

每种作用域有三种表达形式，在所有权与灵活性之间权衡：

### `LogicalKey<Tag>` — 持有所有权

```cpp
template <typename Tag>
class LogicalKey {
public:
    static LogicalKey from_columns(std::vector<Value> columns);
    size_t column_count() const noexcept;
    const Value& column(size_t index) const;
    const std::vector<Value>& columns() const noexcept;
};
```

- 通过 `std::vector<Value>` **持有**列数据
- 适用于需要长期持有键的场景（如 builder 中的 last_key）

### `KeyView<Tag>` — 零拷贝借用

```cpp
template <typename Tag>
class KeyView {
public:
    static KeyView from_columns(const Value* columns, size_t column_count);
    size_t column_count() const noexcept;
    const Value& column(size_t index) const;
};
```

- **借用**指向已有 `Value` 存储的指针
- 调用者必须保证源数据生命周期
- 适用于不应发生内存分配的临时比较

### `EncodedKey<Tag>` — 预计算的可比较字节

```cpp
template <typename Tag>
class EncodedKey {
public:
    static EncodedKey from_encoded_bytes(std::string bytes);
    std::string_view bytes() const noexcept;
};
```

- **持有**一段 memcomparable 编码后的字节串
- 用于布隆过滤器键和索引块二分查找
- 对 `.bytes()` 做 `memcmp` 即得到正确排序

## 类型别名

九种具体键类型：

```cpp
using RowKey    = LogicalKey<RowKeyTag>;
using AllKey    = LogicalKey<AllKeyTag>;
using PrefixKey = LogicalKey<PrefixKeyTag>;

using RowKeyView    = KeyView<RowKeyTag>;
using AllKeyView    = KeyView<AllKeyTag>;
using PrefixKeyView = KeyView<PrefixKeyTag>;

using EncodedRowKey    = EncodedKey<RowKeyTag>;
using EncodedAllKey    = EncodedKey<AllKeyTag>;
using EncodedPrefixKey = EncodedKey<PrefixKeyTag>;
```

## C++20 Concept

`KeyComparator` 使用 concept 防止无效比较：

```cpp
template <typename Key>
concept KeyLike = requires(const Key& key, size_t index) {
    typename Key::tag;
    { key.column_count() } -> std::convertible_to<size_t>;
    { key.column(index) } -> std::same_as<const Value&>;
};

template <typename Key, typename Tag>
concept KeyWithTag = KeyLike<Key> && std::same_as<typename Key::tag, Tag>;
```

这意味着编译器**拒绝**无效比较：

```cpp
KeyComparator cmp(schema);
cmp.compare_all_key(all_key, row_key);        // ✅ 编译通过（允许 AllKeyTag × RowKeyTag）
cmp.compare_all_key(all_key, all_key);        // ✅ 编译通过
cmp.compare_all_key(all_key, prefix_key);     // ✅ 编译通过
cmp.compare_all_key(row_key, row_key);        // ❌ 编译错误：RowKeyTag ≠ AllKeyTag
```

## KeyComparator API

```cpp
class KeyComparator {
public:
    explicit KeyComparator(InternalSchema::ConstRef schema);

    // 以精确边界语义比较任意两个 KeyLike 值
    template <KeyLike Lhs, KeyLike Rhs>
    StatusOr<int> compare_exact(const Lhs& lhs, const Rhs& rhs) const;

    // AllKey vs AllKey — 完整精确比较
    template <KeyWithTag<Lhs, AllKeyTag>, KeyWithTag<Rhs, AllKeyTag>>
    StatusOr<int> compare_all_key(const Lhs& lhs, const Rhs& rhs) const;

    // AllKey vs PrefixKey — 前缀匹配（较短的键是较长键的前缀）
    template <KeyWithTag<Lhs, AllKeyTag>, KeyWithTag<Rhs, PrefixKeyTag>>
    StatusOr<int> compare_all_key_to_prefix(const Lhs& lhs, const Rhs& rhs) const;

    // 便捷谓词
    StatusOr<bool> all_key_less(const auto& lhs, const auto& rhs) const;
    StatusOr<bool> all_key_less_than_prefix(const auto& lhs, const auto& rhs) const;
    StatusOr<bool> prefix_less(const auto& lhs, const auto& rhs) const;
};
```

### 比较语义

```
compare_all_key(A, B):
    for each column i in [0, min(A.columns, B.columns)):
        cmp = compare_values(A[i], B[i])
        if column i is DESC: cmp = -cmp
        if cmp ≠ 0: return cmp
    return sign(A.columns - B.columns)   // 列数少的一方更小

compare_all_key_to_prefix(A, P):
    for each column i in [0, min(A.columns, P.columns)):
        cmp = compare_values(A[i], P[i])
        if column i is DESC: cmp = -cmp
        if cmp ≠ 0: return cmp
    return 0  // 前缀匹配 — A 和 P 在 P 的长度内相等
```

## 范围扫描用的 KeyPrefix

```cpp
struct KeyPrefix {
    std::vector<Value> key_columns;      // row key 列的前缀 (0..M)
    std::optional<Version> version;       // 需要完整的 row key 前缀
    std::optional<OpType> op_type;        // 需要 version 存在
};
```

约束条件：

- `key_columns.size() ≤ schema.row_key_column_count()`
- `version` 需要 `key_columns.size() == schema.row_key_column_count()`
- `op_type` 需要 `version.has_value()`

### 范围扫描示例

```
Schema: (tenant ASC, user_id ASC) → row_key_columns = 2
AllKey:  (tenant, user_id, version, op_type, flags, filename, offset, length, checksum)

Scan: start = KeyPrefix{.key_columns = {"acme"}}
      limit = KeyPrefix{.key_columns = {"acme", 9999}}

效果: 返回所有 tenant = "acme" 且 user_id ∈ [0, 9999] 的行
```

## 工厂函数 (key_factory.h)

```cpp
// 从 InternalRow 构造 AllKey — 对照 schema 校验列类型
StatusOr<AllKey> make_all_key(const InternalRow& row, const InternalSchema::ConstRef& schema);

// 构造 AllKeyView — 从 InternalRow 列零拷贝借用
StatusOr<AllKeyView> make_all_key_view(const InternalRow& row, const InternalSchema::ConstRef& schema);

// 构造 PrefixKey — 对照用户 schema 校验前缀形状
StatusOr<PrefixKey> make_prefix_key(const KeyPrefix& prefix,
                                     const Schema::ConstRef& user_schema,
                                     const InternalSchema::ConstRef& internal_schema);
```

## 编码键的比较

编码后的键无需 schema 即可进行原始字节比较：

```cpp
template <typename LhsTag, typename RhsTag>
inline int compare_encoded_bytes(const EncodedKey<LhsTag>& lhs,
                                  const EncodedKey<RhsTag>& rhs) noexcept {
    const int cmp = lhs.bytes().compare(rhs.bytes());
    return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}
```
