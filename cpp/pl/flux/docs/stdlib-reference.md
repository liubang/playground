# Flux Stdlib Reference

Universe builtin 和内置包的完整 API 参考。

## Universe Builtins

Universe builtin 默认注入，无需 `import`。

### 基础函数

| 函数                     | 说明                              |
| ------------------------ | --------------------------------- |
| `len(v)`                 | 返回 string、array、object 的长度 |
| `string(v)`              | 将值转成 Flux 风格字符串          |
| `contains(set:, value:)` | 判断数组中是否存在值              |

### 表变换

| 函数                                       | 说明                                           |
| ------------------------------------------ | ---------------------------------------------- |
| `range(start:, stop:)`                     | 按 `_time` 过滤时间范围                        |
| `filter(fn:, onEmpty:)`                    | 按谓词过滤行，`onEmpty: "keep"` 可保留空表形状 |
| `map(fn:)`                                 | 对每行做对象映射                               |
| `limit(n:, offset:)`                       | 取前 `n` 行                                    |
| `tail(n:, offset:)`                        | 取后 `n` 行                                    |
| `keep(columns:)`                           | 只保留指定列                                   |
| `drop(columns:)`                           | 删除指定列                                     |
| `rename(columns:)`                         | 重命名列                                       |
| `duplicate(column:, as:)`                  | 复制列                                         |
| `set(key:, value:)`                        | 给每行写入固定列值                             |
| `sort(columns:, desc:)`                    | 按列排序                                       |
| `group(columns:)`                          | 生成逻辑分组                                   |
| `pivot(rowKey:, columnKey:, valueColumn:)` | 透视行列                                       |
| `fill(column:, value:, usePrevious:)`      | 填充空值                                       |
| `union(tables:)`                           | 合并多个表流                                   |

### 聚合、选择器与排名

| 函数                     | 说明                                |
| ------------------------ | ----------------------------------- |
| `sum(arr)`               | 数组求和                            |
| `mean(arr)`              | 数组均值                            |
| `min(arr)`               | 数组最小值                          |
| `max(arr)`               | 数组最大值                          |
| `count(column:)`         | 按表或分组计数                      |
| `spread(column:)`        | 计算最大值与最小值差                |
| `quantile(q:, column:)`  | 计算分位数，`q` 支持单值或数组      |
| `median(column:)`        | 中位数，等价于 `q = 0.5` 的常用路径 |
| `first()`                | 每个表/分组第一行                   |
| `last()`                 | 每个表/分组最后一行                 |
| `top(n:, columns:)`      | 取最大 `n` 行                       |
| `bottom(n:, columns:)`   | 取最小 `n` 行                       |
| `reduce(identity:, fn:)` | 按行折叠成对象                      |
| `distinct(column:)`      | 取列去重结果                        |

### 窗口与序列函数

| 函数                                                                                          | 说明                                 |
| --------------------------------------------------------------------------------------------- | ------------------------------------ |
| `window(every:, period:, offset:, createEmpty:)`                                              | 按时间窗口重组表流                   |
| `aggregateWindow(every:, fn:, period:, offset:, location:, timeSrc:, timeDst:, createEmpty:)` | 窗口聚合，支持固定时长和部分日历窗口 |
| `elapsed(unit:)`                                                                              | 计算相邻行时间差                     |
| `difference(columns:, nonNegative:, keepFirst:)`                                              | 计算相邻行数值差                     |
| `derivative(unit:, nonNegative:, initialZero:)`                                               | 计算变化率                           |

### Join、检查与输出

| 函数                          | 说明                                             |
| ----------------------------- | ------------------------------------------------ |
| `join(tables:, on:, method:)` | 顶层 join，支持 `inner`、`left`、`right`、`full` |
| `columns()`                   | 返回表列名                                       |
| `keys()`                      | 返回 group key 列名                              |
| `findColumn(fn:, column:)`    | 找到匹配行并返回某列数组                         |
| `findRecord(fn:, idx:)`       | 找到匹配行并返回指定位置的 record                |
| `explain()`                   | 返回 logical / optimized logical / physical / pipeline plan；`json: true` 可返回结构化 DAG，`graph: true` 可返回 Mermaid 图 |
| `yield(name:)`                | 设置结果名并输出表流                             |

## 内置包

### `array`

| 函数                                 | 说明               |
| ------------------------------------ | ------------------ |
| `array.from(rows:, bucket:)`         | 从对象数组构造内联内存表 |
| `array.concat(arr:, v:)`             | 拼接数组           |
| `array.filter(arr:, fn:)`            | 过滤数组元素       |
| `array.map(arr:, fn:)`               | 映射数组元素       |
| `array.contains(arr:, value:)`       | 判断数组是否包含值 |
| `array.reduce(arr:, identity:, fn:)` | 折叠数组           |
| `array.any(arr:, fn:)`               | 任一元素满足谓词   |
| `array.all(arr:, fn:)`               | 全部元素满足谓词   |
| `array.range(start:, stop:, step:)`  | 生成整数序列       |
| `array.repeat(value:, n:)`           | 重复生成值         |
| `array.length(arr:)`                 | 数组长度           |
| `array.get(arr:, index:, default:)`  | 安全索引访问       |
| `array.slice(arr:, start:, end:)`    | 切片               |
| `array.sort(arr:, desc:)`            | 标量数组排序       |
| `array.flatMap(arr:, fn:)`           | 映射后拍平         |
| `array.find(arr:, fn:, default:)`    | 查找首个匹配元素   |
| `array.findIndex(arr:, fn:)`         | 查找首个匹配索引   |
| `array.take(arr:, n:)`               | 取前 N 个元素      |
| `array.drop(arr:, n:)`               | 跳过前 N 个元素    |
| `array.reverse(arr:)`                | 反转数组           |
| `array.unique(arr:)`                 | 去重               |
| `array.unfold(seed:, fn:, limit:)`   | 按状态展开数组     |
| `array.scan(arr:, identity:, fn:)`   | 保留每步折叠结果   |
| `array.zip(left:, right:)`           | 配对两个数组       |
| `array.enumerate(arr:)`              | 添加元素索引       |

### `csv`

| 函数                           | 说明                                          |
| ------------------------------ | --------------------------------------------- |
| `csv.from(csv:, file:, mode:)` | 从 raw CSV 或 annotated CSV 字符串/文件构造表 |

`mode: "raw"` 解析普通表头 CSV；annotated CSV 支持 `#datatype`、`#group`、`#default` 和 result/table 列的常见形态。

### `sqlite`

| 函数                              | 说明                      |
| --------------------------------- | ------------------------- |
| `sqlite.from(path:, table:)`      | 从外部表扫描物化为 Flux 表流 |

`sqlite.from` 通过 SQLite C API 扫描 `path` 指向的数据库表，并将 `null`、integer、float、text、blob-as-string 映射到 Flux 运行时值。用户入口不提供 `query` 模式，SQL 只作为 SQLite connector 内部 physical plan。rowid multi-split 的边界值使用参数绑定，split 之间复用稳定 SQL text。

### `mysql`

| 函数                       | 说明                 |
| -------------------------- | -------------------- |
| `mysql.from(dsn:, table:)` | 从 MySQL 表扫描物化为 Flux 表流 |
| `mysql.from(host:, user:, password:, database:, table:, ?port:)` | 同上，使用显式连接字段 |

`mysql.from` 使用 Boost.MySQL 连接外部 MySQL，支持 `mysql://user:password@host[:port]/database` 和 `user:password@tcp(host[:port])/database` 两类 `dsn`，也支持显式 `host/user/password/database/port` 字段；`port` 默认 3306。用户入口不提供 raw `query` 模式，SQL 只作为 connector 内部 physical plan。默认扫描走 server-side prepared statement，range split 与 pushdown predicate 统一使用参数绑定。

### `date`

| 函数                       | 说明            |
| -------------------------- | --------------- |
| `date.add(d:, to:)`        | 时间加 duration |
| `date.sub(d:, from:)`      | 时间减 duration |
| `date.truncate(t:, unit:)` | 截断到指定单位  |
| `date.year(t:)`            | 年              |
| `date.month(t:)`           | 月              |
| `date.monthDay(t:)`        | 月内日期        |
| `date.weekDay(t:)`         | 星期            |
| `date.hour(t:)`            | 小时            |
| `date.minute(t:)`          | 分钟            |
| `date.second(t:)`          | 秒              |

### `dict`

| 函数                               | 说明                           |
| ---------------------------------- | ------------------------------ |
| `dict.fromList(pairs:)`            | 从 `{key, value}` 数组构造字典 |
| `dict.get(dict:, key:, default:)`  | 读取 key，缺失时返回默认值     |
| `dict.insert(dict:, key:, value:)` | 返回插入/覆盖后的字典          |
| `dict.remove(dict:, key:)`         | 返回删除 key 后的字典          |

当前还没有独立 dict runtime type，字典由 object 承载；非 string key 会按运行时字符串化结果保存。

### `join`

| 函数                                 | 说明                             |
| ------------------------------------ | -------------------------------- |
| `join.inner(left:, right:, on:)`     | 内连接                           |
| `join.left(left:, right:, on:, as:)` | 左连接，可用 predicate/as lambda |
| `join.right(left:, right:, on:)`     | 右连接                           |
| `join.full(left:, right:, on:)`      | 全连接                           |

`join` package 是顶层 `join()` 的 facade。`on` 可以是 predicate 函数，也可以是列名数组；列名数组路径下可用 `leftName` / `rightName` 控制重名列输出后缀。两侧输入都有 lazy plan 时，列名数组 join 会保留 federated logical plan，由 CBO 根据 connector 行数、列基数和 top-K 高频值选择 gather、双侧 hash repartition、小 build side broadcast 或大 build side salted repartition。salted 路径处理单列等值 join，只复制热点 build rows，并 round-robin 分散对应 probe rows；execution profile 会列出 distribution、heavy hitters 和各 partition 的 rows/bytes。predicate 和自定义 `as` 路径仍走 eager fallback。

### `json`

| 函数              | 说明                       |
| ----------------- | -------------------------- |
| `json.encode(v:)` | 编码 Flux 值为 JSON 字符串 |

当前运行时没有 bytes 类型，所以 `json.encode` 先返回 string。后续如果增加 `json.decode` 或 bytes 值，应该直接复用 simdjson，而不是继续扩展手写 JSON 逻辑。

### `math`

| 函数               | 说明       |
| ------------------ | ---------- |
| `math.pi`          | 圆周率常量 |
| `math.abs(x:)`     | 绝对值     |
| `math.ceil(x:)`    | 向上取整   |
| `math.floor(x:)`   | 向下取整   |
| `math.round(x:)`   | 四舍五入   |
| `math.sqrt(x:)`    | 平方根     |
| `math.pow(x:, y:)` | 幂         |

### `regexp`

| 函数                               | 说明                 |
| ---------------------------------- | -------------------- |
| `regexp.compile(v:)`               | 编译正则字符串       |
| `regexp.findString(r:, v:)`        | 返回第一个匹配字符串 |
| `regexp.matchRegexpString(r:, v:)` | 判断是否匹配         |
| `regexp.quoteMeta(v:)`             | 转义正则元字符       |

### `runtime`

| 函数                | 说明                                   |
| ------------------- | -------------------------------------- |
| `runtime.version()` | 返回当前 playground runtime 版本字符串 |

### `strings`

| 函数                               | 说明                   |
| ---------------------------------- | ---------------------- |
| `strings.containsStr(v:, substr:)` | 是否包含子串           |
| `strings.hasPrefix(v:, prefix:)`   | 是否有前缀             |
| `strings.hasSuffix(v:, suffix:)`   | 是否有后缀             |
| `strings.joinStr(arr:, v:)`        | 用分隔符拼接字符串数组 |
| `strings.replaceAll(v:, t:, u:)`   | 替换全部子串           |
| `strings.split(v:, t:)`            | 分割字符串             |
| `strings.toUpper(v:)`              | 转大写                 |
| `strings.toLower(v:)`              | 转小写                 |
| `strings.trimSpace(v:)`            | 去掉首尾空白           |

### `system`

| 函数            | 说明              |
| --------------- | ----------------- |
| `system.time()` | 返回当前 UTC 时间 |

`system.time()` 是唯一一个 conformance 中用正则匹配的包函数，因为输出随执行时间变化。

### `timezone`

| 函数                         | 说明                     |
| ---------------------------- | ------------------------ |
| `timezone.utc`               | 默认 UTC location 记录   |
| `timezone.fixed(offset:)`    | 生成固定偏移 location    |
| `timezone.location(name:)`   | 生成命名时区 location    |

`timezone` 返回的 location 记录可直接传给 `window(location:)`、`aggregateWindow(location:)`，也可用于 `option location = ...`。

### `types`

| 函数                      | 说明                      |
| ------------------------- | ------------------------- |
| `types.isNumeric(v:)`     | 是否为 int、uint 或 float |
| `types.isType(v:, type:)` | 按类型名判断              |
| `types.isString(v:)`      | 是否为 string             |
| `types.isDuration(v:)`    | 是否为 duration           |
| `types.isBool(v:)`        | 是否为 bool               |
| `types.isInt(v:)`         | 是否为 int                |
| `types.isUInt(v:)`        | 是否为 uint               |
| `types.isFloat(v:)`       | 是否为 float              |
| `types.isTime(v:)`        | 是否为 time               |
| `types.isRegexp(v:)`      | 是否为 regexp             |

这里没有严格照搬官方 `types` 包的最小 API，而是按当前 runtime 值模型扩展了一批直接可用的 `isXxx` helper，方便样例和后续包实现做类型分派。
