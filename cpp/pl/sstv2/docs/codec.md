# Memcomparable 编码（Codec）

SSTable v2 使用双层编码系统：**标量 comparable** 编码处理原始类型，**值 comparable** 编码
根据 `DataType` 分发到标量编码，处理 `Value` 对象。

## 核心原理

编码后字节的位序（`memcmp`）与原始值的逻辑顺序完全一致。这意味着：

- 索引块中的编码键可直接用 `memcmp` 二分查找
- 布隆过滤器可直接哈希编码后的字节（无需每类型单独实现哈希）
- 编码本身就是序列化格式 — 没有单独的 wire format

## 整数编码

### 无符号整数（uint8–uint64）

大端序编码。无需变换，因为大端字节本来就是正确排序的。

```
value = 0x000000000000002A（uint64 类型的 42）

encoded = [00 00 00 00 00 00 00 2A]  （8 字节，大端序）
```

### 有符号整数（int8–int64）

翻转符号位后按大端序编码。将负数映射到正数下方，同时保持各自组内有序。

```
value = -42（int64 类型）
符号翻转后 = 0x7FFFFFFFFFFFFFD6
encoded = [7F FF FF FF FF FF FF D6]

value = 0（int64 类型）
符号翻转后 = 0x8000000000000000
encoded = [80 00 00 00 00 00 00 00]

value = 42（int64 类型）
符号翻转后 = 0x800000000000002A
encoded = [80 00 00 00 00 00 00 2A]
```

符号翻转后值的自然顺序：`-inf ... -1 < 0 < 1 ... +inf`

### 降序排列

对升序编码的所有位取反：

```cpp
void invert_buf(uint8_t* buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        buf[i] = static_cast<uint8_t>(~buf[i]);
    }
}
```

这会反转字节序，同时保持 memcomparable 特性。

## 浮点数编码

IEEE 754 double 通过操作符号位和指数位来编码：

```
正数（符号位 = 0）：
    符号位翻转为 1
    → 正数排在负数之后，且正数内部排序正确

负数（符号位 = 1）：
    所有位取反
    → 负数内部排序正确（越负 → 编码越小）

特殊情况：
    +0    → 0x8000000000000000
    -0    → 0x7FFFFFFFFFFFFFFF
    +inf  → 0xFFF0000000000000
    -inf  → 0x000FFFFFFFFFFFFF
    NaN   → 0x0008000000000000（规范形式）
```

```
IEEE 754 位序：  -inf < -1.0 < -0.0 < +0.0 < +1.0 < +inf
编码后字节序：   -inf < -1.0 < -0.0 < +0.0 < +1.0 < +inf  ✅
```

## 字符串编码

通过对两个特殊字节做转义使字符串变得 memcomparable：

```
输入字节     →  编码
0x00         →  0x00 0xFF
0xFF         →  0xFF 0x00
其他所有字节  →  原样

终止符：0x00 0x01
```

这保证了：
- `0x00` 不会以未转义形式出现在编码输出中（与 C 字符串兼容）
- `0xFF` 被转义，避免与 0x00 的转义序列混淆
- 终止符 `0x00 0x01` 按字典序大于任何合法字符串的延续

```
"foo"       → 66 6F 6F 00 01
"foo\0bar"  → 66 6F 6F 00 FF 62 61 72 00 01
"foobar"    → 66 6F 6F 62 61 72 00 01

memcmp("foo", "foo\0bar") < 0  ✅
memcmp("foo", "foobar")   < 0  ✅
```

## 复合类型编码

### Time（时间）

```
Time = {seconds: int64, nanoseconds: int32}
编码: encode_int64_desc(seconds) + encode_int32_desc(nanoseconds)
     （两者均用降序 — 更新的时间戳排在前面）
```

### Version（版本）

```
Version = {major: uint64, minor: uint64}
编码: encode_uint64(major) + encode_uint64(minor)
```

### Array（数组）

```
编码: 对每个元素依次 encode_value(element, order)
终止符: encode_value(kNone)  — 0x00（NONE 类型标记）
```

### Map

```
编码: 对每个排序后的条目依次 encode_value(key, order) + encode_value(value, order)
终止符: encode_value(kNone)
```

Map 在编码前按键排序以确保确定性输出。

### Nullable（可空值）

```
编码:
  if has_value:  encode_uint8(1) + encode_value(value, order)
  if null:       encode_uint8(0)
```

确保升序下 `NULL < 任意非空值`。

## Value Comparable API

```cpp
// 顶层：以指定排序编码 Value
void encode_value(const Value& value, SortOrder order, std::string* dst);

// 从编码字节解码 Value
size_t decode_value(const uint8_t* src, size_t len, Value* value, SortOrder order = kAscending);

// 将 InternalRow 编码为 memcomparable 的 all-key
absl::Status encode_all_key(const InternalRow& row,
                              const InternalSchema::ConstRef& schema,
                              std::string* dst);

// 编码指定列数的前缀键
absl::Status encode_key_prefix(const InternalRow& row,
                                const InternalSchema::ConstRef& schema,
                                size_t prefix_column_count,
                                std::string* dst);
```

## 定长与变长辅助函数

```cpp
// 定长 (codec/fixed.h)
uint32_t read_fixed32(const uint8_t* src);
uint64_t read_fixed64(const uint8_t* src);
void write_fixed32(std::string* dst, uint32_t v);
void write_fixed64(std::string* dst, uint64_t v);

// 变长整数 (codec/varint.h)
void encode_varint(uint64_t v, std::string* dst);
std::pair<uint64_t, size_t> decode_varint(std::string_view src);
```

## 性能特征

| 操作 | 复杂度 | 备注 |
|------|--------|------|
| `encode_value(int)` | O(1) | 1–8 字节，无内存分配 |
| `encode_value(string)` | O(n) | 单次扫描 + 转义，无中间缓冲区 |
| `encode_all_key(row)` | O(k · v) | k = 排序列数，v = 平均值大小 |
| `compare_encoded_bytes` | O(min(m, n)) | 原始 `memcmp`，无需 schema |
| `decode_value` | O(n) | 单次扫描，编码的逆操作 |

编码设计尽量减少内存分配：调用者提供目标缓冲区，多数标量编码可直接填入栈上定长空间。
