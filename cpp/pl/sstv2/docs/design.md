# SSTableV2 详细设计文档

---

## 1. 概述

SSTableV2 是一种不可变的有序键值存储文件格式，采用列存储（column-store）设计。一个逻辑
SSTable 由两个物理文件组成：

- **Key File**：存储行的结构化键、元数据、索引和内嵌值。
- **Value File**：存储超过阈值的大值（separated value）。

Key File 是自描述的：读取时从文件末尾的固定 32 字节 Tail 开始，逐步定位 Locator、
Metadata、Index 和 Data Block。

---

## 2. Key File 整体布局

```
+-------------------------------------+  offset 0
|         Data Block 0                |
+-------------------------------------+
|         Data Block 1                |
+-------------------------------------+
|         ...                         |
+-------------------------------------+
|         Data Block N-1              |
+-------------------------------------+
|    Index Blocks (post-order)        |  <-- 与 Data Block 交错排列
+-------------------------------------+
|         Root Index Block            |
+-------------------------------------+
|    Bloom Filter Section(s)          |  <-- 可选
+-------------------------------------+
|    Metadata: CFIG                   |
+-------------------------------------+
|    Metadata: SEMA                   |
+-------------------------------------+
|    Metadata: STAT                   |
+-------------------------------------+
|    Metadata: COMP                   |  <-- 可选
+-------------------------------------+
|    Metadata: USER                   |  <-- 可选
+-------------------------------------+
|         Locator                     |
+-------------------------------------+
|         Tail (32 bytes)             |  <-- 文件末尾
+-------------------------------------+
```

说明：

- Data Block 和 Index Block 按照后序遍历（post-order traversal）交错排列。对于单层索引
  （只有 Root Index），所有 Data Block 在前，Root Index Block 紧随其后。
- Bloom Filter、Metadata sections 的顺序不做强制要求，读取时通过 Locator 定位。
- 各 section 之间允许存在 padding。

---

## 3. Tail（尾部，32 字节）

Tail 是 Key File 的最后 32 字节，是读取的入口点。

### 3.1 二进制布局

```
Offset  Size   Field            Description
------  ----   -----            -----------
0       8      checksum         CRC32C of the entire 32 bytes (checksum field zeroed)
8       8      locator_offset   Byte offset of Locator in the key file
16      8      locator_length   Byte length of Locator
24      4      version          Format version, must be 2
28      4      magic            0x00545353 ("SST\0", little-endian)
```

所有多字节字段均为 **little-endian**。

### 3.2 Checksum 计算

1. 将 32 字节 tail 的 checksum 字段（offset 0-7）置零。
2. 对整个 32 字节计算 CRC32C。
3. 将 CRC32C 结果（uint32）零扩展为 uint64，写入 checksum 字段。

### 3.3 读取流程

1. 读取文件最后 32 字节。
2. 验证 magic == 0x00545353。
3. 验证 version == 2。
4. 验证 checksum。
5. 使用 locator_offset 和 locator_length 读取 Locator。

---

## 4. Locator（定位器）

Locator 采用 Metadata Section 的通用格式（见第 6 节），magic 为 `LOCA`。其 payload 是
一个 `Map<String, Variant>` 结构，记录各 section 在 key file 中的位置。

### 4.1 二进制布局

```
Offset  Size     Field       Description
------  ----     -----       -----------
0       4        magic       0x41434F4C ("LOCA", little-endian)
4       8        checksum    CRC32C of entire section (checksum field zeroed), zero-extended to uint64
12      varint   entry_count Number of map entries
12+     ...      entries     Repeated: key_len(varint) + key(bytes) + type_tag(1B) + value(fixed)
```

### 4.2 Map Entry 格式

每个 entry：

```
varint   key_length
bytes    key_data        (UTF-8 string)
uint8    value_type      (DataType enum, 通常为 kUint64=9)
fixed    value_data      (根据 value_type 确定长度，uint64 为 8 字节 LE)
```

### 4.3 必需的 Locator Key

| Key                        | 含义                          |
|----------------------------|-------------------------------|
| `RootIndex_Offset`         | Root Index Block 的文件偏移   |
| `RootIndex_Length`         | Root Index Block 的字节长度   |
| `Configuration_Offset`     | CFIG section 的文件偏移       |
| `Configuration_Length`     | CFIG section 的字节长度       |
| `Schema_Offset`            | SEMA section 的文件偏移       |
| `Schema_Length`            | SEMA section 的字节长度       |
| `Statistics_Offset`        | STAT section 的文件偏移       |
| `Statistics_Length`        | STAT section 的字节长度       |

### 4.4 可选的 Locator Key

| Key                        | 含义                          |
|----------------------------|-------------------------------|
| `Compatibility_Offset`     | COMP section 的文件偏移       |
| `Compatibility_Length`     | COMP section 的字节长度       |
| `BloomFilter0_Offset`      | 第 0 个 Bloom Filter 的偏移   |
| `BloomFilter0_Length`      | 第 0 个 Bloom Filter 的长度   |
| `BloomFilterN_Offset`      | 第 N 个 Bloom Filter 的偏移   |
| `BloomFilterN_Length`      | 第 N 个 Bloom Filter 的长度   |
| `UserDefinedData_Offset`   | USER section 的文件偏移       |
| `UserDefinedData_Length`   | USER section 的字节长度       |

### 4.5 Checksum 计算

与 Tail 类似：将 checksum 字段（offset 4-11）置零后，对整个 section 字节计算 CRC32C，
零扩展为 uint64。

---

## 5. Block 通用格式

Data Block、Index Block 和 Root Index Block 共享相同的 52 字节 header 和 body 结构。

### 5.1 Block Header（52 字节）

```
Offset  Size   Field                Description
------  ----   -----                -----------
0       4      magic                Block 类型标识
4       8      flags                标志位（见 5.2）
12      8      row_count            Block 中的行数
20      8      offset_table_offset  Offset Table 相对于 block 起始的字节偏移
28      8      uncompressed_size    未压缩时整个 block 的字节数（含 header）
36      8      compressed_size      压缩后的字节数（0 表示未压缩）
44      8      checksum             CRC32C of entire block (checksum field zeroed)
```

### 5.2 Magic 值

| Magic 值     | ASCII  | 含义             |
|-------------|--------|------------------|
| 0x4B425444  | "DTBK" | Data Block       |
| 0x4B425849  | "IXBK" | Index Block      |
| 0x544F4F52  | "ROOT" | Root Index Block |

### 5.3 Flags 位定义（64 位无符号整数）

```
Bit    Name   Width   Description
---    ----   -----   -----------
0      PS     1 bit   Pattern Store: 1 = 使用 pattern encoding
1      PK     1 bit   RowKey bitmap: 1 = block 包含重复 rowkey bitmap
2-9    C      8 bits  压缩算法编号: 0 = 无压缩, 1-255 = 压缩算法 ID
10-13  PC     4 bits  前缀压缩轮数: 0 = 不使用, 1-15 = 前缀压缩轮数
14-63  -      50 bits 保留，必须为 0
```

注意：块头永远不参与压缩，reader 需要先读取 C 位确定解压算法。如果 C=0（无压缩），
header 中 compressed_size 必须为 0。

### 5.4 Checksum 计算

1. 将 block 的 checksum 字段（offset 44-51）置零。
2. 对整个 block（header + body）计算 CRC32C。
3. CRC32C 结果零扩展为 uint64，写入 checksum 字段。

### 5.5 Block Body 结构

```
+-------------------------------------+  offset = 52 (紧接 header)
|         Data Table                  |  变长数据存储区
+-------------------------------------+
|    Column-Store Unit 0              |
+-------------------------------------+
|    Column-Store Unit 1              |
+-------------------------------------+
|         ...                         |
+-------------------------------------+
|    Column-Store Unit K-1            |
+-------------------------------------+
|    RowKey Bitmap (optional)         |  仅当 flags.PK=1
+-------------------------------------+
|         Offset Table                |  <-- header.offset_table_offset 指向此处
+-------------------------------------+
```

### 5.6 Offset Table

Offset Table 是一个 varint 编码的序列，每个值是对应 Column-Store Unit（或 RowKey
Bitmap）相对于 block 起始的绝对字节偏移。项数等于内部表格的列数（M+7），如果
flags.PK=1 则再多一项（bitmap 的偏移）。

```
varint  unit_0_offset           (RowKey[0] 列)
varint  unit_1_offset           (RowKey[1] 列)
...
varint  unit_M+6_offset         (Checksum 列)
varint  rowkey_bitmap_offset    (仅当 flags.PK=1)
```

Reader 通过相邻 offset 的差值推算每个 unit 的长度（最后一个 unit 的长度 =
offset_table_offset - last_unit_offset）。

---

## 6. Metadata Section 通用格式

CFIG、SEMA、STAT、COMP、USER 以及 Locator 都使用相同的 section 格式：

```
Offset  Size     Field        Description
------  ----     -----        -----------
0       4        magic        Section 类型标识
4       8        checksum     CRC32C (checksum field zeroed), zero-extended to uint64
12      varint   entry_count  Map entry 数量
12+     ...      entries      Map entries (同 Locator 格式)
```

### 6.1 Section Magic 值

| Magic 值     | ASCII  | 含义                |
|-------------|--------|---------------------|
| 0x41434F4C  | "LOCA" | Locator             |
| 0x47494643  | "CFIG" | Configuration       |
| 0x414D4553  | "SEMA" | Schema              |
| 0x54415453  | "STAT" | Statistics          |
| 0x504D4F43  | "COMP" | Compatibility       |
| 0x52455355  | "USER" | User-Defined Data   |

### 6.2 Configuration (CFIG) 内容

| Key                              | Type   | Description                    |
|----------------------------------|--------|--------------------------------|
| `MaxEmbeddedValueSizeInByte`     | uint64 | 内嵌值大小阈值                 |
| `MaxDataBlockSizeInByte_SoftLimit` | uint64 | Data Block 软上限            |
| `MaxDataBlockSizeInByte_HardLimit` | uint64 | Data Block 硬上限            |
| `MaxDataBlockRowCount`           | uint64 | Data Block 最大行数           |

### 6.3 Schema (SEMA) 内容

| Key                              | Type   | Description                    |
|----------------------------------|--------|--------------------------------|
| `RowKeyColumnCount`              | uint64 | Row Key 列数                   |
| `RowKeyColumnN_Type`             | uint64 | 第 N 列的 DataType 枚举值     |
| `RowKeyColumnN_Order`            | uint64 | 第 N 列的排序方向 (0=ASC,1=DESC) |

### 6.4 Statistics (STAT) 内容

| Key                              | Type   | Description                    |
|----------------------------------|--------|--------------------------------|
| `TotalRowCount`                  | uint64 | 总行数                         |
| `DataBlockCount`                 | uint64 | Data Block 数量                |
| `IndexBlockCount`                | uint64 | Index Block 数量（含 root）    |
| `KeyFileSize`                    | uint64 | Key File 总字节数              |
| `ValueFileSize`                  | uint64 | Value File 总字节数            |

### 6.5 Compatibility (COMP) 内容

用于跨平台兼容性验证，可选。

### 6.6 User-Defined Data (USER) 内容

用户自定义的键值对，可选。

---

## 7. Internal Table（内部表结构）

每个 Data Block 存储的逻辑行具有固定的内部表结构：

```
Column Index   Column Name    Type          Description
------------   -----------    ----          -----------
0..M-1         RowKey[0..M-1] user-defined  用户定义的 row key 列
M              Version        uint64        版本号（降序排列）
M+1            OpType         uint8         操作类型
M+2            Flag           uint64        标志位（见 7.1）
M+3            Filename       string        值文件名
M+4            Offset         uint64        值在文件中的偏移
M+5            Length         uint64        值的字节长度
M+6            Checksum       uint64        值的 CRC32C (zero-extended to uint64)
```

### 7.1 Flag 位定义（64 位无符号整数，低 10 bits 有效）

```
Bit    Name   Description
---    ----   -----------
0-7    DT     DataType 枚举值（值的数据类型，最多 256 种）
8      C      Checksum bit: 1 = 计算校验和, 0 = 不计算（Checksum 列可为任意值）
9      B      Bool bit: 若 DT=Bool(1)，该位存储布尔值（1=TRUE, 0=FALSE）；
              若 DT≠Bool，该位无含义，必须为 0
10-63  -      保留，必须为 0
```

Flag 是 64 位无符号整数，但只定义了低 10 位。在索引块中，Flag 使用私有类型值：
21 = DataBlock（指向数据块），22 = IndexBlock（指向索引块），此时 C 位和 B 位必须为 0。

### 7.2 Filename 语义

| Filename | 含义                                           |
|----------|------------------------------------------------|
| `@1`     | 值内嵌在当前 block 的 data table 中            |
| `@2`     | 值存储在当前 key file 的其他位置               |
| 其他路径 | 值存储在外部 value file 中                     |

### 7.3 行排序规则

行按照以下复合键排序：

1. RowKey 各列按 Schema 定义的顺序和方向排序（默认 ASC）。
2. Version 降序（新版本在前）。
3. OpType 升序。

排序使用 memcomparable encoding，确保字节序比较等价于逻辑比较。

---

## 8. Column-Store Unit

每个内部表列对应一个 Column-Store Unit。Unit 的格式取决于 pattern encoding。

### 8.1 Pattern 0（无编码，原始存储）

适用于定长整数类型的子列。每个成员直接存储，不采用编码，占用固定大小。

```
uint8   pattern_id = 0
varint  row_count
cell*   cells       (每个 cell 占用 DataType 对应的固定字节数，直接存储)
```

Pattern 0 支持 O(1) 随机定位（offset = cell_index * fixed_size）。

注意：变长列和复合类型列不直接使用 pattern 0，而是通过 compound pattern（8.7 节）
将 offset/length 拆分为子列后，子列再使用 pattern 0-5。

### 8.2 Pattern 1（Stream VByte）

对定长整数列使用 Stream VByte 编码，提高压缩率。

```
uint8   pattern_id = 1
varint  row_count
bytes   stream_vbyte_encoded_data
```

### 8.3 Pattern 2（PFOR - Patched Frame of Reference）

```
uint8   pattern_id = 2
varint  row_count
bytes   pfor_encoded_data
```

### 8.4 Pattern 3（Dictionary Encoding）

```
uint8   pattern_id = 3
varint  row_count
varint  dict_size
bytes   dictionary_entries
bytes   index_array
```

### 8.5 Pattern 4（Constant Stride Increment）

适用于等差递增序列。

```
uint8   pattern_id = 4
varint  row_count
varint  base_value
varint  stride
```

### 8.6 Pattern 5（Constant Stride Decrement）

适用于等差递减序列。

```
uint8   pattern_id = 5
varint  row_count
varint  base_value
varint  stride
```

### 8.7 Pattern 100（Compound）

以下类型的列使用 compound pattern，将一个逻辑列拆分为多个子列：

- **变长类型**（String, Binary 等）：拆分为 offset 子列（UInt64）+ length 子列（UInt64）。
- **Version 类型**：拆分为 major 子列（UInt64）+ minor 子列（UInt64）。
- **Time 类型**：拆分为 seconds 子列（Int64）+ nanoseconds 子列（UInt32）。
- **复合类型**（Array, Map）：拆分为各子元素的子列。

Key 文件中列存储区的 unit 与 schema 中的 column 一一对应（每列一个 unit）。

```
uint8   pattern_id = 100
varint  sub_column_count
unit*   sub_units          (每个 sub_unit 可以是 pattern 0-5 中的任一种)
```

Compound pattern 的 meta 部分记录每个子列的 DataType 和起始偏移。

### 8.8 Pattern 选择策略

写入时对每列的数据进行分析，选择压缩率最优的 pattern。如果所有 pattern 都不能
有效压缩，则 fallback 到 pattern 0。

---

## 9. Data Table（数据表）

Data Table 是 block body 中的第一个区域，存储变长数据的原始字节。Column-Store Unit
中的变长列通过 (offset, length) 引用 Data Table 中的数据。

### 9.1 Data Table 内部布局

Data Table 由两部分组成，中间不允许有填充字节：

```
+-----------------------------------+  offset = 52 (紧接 header)
|  多轮前缀压缩数据                 |  变长/复合类型列的数据
+-----------------------------------+
|  内嵌数据 (Embedded Data)         |  Filename="@1" 的 value 数据
+-----------------------------------+
```

### 9.2 多轮前缀压缩

当 flags.PC > 0 时，Data Table 的前缀压缩数据部分使用多轮前缀压缩（轮数 = flags.PC）：

1. 将所有变长/复合类型列的数据按行顺序拼接。
2. 对相邻数据计算公共前缀长度。
3. 存储格式：`shared_prefix_len(varint) + unshared_data(bytes)`。
4. 递归应用，最多 15 轮（由 flags.PC 指定）。

当 flags.PC=0 时，该部分是原始字节的简单拼接。

### 9.3 内嵌数据（Embedded Data）

当 value 大小 <= MaxEmbeddedValueSize 时，value 存储在 Data Table 的内嵌数据区域
（Filename="@1"，Offset 为相对于块头的偏移）。

内嵌数据的排列规则：

1. **定长类型优先**：先存储定长类型（整数、浮点等）的内嵌数据，再存储变长/复合类型。
2. **定长类型内部**：按 DataType 分组，组间按 type 编号排序，组内按值升序排序。
   定长类型的内嵌数据使用 pattern 存储（参见第 8 节），不进行 zigzag/varints 编码。
3. **变长/复合类型内部**：按 DataType 分组，组间按 type 编号排序，组内按 memcmp 结果
   升序排序并去重。

注意：内嵌数据不参与前缀压缩，它位于前缀压缩数据之后。

---

## 10. RowKey Bitmap

当 flags.PK=1 时，block 包含一个 RowKey Bitmap，用于标记哪些行与前一行具有相同的
row key（仅 Version/OpType 不同）。

```
varint  num_bits
bytes   bitmap_data    (ceil(num_bits/8) 字节，bit 0 = 第一行)
```

Bit 为 1 表示该行的 row key 与前一行相同。第一行的 bit 始终为 0。

---

## 11. Index Tree（索引树）

### 11.1 结构

索引树是一棵 B-tree，叶子节点指向 Data Block，内部节点指向子 Index Block。树可以
是不平衡的。

- **Root Index Block**：magic = `ROOT`，是索引树的根。
- **Index Block**：magic = `IXBK`，是中间层索引节点。
- **Data Block**：magic = `DTBK`，是叶子层数据。

### 11.2 Index Block 内部表结构

Index Block 的内部表结构与 Data Block **完全相同**（第 7 节），即：

```
Column Index   Column Name    Type          Description
------------   -----------    ----          -----------
0..M-1         RowKey[0..M-1] user-defined  子节点中最后一行的 all key 各列
M              Version        uint64        子节点中最后一行的 Version
M+1            OpType         uint8         子节点中最后一行的 OpType
M+2            Flag           uint64        私有类型: 21=DataBlock, 22=IndexBlock
M+3            Filename       string        "@2" = 当前 key file
M+4            Offset         uint64        子节点在 key file 中的字节偏移
M+5            Length         uint64        子节点的字节长度
M+6            Checksum       uint64        子树中所有数据块包含的总行数
```

关键语义差异：

- **RowKey + Version + OpType**：存储的是子节点中最后一行的完整 all key（用于二分查找）。
- **Flag**：不使用 DT/C/B 位定义，而是使用私有类型值 21（指向 DataBlock）或 22（指向 IndexBlock）。
- **Checksum**：不是校验和，而是子树总行数。对于 Flag=21（指向 DataBlock），值为该数据块的行数；
  对于 Flag=22（指向 IndexBlock），值为该索引块子树中所有数据块行数之和。
- 索引块中 Flag 的 C 位和 B 位必须为 0。

### 11.3 后序遍历布局

Index Block 和 Data Block 在文件中按后序遍历顺序排列：

```
对于一棵 3 层树（root -> index -> data）：
  DataBlock_0, DataBlock_1, IndexBlock_0,
  DataBlock_2, DataBlock_3, IndexBlock_1,
  RootIndexBlock
```

这种布局使得写入时可以流式生成：每当一个 Index Block 满了，就 flush 它，然后继续
写入下一批 Data Block。

### 11.4 查找流程

1. 从 Locator 获取 RootIndex_Offset/Length。
2. 读取 Root Index Block。
3. 在 Root Index Block 中二分查找目标 key（比较 all key 各列），找到对应的 entry。
4. 如果 entry 的 Flag=22（IndexBlock），读取子 Index Block，递归步骤 3。
5. 如果 entry 的 Flag=21（DataBlock），读取该 Data Block，在其中查找目标行。

---

## 12. Bloom Filter

### 12.1 二进制布局

```
Offset  Size     Field        Description
------  ----     -----        -----------
0       4        magic        0x4D4F4C42 ("BLOM", little-endian)
4       4        version      Bloom filter 版本号（当前为 1）
8       4        hash_count   哈希函数数量
12      8        num_bits     Bit array 的总位数
20      8        row_count    插入的 key 数量
28      8        checksum     CRC32C (checksum field zeroed), zero-extended to uint64
36      N        bit_array    N = ceil(num_bits / 8) 字节
```

Header 共 36 字节。

### 12.2 Checksum 计算

将 checksum 字段（offset 28-35）置零后，对整个 bloom filter section（header +
bit_array）计算 CRC32C，零扩展为 uint64。

### 12.3 哈希方案

使用 double hashing：

```
h1 = Hash(key, seed1)
h2 = Hash(key, seed2) | 1    // 确保 h2 为奇数
bit_i = (h1 + i * h2) % num_bits,  for i in [0, hash_count)
```

### 12.4 查询

对目标 key 计算 hash_count 个 bit 位置，如果所有位都为 1 则返回 "可能存在"，
否则返回 "一定不存在"。

---

## 13. Value File

### 13.1 格式

Value File 是简单的字节拼接，没有 header 或 footer：

```
+------------------+  offset 0
|  Value 0 bytes   |
+------------------+
|  Value 1 bytes   |
+------------------+
|       ...        |
+------------------+
|  Value N-1 bytes |
+------------------+
```

每个 value 的位置由 Key File 中对应行的 (Filename, Offset, Length, Checksum) 定位。
Value 按输入顺序摆放，不排序不去重（与内嵌数据不同）。

### 13.2 Value 编码规则

Value File 中的数据编码方式取决于数据类型：

- **Bool**：1 字节，0=FALSE, 1=TRUE。
- **Int8/UInt8**：1 字节，直接存储。
- **UInt16/UInt32/UInt64**：varints 编码。
- **Int16/Int32/Int64**：zigzag + varints 编码。
- **Float/Double/LongDouble**：直接存储（4/8/16 字节）。
- **Time**：Int64(zigzag+varints) + UInt32(varints)。
- **Version**：两个 UInt64，均 varints 编码。
- **String/Binary**：原始字节（不编码）。
- **Array**：项数(varints) + 各 Variant 依次存储。
- **Map**：成员数(varints) + 各 key-value Variant 依次存储。

注意：内嵌数据（存储在 Data Table 中）不使用 zigzag/varints 编码，而是按 DataType
分组后使用 pattern 存储。编码差异是 value file 和内嵌数据的关键区别。

### 13.3 Value 完整性

读取 separated value 后，计算其 CRC32C 并与 Key File 中存储的 Checksum 比对。

---

## 14. DataType 枚举

```
Value   Name        Size      Description
-----   ----        ----      -----------
0       None        0         空类型
1       Bool        1         布尔值
2       Int8        1         有符号 8 位整数
3       Uint8       1         无符号 8 位整数
4       Int16       2         有符号 16 位整数
5       Uint16      2         无符号 16 位整数
6       Int32       4         有符号 32 位整数
7       Uint32      4         无符号 32 位整数
8       Int64       8         有符号 64 位整数
9       Uint64      8         无符号 64 位整数
10      Float       4         32 位浮点数
11      Double      8         64 位浮点数
12      LongDouble  16        128 位浮点数
13      Time        8         时间戳（微秒）
14      Version     8         版本号
15      String      variable  UTF-8 字符串
16      U16String   variable  UTF-16 字符串
17      U32String   variable  UTF-32 字符串
18      Binary      variable  二进制数据
19      Array       variable  数组（复合类型）
20      Map         variable  映射（复合类型）
```

---

## 15. Memcomparable Encoding（排序编码）

Row Key 在 block 内以 memcomparable 格式存储，使得字节序比较等价于逻辑值比较。

### 15.1 编码规则

- **有符号整数**：翻转符号位后 big-endian 存储。
- **无符号整数**：直接 big-endian 存储。
- **String/Binary**：使用 escaped encoding（每 8 字节一组，附加 continuation byte）。
- **降序列**：对编码结果按位取反。

### 15.2 All-Key 构造

All-Key 是行的完整排序键，由以下部分拼接而成：

```
MemComparable(RowKey[0]) || MemComparable(RowKey[1]) || ... ||
MemComparable(Version, descending) ||
MemComparable(OpType, ascending)
```

---

## 16. 压缩

### 16.1 Block 级压缩

当 flags.C > 0 时，block body（header 之后的部分）经过压缩。flags.C 的值（1-255）
是压缩算法的编号，具体编号与算法的映射由实现定义。Header（52 字节）始终不压缩。

- `compressed_size` 记录压缩后整个 block 的大小（含 header 的 52 字节）。
- `uncompressed_size` 记录原始整个 block 的大小（含 header 的 52 字节）。
- 当 flags.C=0（无压缩）时，`compressed_size` 必须为 0。

常见压缩算法选项：LZ4、Zstd、Snappy。

### 16.2 Checksum 与压缩的关系

Checksum 在压缩之前计算（对未压缩的完整 block），这样可以同时检测压缩算法的错误。

### 16.3 解压流程

1. 读取 52 字节 header。
2. 如果 flags.C > 0，读取 (compressed_size - 52) 字节的压缩 body，
   解压到 (uncompressed_size - 52) 字节。
3. 验证 checksum（对解压后的完整 block 计算，checksum 字段置零）。

---

## 17. 写入流程

### 17.1 SSTableBuilder 状态机

```
Created --> Adding Rows --> Finishing --> Finished
                |
                v
             Aborted
```

### 17.2 写入步骤

1. **初始化**：创建 Builder，指定 Schema 和 Options。
2. **添加行**：按 all-key 严格递增顺序添加行。
   - 如果值大小 <= MaxEmbeddedValueSize：标记为 embedded，Filename="@1"。
   - 否则：写入 Value File，记录 Filename/Offset/Length/Checksum。
3. **Flush Data Block**：当累积行的 block 大小达到阈值时，序列化为 Data Block 并追加到 Key File。
4. **构建 Index Tree**：
   - 每 flush 一个 Data Block，向 IndexTreeBuilder 注册该 block 最后一行的完整 all key
     各列值、Flag=21、Filename="@2"、Offset、Length、以及 Checksum=block 行数。
   - IndexTreeBuilder 在 Index Block 满时自动 flush，生成后序遍历布局。
   - 高层索引块中 Flag=22，Checksum=子树总行数。
5. **写入 Bloom Filter**：对所有 all-key 构建 bloom filter。
6. **写入 Metadata Sections**：CFIG、SEMA、STAT、COMP（可选）、USER（可选）。
7. **写入 Locator**：记录所有 section 的 offset/length。
8. **写入 Tail**：记录 locator_offset/length，计算 checksum。

### 17.3 Data Block 序列化

```
1. 构造 data table：
   a. 拼接变长/复合类型列的数据。
   b. 可选：对拼接数据执行多轮前缀压缩（轮数写入 flags.PC）。
   c. 追加内嵌数据（定长类型按 type 分组排序，变长类型排序去重）。
2. 为每列构造 column-store unit（选择最优 pattern）。
3. 可选：构造 rowkey bitmap。
4. 构造 offset table（项数 = 内部表格列数，若有 bitmap 则多一项）。
5. 拼接：header(52B) + data_table + units + [bitmap] + offset_table。
6. 可选：压缩 body（算法编号写入 flags.C）。
7. 计算 checksum（压缩前计算），回填 header。
```

---

## 18. 读取流程

### 18.1 打开文件

```
1. 读取文件最后 32 字节 -> Tail。
2. 验证 Tail (magic, version, checksum)。
3. 读取 [locator_offset, locator_offset + locator_length) -> Locator 字节。
4. 解码 Locator (magic, checksum, map)。
5. 从 Locator 获取 RootIndex_Offset/Length。
```

### 18.2 全表扫描

```
1. 读取 Root Index Block。
2. 遍历 Root Index 的每一行，获取子节点 Flag/Offset/Length。
3. 如果 Flag=22（IndexBlock）-> 递归读取中间索引。
4. 如果 Flag=21（DataBlock）-> 读取 Data Block，解码所有行。
5. 对于每行：
   a. 如果 embedded（Filename="@1"）-> 从 data table 读取 value。
   b. 如果 separated -> 打开 Filename 指定的文件，seek 到 Offset，读取 Length 字节。
   c. 如果 Flag.C=1 -> 验证 value 的 CRC32C == Checksum。
```

### 18.3 点查

```
1. 计算目标 key 的 bloom filter hash。
2. 查询 Bloom Filter，如果返回 false -> key 不存在。
3. 从 Root Index 开始二分查找：
   a. 找到 all key >= target_key 的最小 entry。
   b. 如果 Flag=22（IndexBlock）-> 递归进入。
   c. 如果 Flag=21（DataBlock）-> 读取该 Data Block。
4. 在 Data Block 内二分查找 all_key。
```

---

## 19. 实现范围与分期

### Phase 1（最小可用）

- Tail / Locator / Metadata sections 的编解码。
- Data Block：pattern 0 + compound pattern（子列用 pattern 0），无压缩，无 rowkey bitmap。
- Root Index Block（单层，直接指向 Data Blocks）。
- Value file 分离存储（含 zigzag/varints 编码）。
- 内嵌数据基本支持（Filename="@1"）。
- Bloom Filter。
- SSTableBuilder / SSTableReader 端到端读写。

### Phase 2（性能优化）

- Pattern 1 (Stream VByte) 和 Pattern 4/5 (constant stride)。
- 多轮前缀压缩 (data table, flags.PC > 0)。
- Block 压缩 (flags.C > 0, LZ4/Zstd)。
- Rowkey bitmap (flags.PK=1)。

### Phase 3（大文件支持）

- 多级索引树 (IXBK 中间节点, Flag=22)。
- Pattern 2 (PFOR) 和 Pattern 3 (Dictionary)。
- 多 Bloom Filter keys。

---

## 20. 设计约束与不变量

1. Key file 写入后不可变（append-only build, immutable after finish）。
2. 所有多字节整数使用小端序（little-endian）。
3. 所有 checksum 使用 CRC32C，存储为 uint64（高 32 位为 0）。
4. Block 内的行按 all_key 严格递增排序。
5. Index tree 的叶子节点按 key 顺序排列，保证顺序扫描的 I/O 局部性。
6. Locator 是唯一的 section 定位机制，reader 不应假设 section 的物理顺序。
7. Tail 是唯一的入口点，reader 从文件末尾 32 字节开始解析。
8. Pattern 0 是所有 column-store unit 的 fallback，任何 pattern 选择失败时回退到 pattern 0。
