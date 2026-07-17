# minitable Key 编码规范

本文定义 minitable 复合 RowKey、StorageKey 与 VersionedStorageKey 的 canonical memcomparable 编码。它解释为什么异构多列主键编码后可以直接做无符号字节字典序比较，并给出格式、证明、反例、golden vector 与严格解码要求。

本文描述的是持久格式契约，不是实现建议。修改本文中的 tag、宽度、变换、字段顺序或 canonical 规则，必须升级 KeyFormat version，并同步迁移 Table metadata、Manifest、SST properties、MemTable、路由与测试。

相关实现：

- `cpp/pl/minitable/codec/cell_key_codec.{h,cpp}`
- `cpp/pl/sstv2/codec/scalar_comparable.{h,cpp}`
- `cpp/pl/sstv2/codec/ordered_uint.{h,cpp}`
- `cpp/pl/minitable/ut/codec/cell_key_codec_test.cpp`
- `cpp/pl/sstv2/ut/codec/comparable_test.cpp`

---

## 1. 目标与非目标

### 1.1 目标

对同一 comparator domain 中的逻辑 Key，编码必须同时满足：

1. **可逆性**

\[
D(E(x))=x
\]

2. **单射与 canonical 唯一性**

\[
x=y \iff E(x)=E(y)
\]

3. **保序性**

\[
x<y \iff E(x)<_{bytes}E(y)
\]

4. **可连接性**：多个异构分量编码直接连接后，整体字节序等价于 tuple 字典序。
5. **自终止性**：变长分量无需外部长度即可确定结束位置。
6. **严格解码**：截断、未知 tag、非 canonical 表示、越界和 trailing bytes 必须失败。

满足这些性质后，MemTable、SST index、Bloom、MergeIterator 和 Compaction 热路径只需比较编码字节，不必反复解析类型和值。

### 1.2 非目标

- 不允许比较任意来源的 encoded key；只有相同 comparator domain 的 key 才可直接比较。
- HASH 表的物理顺序不是原始 RowKey 的全表全局序。
- RowKey 不支持 NULL、ARRAY、MAP 和未固定语义的 NaN。
- 本格式不提供跨 KeyFormat version 的隐式兼容。

---

## 2. Comparator domain

直接 byte compare 的前提是双方属于同一个 comparator domain。该 domain 至少由以下信息确定：

```text
ComparatorDomain =
    key_format_version
  + row_key_schema_fingerprint
  + partition_mode
  + hash_algorithm_version
  + virtual_bucket_count
```

RowKey schema fingerprint 必须覆盖每列的：

```text
(column_id, position, data_type, sort_order)
```

同一张表内，第 \(i\) 个 RowKey 分量的类型和排序方向固定。因此比较两个 Key 时，永远是：

```text
A.column[i] 的类型 T_i  vs  B.column[i] 的类型 T_i
```

不会发生 STRING 与 UINT64 在同一列位置互比。类型信息在编码阶段已经转化为字节排列规则，比较阶段不再需要 type tag。

下列 encoded key 禁止直接混合比较：

- 不同表或不同 RowKey schema；
- KeyFormat version 不同；
- ASC/DESC 定义不同；
- HASH 与 GLOBAL_ORDER；
- hash algorithm 或 virtual bucket count 不同；
- canonical 与非 canonical 编码。

---

## 3. 比较模型

### 3.1 字节序

本文中的 byte compare 是对无符号字节序列做 lexicographical comparison：

1. 从左到右找到第一个不同字节；
2. 较小的无符号字节所在序列更小；
3. 若公共部分完全相同，较短序列更小。

记为：

\[
<_{bytes}
\]

### 3.2 复合主键 tuple 序

给定 RowKey schema：

\[
S=((T_0,O_0),(T_1,O_1),\ldots,(T_{n-1},O_{n-1}))
\]

其中 \(T_i\) 是类型，\(O_i\) 是 ASC 或 DESC。逻辑主键为：

\[
K=(k_0,k_1,\ldots,k_{n-1})
\]

两个 Key 的 tuple 比较由第一个不同分量决定。若该位置为 ASC，按类型自然序比较；若为 DESC，反转结果。

编码定义为各列编码的直接连接：

\[
E_S(K)=E_{T_0,O_0}(k_0)\Vert E_{T_1,O_1}(k_1)\Vert\cdots\Vert E_{T_{n-1},O_{n-1}}(k_{n-1})
\]

其中 \(\Vert\) 表示 byte concatenation。

---

## 4. 为什么异构多列可以直接 byte compare

### 4.1 直观示例

Schema：

```text
(tenant STRING ASC, user_id UINT64 ASC, score DOUBLE DESC)
```

Key：

```text
A = ("foobar", 100, 9.5)
B = ("foobar", 101, 1.0)
```

编码：

```text
E(A) = EString("foobar") | EUint64(100) | EDoubleDesc(9.5)
E(B) = EString("foobar") | EUint64(101) | EDoubleDesc(1.0)
```

比较过程：

1. tenant 编码相同；
2. 进入 user_id 编码；
3. big-endian `100 < 101`；
4. 结果已经确定，score 不再参与比较。

虽然不同列类型不同，但同一位置的类型始终相同。

### 4.2 拼接保序定理

对每个列位置 \(i\)，假设编码 \(E_i\) 满足：

1. 单射：\(a=b \iff E_i(a)=E_i(b)\)；
2. 按该列方向保序：\(a<_{O_i}b \iff E_i(a)<_{bytes}E_i(b)\)；
3. 编码边界唯一可解析。

取两个不同 Key：

\[
A=(a_0,\ldots,a_{n-1}),\quad B=(b_0,\ldots,b_{n-1})
\]

令 \(j\) 是第一个满足 \(a_j\ne b_j\) 的位置，则：

\[
a_i=b_i,\quad 0\le i<j
\]

由单射性：

\[
E_i(a_i)=E_i(b_i),\quad 0\le i<j
\]

所以两个连接编码在前 \(j-1\) 个分量完全相同，第一次可能产生差异的位置位于第 \(j\) 个分量。由 \(E_j\) 的保序性：

\[
a_j<_{O_j}b_j \iff E_j(a_j)<_{bytes}E_j(b_j)
\]

因此：

\[
A<_{tuple}B \iff E_S(A)<_{bytes}E_S(B)
\]

后续分量不会影响已经确定的结果。

### 4.3 为什么边界唯一仍然必要

仅有“单列编码保序”还不够。如果变长分量没有唯一边界，两个不同 tuple 可能碰撞：

```text
("a", "bc") -> "abc"
("ab", "c") -> "abc"
```

此时单射性和可解析性同时失效。因此 STRING/BINARY 必须使用自终止编码，而不能拼接原始 bytes。

---

## 5. 标量编码

以下编码均输出 fixed-width big-endian 或 canonical self-delimiting bytes。

### 5.1 BOOL

```text
false -> 00
true  -> 01
```

Strict decoder 拒绝其他值。

### 5.2 无符号整数

UINT32/UINT64 使用固定宽 big-endian：

```text
UINT32 1   -> 00 00 00 01
UINT32 255 -> 00 00 00 FF
UINT32 256 -> 00 00 01 00
```

对相同宽度无符号整数，最高有效字节最先比较，因此：

\[
x<y \iff BE(x)<_{bytes}BE(y)
\]

固定宽度不能省略前导零；否则长度变化会改变比较语义。

### 5.3 有符号整数

二进制补码按无符号字节比较时，负数会排在正数之后。编码先翻转最高符号位，再按 big-endian 输出。

对 \(w\) 位整数：

\[
U(x)=bits(x)\oplus 2^{w-1}
\]

然后：

\[
E(x)=BE(U(x))
\]

INT32 示例：

```text
INT32_MIN -> 00 00 00 00
-1        -> 7F FF FF FF
0         -> 80 00 00 00
1         -> 80 00 00 01
INT32_MAX -> FF FF FF FF
```

符号位翻转等价于将补码环形空间在有符号最小值处切开，得到自然有符号顺序。

### 5.4 FLOAT 与 DOUBLE

IEEE 754 正数 bit pattern 按无符号整数排序时与数值顺序一致；负数区域方向相反。设原始 bits 为 \(b\)，符号位 mask 为 \(M\)：

\[
Ordered(b)=
\begin{cases}
\sim b, & b\ \&\ M\ne 0\\
b\oplus M, & b\ \&\ M=0
\end{cases}
\]

结果再按 big-endian 输出。

直观上：

- 负数全部按位取反，修正负数内部顺序；
- 非负数只翻转符号位，使其排在负数之后；
- 数值从负无穷到正无穷映射为递增无符号整数。

Minitable RowKey 额外规定：

- NaN 非法；
- `-0` 在编码前 canonicalize 为 `+0`；
- strict decoder 拒绝持久化的 NaN 和 `-0`。

这是因为逻辑 equality 将 `-0` 与 `+0` 视为同一个 Key；若允许两个 bit pattern，会破坏 canonical 唯一性。

### 5.5 DESC

任意 ASC 编码 \(E(x)\) 的每个字节按位取反：

\[
E_{desc}(x)=\sim E_{asc}(x)
\]

若两个编码第一次不同的字节为 \(p<q\)，取反后有：

\[
\sim p>\sim q
\]

因此：

\[
x<y \iff E_{desc}(x)>_{bytes}E_{desc}(y)
\]

正向扫描即可得到逻辑降序。

---

## 6. STRING/BINARY 自终止编码

### 6.1 为什么不能直接拼接

Schema：

```text
(first STRING, second STRING)
```

朴素 raw concatenation 会发生碰撞：

```text
("a",  "bc") -> 61 62 63
("ab", "c")  -> 61 62 63
```

因此必须编码每个分量的结束位置。

### 6.2 为什么不能简单使用 length prefix

假设编码为 `[length][raw bytes]`：

```text
"a"  -> 01 61
"b"  -> 01 62
"aa" -> 02 61 61
```

byte order 得到：

```text
"a" < "b" < "aa"
```

但字符串自然字典序是：

```text
"a" < "aa" < "b"
```

length prefix 将排序错误地变为长度优先。

### 6.3 为什么不能简单使用 NUL terminator

编码 `raw bytes + 00` 无法表示包含 NUL 的 BINARY：

```text
原值 "a\0b" -> 61 00 62 00
```

Decoder 无法判断第一个 `00` 是数据还是结束符。可以设计 escape，但需要逐字节分支和可变扩张；当前格式采用固定分组。

### 6.4 8+1 group 格式

输入按最多 8 bytes 分组，每组固定编码为：

```text
[8 data bytes][1 marker byte]
```

marker：

```text
FF      非最终组，8 个 data bytes 全部有效，后面还有组
00..08  最终组，数值表示该组有效 data bytes 数量
```

最终组不足 8 bytes 时，尾部必须补 `00`。空值也编码为一个最终组。

| 原始长度 | 编码 |
|---:|---|
| 0 | `00 00 00 00 00 00 00 00 00` |
| 1，`"a"` | `61 00 00 00 00 00 00 00 01` |
| 2，`"ab"` | `61 62 00 00 00 00 00 00 02` |
| 8，`"abcdefgh"` | `61 62 63 64 65 66 67 68 08` |
| 10，`"abcdefghij"` | `61 62 63 64 65 66 67 68 FF 69 6A 00 00 00 00 00 00 02` |

长度为 \(n\) 的值使用：

\[
g=\max(1,\lceil n/8\rceil)
\]

个 group，编码长度为：

\[
9g
\]

长值空间开销趋近 12.5%。

### 6.5 Decoder 如何定位下一列

伪代码：

```text
output = empty
saw_continuation = false
loop:
    require at least 9 bytes
    data = next 8 bytes
    marker = next byte

    if marker == FF:
        append all 8 data bytes
        saw_continuation = true
        continue

    require marker <= 8
    reject saw_continuation && marker == 0
    require data[marker..7] are all zero
    append data[0..marker)
    return consumed byte count
```

返回的 consumed byte count 精确指向下一列开头。Decoder 不依赖外部字符串长度，也不扫描 sentinel。

例如 Schema：

```text
(name STRING, id UINT32)
```

Key：

```text
("abcdefghij", 42)
```

编码：

```text
61 62 63 64 65 66 67 68 FF
69 6A 00 00 00 00 00 00 02
00 00 00 2A
```

前两个 9-byte group 自终止于 marker `02`；紧随其后的 `00 00 00 2A` 是 UINT32。

### 6.6 保序性：第一个不同数据字节

若两个 byte string 在第一个不同位置就存在不同数据字节，则 group 编码把该数据字节原样放在 marker 之前，因此原始字典序直接保留。

```text
"ab" -> 61 62 00 00 00 00 00 00 02
"ac" -> 61 63 00 00 00 00 00 00 02
                 62 < 63
```

所以：

\[
"ab"<"ac" \iff E("ab")<_{bytes}E("ac")
\]

### 6.7 保序性：真前缀

若 \(a\) 是 \(b\) 的真前缀，原始字典序规定 \(a<b\)。需要讨论前缀结束位置。

#### 前缀在 group 内结束

```text
"a"   -> 61 00 00 00 00 00 00 00 01
"a\0" -> 61 00 00 00 00 00 00 00 02
                                           01 < 02
```

即使新增字节是 `00`，marker 也能区分真实长度。

#### 前缀恰好在 8-byte 边界结束

```text
"abcdefgh"
-> 61 62 63 64 65 66 67 68 08

"abcdefgh\0"
-> 61 62 63 64 65 66 67 68 FF
   00 00 00 00 00 00 00 00 01
```

第一组 data 相同，而：

```text
08 < FF
```

所以较短前缀仍排在较长值之前。Continuation marker 必须大于所有 final marker；选择 `FF` 正是为了维持该性质。

### 6.8 含任意零字节仍然无歧义

零字节是否为 padding 由 marker 决定，不由其数值决定：

```text
"a\0" -> 61 00 00 00 00 00 00 00 02
```

前两个 bytes 是真实 payload，后六个才是 padding。因此 BINARY 可以包含任意 bytes，包括 `00` 和 `FF`。

### 6.9 Canonical 唯一性

Strict decoder 必须拒绝所有等价但非 canonical 表示。

#### 非零 padding

`"ab"` 的唯一表示是：

```text
61 62 00 00 00 00 00 00 02
```

以下表示必须拒绝：

```text
61 62 FF EE DD CC BB AA 02
```

否则不同物理 key 会解码为同一个逻辑值，破坏 Bloom、point lookup、Compaction 和 tombstone 匹配。

#### 额外空终止组

恰好 8 bytes 的值必须编码为：

```text
61 62 63 64 65 66 67 68 08
```

不能编码为：

```text
61 62 63 64 65 66 67 68 FF
00 00 00 00 00 00 00 00 00
```

因此 continuation 后的 final marker 不允许为 `00`。

### 6.10 Bytes 保序性的形式化说明

对两个不同 byte string \(x,y\)，令 \(p\) 为第一个不同原始 byte 的位置；若不存在，则一个是另一个的真前缀。

- 若 \(p\) 存在：在包含 \(p\) 的 group 中，前面的 payload bytes 相同，第 \(p\) 个原始 byte 原样出现在 marker 前，故编码比较结果等于原始 byte 比较结果。
- 若 \(x\) 是 \(y\) 的真前缀：
  - 当前 group 内结束时，相同 padding 后 final marker 以有效长度递增；
  - 8-byte 边界结束时，短值 final marker `08` 小于长值 continuation marker `FF`。

故：

\[
x<_{bytes}y \iff E_{bytes}(x)<_{bytes}E_{bytes}(y)
\]

再结合自终止和 canonical 校验，bytes 编码满足连接定理所需的三个条件。

---

## 7. Ordered uint32

CF ID 和 static column ID 使用 shortest big-endian ordered uint32：

```text
0          -> 00
1          -> 01 01
255        -> 01 FF
256        -> 02 01 00
65535      -> 02 FF FF
65536      -> 03 01 00 00
UINT32_MAX -> 04 FF FF FF FF
```

非零格式：

```text
[payload width: 01..04][shortest big-endian payload]
```

宽度 byte 先比较，因此较少有效 bytes 的数值总是更小；相同宽度内由 big-endian 保序。Decoder 拒绝：

- width 大于 4；
- 截断；
- payload 首字节为零；
- 非 shortest 表示。

注意：virtual bucket ID 不使用该格式，而是固定 4-byte big-endian，以保持物理 range 边界宽度稳定。

---

## 8. RowKey 编码示例与推导

Schema：

```text
(region STRING ASC, user_id UINT32 ASC, timestamp UINT64 DESC)
```

Key：

```text
A = ("cn", 10, 200)
B = ("cn", 10, 100)
C = ("cn", 11, 999)
D = ("usa", 1, 999)
```

编码结构：

```text
A = EBytes("cn")  | EUint32(10) | ~EUint64(200)
B = EBytes("cn")  | EUint32(10) | ~EUint64(100)
C = EBytes("cn")  | EUint32(11) | ~EUint64(999)
D = EBytes("usa") | EUint32(1)  | ~EUint64(999)
```

结果：

```text
A < B < C < D
```

原因：

- A/B 前两列相同；timestamp 为 DESC，因此 200 排在 100 前；
- B/C region 相同，`10 < 11`，timestamp 不参与；
- C/D 在第一列即有 `"cn" < "usa"`，后续列不参与。

这同时展示了异构列、变长边界和混合 ASC/DESC 如何共同工作。

---

## 9. StorageKey 物理布局

### 9.1 GLOBAL_ORDER

```text
LogicalRowKey
+ RecordPrefix
+ OrderedCfId
+ optional QualifierToken
```

### 9.2 HASH

```text
VirtualBucketId: fixed uint32 big-endian
+ LogicalRowKey
+ RecordPrefix
+ OrderedCfId
+ optional QualifierToken
```

HASH 的 byte order 是：

```text
bucket order -> bucket 内 RowKey order -> record target order
```

它不承诺原始 RowKey 跨 bucket 全局有序。

### 9.3 Record target

```text
00 + ordered_uint32(0)                row tombstone
01 + ordered_uint32(cf_id)            CF tombstone
02 + ordered_uint32(cf_id) + qualifier ordinary cell
```

用户 CF ID 从 1 开始，0 永久保留。因此同一 RowKey 下：

```text
row tombstone < CF tombstone < ordinary cell
```

Qualifier：

```text
00 + ordered_uint32(column_id) static qualifier
01 + memcomparable_bytes(value) dynamic qualifier
```

空 dynamic qualifier 仍有完整 bytes 终止组，不会与 static qualifier 或缺失 qualifier 混淆。

### 9.4 当前 golden vector

Schema：

```text
(tenant UINT64 ASC, name STRING ASC)
```

GLOBAL_ORDER Cell：

```text
tenant = 7
name = "ab"
cf_id = 3
static column_id = 9
```

分解：

```text
0000000000000007        UINT64 tenant
616200000000000002      bytes("ab")
02                      ordinary cell
0103                    ordered_uint32(3)
00                      static qualifier
0109                    ordered_uint32(9)
```

完整 hex：

```text
0000000000000007616200000000000002020103000109
```

HASH + embedded NUL 示例：

```text
bucket = 17
tenant = 1
name = ""
cf_id = 2
dynamic qualifier = "a\0b"
```

分解：

```text
00000011                fixed bucket 17
0000000000000001        tenant 1
000000000000000000      bytes("")
02                      ordinary cell
0102                    ordered_uint32(2)
01                      dynamic qualifier
610062000000000003      bytes("a\0b")
```

完整 hex：

```text
00000011000000000000000100000000000000000002010201610062000000000003
```

---

## 10. MVCC Version 后缀

VersionedStorageKey：

```text
StorageKey
+ DescendingUint64(commit_ts.domain_epoch)
+ DescendingUint64(commit_ts.counter)
+ DescendingUint32(mutation_seq)
+ Uint8(op_type)
```

对固定 StorageKey，较大的 `(commit_ts, mutation_seq)` 经逐字节取反后编码更小，因此正向扫描首先看到最新版本：

\[
T_{new}>T_{old}\Rightarrow E_{desc}(T_{new})<_{bytes}E_{desc}(T_{old})
\]

Row/CF tombstone 只能搭配 Delete。Cell 可以搭配 Put、Merge 或 Delete。Strict encoder/decoder 均须拒绝非法组合。

---

## 11. Prefix、range 与直接比较

完整 RowKey 编码是所有 RowKey 分量编码的连接。RowKey 的前 \(m\) 列编码也恰好是完整编码的 byte prefix：

\[
E(k_0,\ldots,k_{m-1})
\]

因此在相同 schema/domain 下可以构造 typed prefix seek。但不能在任意 raw byte 位置截断：

- 固定宽分量只能在完整宽度边界截断；
- bytes 分量只能在完整 final group 后截断；
- descending 编码同样必须保持分量完整。

Range boundary 必须由 schema-aware encoder 生成，禁止调用方手工拼接或递增任意尾字节。

---

## 12. Strict decode 与安全要求

Persisted-key decoder 必须拒绝：

- 任意截断位置；
- trailing bytes；
- 未知 partition/record/qualifier/op tag；
- 超范围 virtual bucket；
- CF ID 0 的普通 Cell 或 CF tombstone；
- static column ID 0；
- ordered uint 的非 shortest 表示；
- bytes 非零 padding；
- bytes continuation 后的空 final group；
- NaN 和持久化 `-0`；
- Row/CF tombstone 搭配非 Delete；
- 超出持久 KeyFormat hard limit 的数据。

资源限制必须属于持久 Table KeyFormat，不能由各节点可变运行参数决定。否则同一 SST 可能在不同副本被判定为合法或损坏。

对任意 strict decode 成功的 persisted key，应满足 canonical oracle：

\[
Encode(Decode(bytes))=bytes
\]

该性质适合作为 fuzz/property test 的核心断言。

---

## 13. 常见错误设计

### 13.1 拼接原始变长 bytes

会产生列边界碰撞：

```text
("a", "bc") ==encoded ("ab", "c")
```

### 13.2 普通 length prefix

会变为长度优先，破坏字符串字典序。

### 13.3 允许多种等价编码

会导致逻辑相同的 Key 在 MemTable/SST/Bloom 中成为不同物理 Key。

### 13.4 跨 schema 比较

相同 byte offset 表示不同类型或方向，比较结果没有业务语义。

### 13.5 对有符号整数直接 big-endian

负数补码最高位为 1，会错误地排在非负数后面。

### 13.6 直接比较 IEEE 浮点 bits

负数区域顺序相反，且 NaN/signed zero 会破坏逻辑 equality 与 canonical 唯一性。

### 13.7 为每列增加 type tag

在 RowKey schema 已固定类型时属于冗余空间开销，也不能替代 comparator domain 校验。Type tag 适用于自描述异构容器，不适用于固定 schema 的热路径 RowKey。

---

## 14. 测试与格式冻结要求

格式冻结前至少覆盖：

- 每个标量的 min、max、零和符号边界；
- signed integer 的 `-1/0` 边界；
- float/double 的负数、零、正数、无穷；
- NaN 拒绝和 `-0` canonicalization；
- bytes 长度 `8n-1`、`8n`、`8n+1`；
- bytes 内嵌 `00`、`FF`；
- bytes 真前缀和 8-byte 边界真前缀；
- 非零 padding 和额外空终止组拒绝；
- ordered uint 宽度边界；
- 混合类型与混合 ASC/DESC tuple pairwise order；
- GLOBAL_ORDER/HASH golden vectors；
- record target 顺序；
- MVCC newest-first；
- 每一个 truncation point；
- trailing bytes、未知 tag、超范围 bucket；
- property：`Decode(Encode(x)) == x`；
- property：`sign(compare(x,y)) == sign(byte_compare(E(x),E(y)))`；
- property：strict decode 成功则 `Encode(Decode(bytes)) == bytes`。

Golden vectors 是永久格式兼容测试，不得因重构实现而随意更新。确需更新时必须升级 KeyFormat version，并提供迁移与双读策略。
