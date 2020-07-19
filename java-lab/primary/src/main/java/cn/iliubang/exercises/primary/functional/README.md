## Lambda表达式

## 流

### 从外部迭代到内部迭代

Java程序员在使用集合类时，一个通用的模式是在集合上进行迭代，然后处理返回的每一个元素。
比如要计算从伦敦来的艺术家的人数，通常代码会写成下面这样：

```java
int count = 0;
for (Artist artist : allArtists) {
    if (artist.isFrom("London")) {
        count++;
    }
}
```

尽管这样的操作可行，但是存在几个问题。每次迭代集合类时，都需要写很多样板代码。将for循环改造成并行方式也很麻烦，
需要修改每个for循环才能实现。此外，上述代码无法流畅传达程序员的意图。for循环的样板代码模糊了代码的本意，程序员
必须阅读整个循环体才能理解。若是单一的for循环，倒也问题不大，但是面对一个满是循环的庞大代码库时，负担就重了。

就其背后的原理来看，for循环其实就是一个封装了迭代的语法糖，我们再这里多花点时间，看看它的工作原理。首先调用iterator方法，
产生一个新的Iterator对象，进而控制整个迭代过程，这就是外部迭代。迭代过程通过显示调用Iterator对象的hasNext和next方法进行：

```java
int count = 0;
Iterator<Artist> iterator = allArtists.iterator();
while(iterator.hasNext()) {
    Artist artist = iterator.next();
    if (artist.isFrom("London")) {
        count++;
    }
}
```

然而外部迭代也有问题。首先，它很难抽象出稍后提及的不同操作；此外，它从本质上来讲就是一种串行化操作。总体来看，
使用for循环会将行为和方法混为一谈。
另一种方法就是内部迭代。首先要注意stream()方法的调用，它和上例iterator()的作用一样。该方法返回的不是一个控制迭代的Iterator，
对象，而是返回内部迭代中的相应接口：Stream

```java
long count = allArtists.stream()
                       .fileter(artist -> artist.isFrom("London"))
                       .count();
```

### 常用的流操作

为了更好的理解Stream API，掌握一些常用的Stream操作十分必要。

#### collect(toList())

> collect(toList())方法由Stream里的值生成一个列表，是一个及早求值操作。

Stream的of方法使用一组初始值生成新的Stream。事实上，collect的用法不仅限于此，它是一个非常通用的强大结构。下面是一个collect方法的例子：

```java
List<String> collected = Stream.of("a", "b", "c").collect(Collectors.toList());
assertEquals(Arrays.asList("a", "b", "c"), collected);
```

这段程序展示了如何使用collect(toList())方法从Stream中生成一个列表。由于很多Stream操作都是惰性求值，因此调用Stream上一系列方法之后，
还需要最后再调用一个类似collect的及早求值的方法。

#### map

> 如果有一个函数可以将一种类型的值转换成另一种类型，map操作就可以使用该函数，将一个流中的值转换成一个新的流。

读者可能已经注意到，以前编程时或多或少使用过类似map的操作。比如编写一段Java代码，将一组字符串转换成对应的大写形式。在一个循环中，对每个字符串调用toUppercase
方法，然后将得到的结果加入到一个新的列表：

```java
List<String> collected = new ArrayList();
for (String string : asList("a", "b", "hello")) {
    String uppercaseString = string.toUpperCase();
    collected.add(uppercaseString);
}

assertEquals(asList("A", "B", "HELLO"), collected);
```

如果你经常使用这样的for循环，就不难猜出map是Stream上最常用的操作之一。

map操作：

```java
List<String> collected = Stream.of("a", "b", "hello").map(String::toUpperCase).collect(toList());

assertEquals(asList("A", "B", "HELLO"), collected);
```

#### filter

> 遍历数据并检查其中的元素时，可尝试使用Stream中提供的新方法filter

#### flatMap

> flatMap可以用于Stream替换值，然后将多个Stream连接成一个Stream

我们来看一个简单的例子。假设有一个包含了多个列表的流，现在希望得到所有数字的序列。

```java
List<Integer> togetter = Stream.of(asList(1, 2), asList(3, 4)).flatMap(numbers -> numbers.stream()).collect(toList());

assertEquals(asList(1, 2, 3, 4), togetter);
```

调用stream方法，将每个列表转换成Stream对象，其余部分由flatMap方法处理。flatMap方法的相关函数接口和map方法的一样，都是Function接口，
只是方法的返回值限定为Stream类型罢了。

#### max和min

Stream上常用的操作之一就是求最大值和最小值。Stream API中的max和min操作足以解决这一问题。

```java
List<Track> tracks = asList(new Track("Bakai", 524), new Track("Violets for Your Furs", 378), new Track("Time Was", 451));

Track shortestTrack = tracks.stream().min(Comparator.comparing(track -> track.getLength())).get();

assertEquals(tracks.get(1), shortestTrack);
```

#### reduce

reduce操作可以实现从一组值中生成一个值。在上述例子中，count，min和max方法，因为常用而被纳入标准库中。事实上这些方法都是reduce操作。

下面是使用reduce操作实现累加：

```java
int count = Stream.of(1,2,3).reduce(0, (acc, element) -> acc + element);

assertEquals(6, count);
```

Lambda表达式的返回值是最新的acc，是上一轮acc的值和当前元素相加的结果。

展开reduce操作

```java
BinaryOperator<Integer> accumulator = (acc, element) -> acc + element;
int count = accumulator.apply(accumulator.apply(accumulator.apply(0, 1), 2), 3);
```
