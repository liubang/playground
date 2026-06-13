# 索引树

SSTable v2 的索引是一棵多级、基于扇出（fanout）的树，将键范围映射到数据块位置。
索引块内联存储在 Key 文件中（与数据块交替排列），根 Block 作为独立 Section 由文件 Tail 引用。

## 设计

### 为什么要多级索引？

单级索引 N 个条目需要 O(log N) 在索引块内二分查找。对于超大型 SSTable（数百万行），索引块本身就会变得很大。
多级树将每个 Block 保持在小范围内（<= 128 KB），每层的二分查找都很快，而树的深度与数据块数量成对数关系。

### 扇出（Fanout）

分叉因子决定树深：

```
fanout = 256  ->  每个索引块覆盖 256 个数据块
              ->  第 2 层覆盖 256^2 = 65,536 个数据块
              ->  第 3 层覆盖 256^3 = 16,777,216 个数据块
```

扇出 256 的三层树可索引约 1600 万个数据块（按 64K 行/块计算约 1 万亿行）。

## 树结构

```
                         +--------------+
                         |  ROOT block  |  1 block, kRootIndex
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

## 索引条目格式

索引条目是一个普通的 `InternalRow`，但带有特殊的列标记：

```cpp
// Data block entry:
//   key columns = fence row (first key in the data block)
//   offset column = absolute byte offset in key file
//   length column = block length
//   flag column = ColumnFlag::for_data_block()

// Index block entry:
//   key columns = fence row (last key in the child block)
//   offset column = absolute byte offset of child index block
//   length column = child index block length
//   flag column = ColumnFlag::for_index_block()
```

对索引块，fence 行是每个子块的**最后一个**键；对数据块，fence 行是每个子块的**第一个**键。这确保了正确的二分查找语义。

## TreeBuilder API

```cpp
class TreeBuilder {
public:
    TreeBuilder(InternalSchema::ConstRef schema,
                uint64_t fanout,
                uint64_t soft_limit,     // target block size (default: 64KB)
                uint64_t hard_limit,     // max block size (default: 128KB)
                compress::Options compression,
                std::string* key_file);  // appends encoded blocks here

    // Must be called before each add_data_block() to check if level 0 needs flushing
    absl::Status prepare_for_data_block();

    // Add a data block reference to the index
    absl::Status add_data_block(const InternalRow& fence,
                                 BlockRef data_block,
                                 uint64_t row_count);

    // Finish and flush remaining levels -> returns root block reference
    absl::StatusOr<FinishResult> finish();
};
```

### 写入协议

```cpp
TreeBuilder index(schema, fanout, soft_limit, hard_limit, compression, &key_file);

for (each data block) {
    index.prepare_for_data_block();     // may flush a full level-0 index block
    index.add_data_block(fence_row, block_ref, row_count);
}

auto result = index.finish();           // flushes all remaining levels
// -> result.root = BlockRef to root index block
// -> result.block_count = total index blocks written
```

## TreeReader API

```cpp
class TreeReader {
public:
    // Full scan: return all data block refs
    static absl::Status scan_data_blocks(
        std::string_view key_file,
        const InternalSchema::ConstRef& schema,
        BlockRef root,
        std::vector<BlockRef>* data_blocks);

    // Prefix scan: data blocks starting from start_key
    static absl::Status scan_data_blocks_from(
        std::string_view key_file,
        const InternalSchema::ConstRef& schema,
        BlockRef root,
        const PrefixKey& start_key,
        std::vector<BlockRef>* data_blocks);

    // Range scan: data blocks in [start_key, limit_key)
    static absl::Status scan_data_blocks_in_range(
        std::string_view key_file,
        const InternalSchema::ConstRef& schema,
        BlockRef root,
        const std::optional<PrefixKey>& start_key,
        const std::optional<PrefixKey>& limit_key,
        std::vector<BlockRef>* data_blocks);

    // Point lookup: find the data block containing target_key
    static absl::StatusOr<std::optional<BlockRef>> find_data_block(
        std::string_view key_file,
        const InternalSchema::ConstRef& schema,
        BlockRef root,
        const AllKey& target_key);
};
```

### 搜索算法

**点查**（`find_data_block`）：

```
function find_data_block(key_file, schema, root, target_key):
    block = open_block(key_file, root, kRootIndex)
    while block.kind != kData:
        // Binary search for first entry >= target_key
        entry = binary_search_first_ge(block.rows, target_key)
        // Open child block
        block = open_block(key_file, entry.block_ref, entry.kind)
    // block is now a data block -> binary search for exact match
    return block.ref
```

**范围扫描**（`scan_data_blocks_in_range`）：

```
function scan_data_blocks_in_range(key_file, schema, root, start, limit):
    blocks = []
    stack = [(root, kRootIndex)]
    while stack is not empty:
        (block_ref, kind) = stack.pop()
        block = open_block(key_file, block_ref, kind)
        if block.kind == kData:
            if block_overlaps_range(block, start, limit):
                blocks.append(block_ref)
        else:
            // Index block: push qualifying children
            for each child in block:
                if child_overlaps_range(child, start, limit):
                    stack.push((child.block_ref, child.kind))
    return blocks
```

## BlockRef

```cpp
struct BlockRef {
    uint64_t offset = 0;   // Absolute byte offset in key file
    uint64_t length = 0;   // Block length in bytes
};

struct FinishResult {
    BlockRef root;           // Root index block location
    uint64_t block_count = 0; // Total index blocks written
};
```

## Key 文件中的排放位置

索引块与数据块在 Key 文件中交替排列。当第 0 层索引块满（达到 fanout 条目数）时，立即刷写到 Key 文件中，
产生一个第 1 层索引条目。第 1 层 Block 满时同理向上刷写。这意味着所有层的索引块都在构建过程中内联写入，
只有根 Block 需要在文件 Tail 中以独立 Section 引用。

```
Key file byte stream:
  [Data Blk 0] [Data Blk 1] ... [Data Blk F-1]           <- F data blocks
  [Index Blk (level 0, first)]                            <- flushed when full
  [Data Blk F] [Data Blk F+1] ... [Data Blk 2F-1]
  [Index Blk (level 0, second)]
  ...
  [Index Blk (level 1)]                                   <- flushed when level 1 full
  ...
  [Root Index Blk]                                        <- flushed by finish()
```

## 配置参数

| 参数         | 默认值 | 描述                  |
| ------------ | ------ | --------------------- |
| `fanout`     | 256    | 每个索引块的条目数    |
| `soft_limit` | 64 KB  | 目标压缩后 Block 大小 |
| `hard_limit` | 128 KB | 最大压缩后 Block 大小 |

扇出与 Block 大小限制交互。如果条目非常大（长键），Block 可能在达到扇出之前就触及硬限制，此时 Block 以较少条目刷写。

## 与 SSTable v1 的对比

| 方面              | SSTable v1              | SSTable v2                 |
| ----------------- | ----------------------- | -------------------------- |
| 索引结构          | 单级 Block              | 多级树                     |
| 索引存储          | 独立 Section            | 内联 + 根 Section          |
| 索引中的键        | 原始 cell key 字节      | Memcomparable 编码 all-key |
| 最大 SSTable 大小 | 约 4 GB（单级索引限制） | 无限制（多级树）           |
| 查找复杂度        | 单索引中 O(log N)       | 每层 O(log_fanout N)       |
