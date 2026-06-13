# SSTable v2 文件格式

## 总览

一个 SSTable v2 由两个文件组成：

| 文件       | 扩展名        | 内容                                     |
| ---------- | ------------- | ---------------------------------------- |
| Key 文件   | `*.sstv2`     | 数据块、索引块、布隆过滤器、元数据、tail |
| Value 文件 | `value.sstv2` | 嵌入值（行外存储的大对象）               |

Key 文件是自描述的：其 tail 包含所有 section 的偏移量。Value 文件是一个简单的追加写值日志。

## Key 文件布局

```
+---------------------------------------------------+  offset 0
|                  Data Block 0                     |
|  +----------+-------------------+--------+-----+  |
|  | Header   | Compressed Payload| Offset |Footr|  |
|  | (52 B)   | (rows + embedded) | Table  |16 B |  |
|  +----------+-------------------+--------+-----+  |
+---------------------------------------------------+
|                  Data Block 1                     |
|  ...                                              |
+---------------------------------------------------+
|                  ...                              |
+---------------------------------------------------+
|             Index Blocks (inline, multi-level)    |
+---------------------------------------------------+
|             Bloom Section                         |
+---------------------------------------------------+
|             Metadata Section                      |
+---------------------------------------------------+
|             Root Index Section                    |
+---------------------------------------------------+
|             Tail (56 bytes)                       |  <-- end of file
+---------------------------------------------------+
```

## Section 格式

每个 Section（Bloom、Metadata、Root Index）将其载荷包装为：长度前缀 + 载荷 + 校验和 + 魔数：

```
+-----------------+---------------------------+---------------------+------------------+
| Length (uint64) | Payload (Length bytes)    | Checksum (xxHash64) | Magic (uint32)   |
|                 |                           | over Payload only   | Section type     |
+-----------------+---------------------------+---------------------+------------------+
```

Section 魔数：

| Section    | Magic                 | 描述             |
| ---------- | --------------------- | ---------------- |
| Bloom      | `0x4D4F4C42` ("BLOM") | 布隆过滤器位数组 |
| Metadata   | `0x4454454D` ("METD") | 配置 + 统计信息  |
| Root Index | `0x544F4F52` ("ROOT") | 根索引块         |

## Block 格式

所有 Block（Data、Index、Root Index）共用同一种线格式。

### Block Header（52 字节）

```
Offset  Size   Field                Description
------  ----   -----                -----------
0       4      magic                DTBK / IXBK / ROOT
4       8      flags                Block-level flags
12      8      row_count            Number of rows in block
20      8      offset_table_offset  Byte offset to offset table
28      8      uncompressed_length  Size before compression
36      8      compressed_length    Size after compression
44      8      checksum             xxHash64 of compressed data
```

Block 魔数：

| Magic                 | 名称   | 用途                                  |
| --------------------- | ------ | ------------------------------------- |
| `0x4B425444` ("DTBK") | 数据块 | 存储用户行                            |
| `0x4B425849` ("IXBK") | 索引块 | 存储索引条目（fence 行 + Block 引用） |
| `0x544F4F52` ("ROOT") | 根索引 | 顶层索引块                            |

### 压缩后的载荷

Header 与偏移表之间的部分是行数据载荷：

```
Row 0 data | Row 1 data | ... | Row N-1 data | Embedded values section
```

每行按 schema 顺序编码其内部列。嵌入值部分按偏移量引用存储行外的大值数据（长字符串 / Blob）。

### 偏移表

压缩载荷之后：

```
offset[0] (uint32) | offset[1] (uint32) | ... | offset[N-1] (uint32)
```

每个 `offset[i]` 指向未压缩载荷中第 i 行的起始位置，实现 O(1) 随机访问。

### Block 校验和计算

1. 将 block checksum 字段置零。
2. 对整个 block（header + compressed payload + offset table）计算 xxHash64。
3. 结果写入 checksum 字段。

## 布隆过滤器格式

### Bloom Header（36 字节）

```
Offset  Size   Field              Description
------  ----   -----              -----------
0       4      magic              0x4D4F4C42 ("BLOM")
4       4      version            1
8       4      hash_count         Number of hash functions (k)
12      8      bit_count          Size of bit array in bits
20      8      row_count          Number of rows indexed
28      8      checksum           xxHash64 of the bit array
```

### 位数组

位数组紧随 Header。位按 LSB 优先打包进每个字节。

## 元数据 Section 格式

包含两个子记录：

### Configuration

```
Field                    Type      Description
---------------------    ------    -----------------------------
max_data_block_rows      uint64    Max rows per data block
index_fanout             uint64    Index tree branching factor
block_soft_limit         uint64    Target block size (bytes)
block_hard_limit         uint64    Max block size (bytes)
block_compression        uint32    Compression algorithm ID
bloom_bits_per_key       uint32    Bloom filter bits per key
value_file_name_length   uint32    Length of value file name
value_file_name          string    Value file name (UTF-8)
```

### Statistics

```
Field                    Type      Description
---------------------    ------    -----------------------------
total_row_count          uint64    Total rows in SSTable
total_data_block_count   uint64    Total data blocks
total_index_block_count  uint64    Total index blocks
key_file_size            uint64    Key file size (bytes)
value_file_size          uint64    Value file size (bytes)
min_timestamp            uint64    Earliest row timestamp
max_timestamp            uint64    Latest row timestamp
```

## Tail 格式（56 字节）

Tail 是 Key 文件的最后 56 字节，是读取的入口点。

```
Offset  Size   Field                 Description
------  ----   -----                 -----------
0       8      root_index_offset     Offset to root index section
8       8      root_index_length     Length of root index section
16      8      bloom_offset          Offset to bloom section
24      8      bloom_length          Length of bloom section
32      8      metadata_offset       Offset to metadata section
40      8      metadata_length       Length of metadata section
48      4      magic                 0x32565453 ("STV2")
52      4      version               1
```

## Value 文件布局

```
+------------------+
| Embedded Value 0 |  <-- variable length, raw bytes
| Embedded Value 1 |
| ...              |
| Embedded Value N |
+------------------+
```

Value 通过 InternalRow 中 `filename`、`offset`、`length` 系统列存储的 (offset, length) 来引用。

## 读取算法

### 点查（get）

```
1. read Tail (last 56 bytes of key file) -> root_index_offset
2. read Root Index Section at root_index_offset
3. open Root Index Block -> BlockReader
4. binary search root index rows for target AllKey -> child index block ref
5. repeat steps 3-4 until reaching a data block ref
6. open data block -> BlockReader
7. binary search data block rows for target key
8. if found, read embedded value from value file
```

### 范围扫描（scan）

```
1. read Tail -> root_index_offset
2. read Root Index Section
3. walk index tree from root:
   a. for each index block, find all child blocks in [start, limit) range
   b. recurse into qualifying index blocks
   c. collect qualifying data block refs
4. for each qualifying data block:
   a. open block -> BlockReader
   b. scan rows in [start, limit) range
   c. read embedded values from value file
```

## 压缩

压缩以 Block 为单位。压缩算法 ID 存储在 Block Header 的 flags 字段中。

| ID  | 算法   | 备注               |
| --- | ------ | ------------------ |
| 0   | None   | 无压缩             |
| 1   | Zstd   | 默认，较好的压缩比 |
| 2   | Snappy | 快速压缩 / 解压    |
| 3   | LZ4    | 最快，中等压缩比   |

## 完整性校验

- **Block 级**：Block Header 中的 xxHash64 校验和
- **Section 级**：xxHash64 校验和覆盖 Section 载荷
- **文件级**：Tail 魔数（`STV2`）验证文件身份
