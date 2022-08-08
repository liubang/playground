## 设计模式 GOF23

将设计者的思维融入大家的学习和工作中，更高层次的思考。

### 分类

**创建型模式：**

- 单例模式
- 工厂模式
- 抽象工厂模式
- 建造者模式
- 原型模式

**结构型模式：**

- 适配器模式
- 桥接模式
- 装饰模式
- 组合模式
- 外观模式
- 享元模式
- 代理模式

**行为型模式：**

- 模板方法模式
- 命令模式
- 迭代器模式
- 观察者模式
- 中介者模式
- 备忘录模式
- 解释器模式
- 状态模式
- 策略模式
- 职责链模式
- 访问者模式

### 单例模式

#### 核心作用

保证一个类只有一个实例，并且提供一个访问该实例的全局访问点。

#### 优点

- 由于单例模式只生成一个实例，减少了系统性能开销，当一个对象的产生需要比较多的资源时，如读取配置、产生其他依赖对象时，则可以通过在应用启动时直接产生一个单例对象，然后永久驻留内存的方式来解决
- 单例模式可以在系统设置全局访问点，优化共享资源的访问，例如可以设计一个单例类，负责所有数据表的映射处理

#### 常见的物种单例模式实现方式

- 主要：
-
    - 饿汉式（线程安全，调用效率高。但是，不能延迟加载）
-
    - 懒汉式（线程安全，调用效率不高。但是可以延迟加载）

- 其他：
-
    - 双重检测锁式
-
    - 静态内部类式（线程安全，调用效率高。可以延迟加载）
-
    - 枚举单例（线程安全，调用效率高。不能延迟加载）

**懒汉式**

[SingletonDemo01.java](./src/main/java/cn/iliubang/exercises/dp/singleton/SingletonDemo01.java)

```java
public class SingletonDemo01 {

    private static SingletonDemo01 instance = new SingletonDemo01();

    /**
     * 私有构造器
     */
    private SingletonDemo01() {
    }


    public static SingletonDemo01 getInstance() {
        return instance;
    }
}
```

**饿汉式**

[SingletonDemo02.java](./src/main/java/cn/iliubang/exercises/dp/singleton/SingletonDemo02.java)

```java
public class SingletonDemo02 {

    private static SingletonDemo02 instance;

    private SingletonDemo02() {
    }

    public static synchronized SingletonDemo02 getInstance() {
        if (null == instance) {
            instance = new SingletonDemo02();
        }
        return instance;
    }
}
```

**双重检测锁式**

[SingletonDemo03.java](./src/main/java/cn/iliubang/exercises/dp/singleton/SingletonDemo03.java)

```java
public class SingletonDemo03 {

    private static volatile SingletonDemo03 instance;

    private SingletonDemo03() {

    }

    public static SingletonDemo03 getInstance() {
        if (null == instance) {
            synchronized (SingletonDemo03.class) {
                if (null == instance) {
                    instance = new SingletonDemo03();
                }
            }
        }
        return instance;
    }
}
```

**静态内部类式**

[SingletonDemo04.java](./src/main/java/cn/iliubang/exercises/dp/singleton/SingletonDemo04.java)

```java
public class SingletonDemo04 {

    private SingletonDemo04() {
    }

    private static class SingletonClassInstance {
        private static final SingletonDemo04 instance = new SingletonDemo04();
    }

    public static SingletonDemo04 getInstance() {
        return SingletonClassInstance.instance;
    }
}
```

**使用枚举实现**

优点：

- 实现简单
- 枚举本身就是单例模式。由JVM从根本上提供保证！避免通过反射和反序列化的漏洞

缺点：

- 无延迟加载

[SingletonDemo05.java](./src/main/java/cn/iliubang/exercises/dp/singleton/SingletonDemo05.java)

```java
public enum SingletonDemo05 {

    /**
     * 定义一个枚举元素，它代表了Singleton的一个实例
     */
    INSTANCE;

    /**
     * 单例可以有自己的操作
     */
    public void op() {
        // 功能
    }
}
```

#### 问题

通过反射可以破解（可以在构造方法中手动抛出异常来控制）

[SingletonDemo06.java](./src/main/java/cn/iliubang/exercises/dp/singleton/SingletonDemo06.java)

[Client01.java](./src/main/java/cn/iliubang/exercises/dp/singleton/Client01.java)

```java
public class Client01 {
    public static void main(String[] args) {
        SingletonDemo06 s1 = SingletonDemo06.getInstance();
        SingletonDemo06 s2 = SingletonDemo06.getInstance();

        System.out.println(s1);
        System.out.println(s2);

        try {
            Class<SingletonDemo06> clazz = (Class<SingletonDemo06>) Class.forName("cn.iliubang.exercises.dp.singleton.SingletonDemo06");
            Constructor<SingletonDemo06> c = clazz.getDeclaredConstructor();
            c.setAccessible(true);
            SingletonDemo06 s3 = c.newInstance();
            SingletonDemo06 s4 = c.newInstance();
            c.setAccessible(false);
            System.out.println(s3);
            System.out.println(s4);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
```

反序列化可以破解 (可以通过定义readResolve()防止获得不同的对象)

[SingletonDemo07.java](./src/main/java/cn/iliubang/exercises/dp/singleton/SingletonDemo07.java)

[Client02.java](./src/main/java/cn/iliubang/exercises/dp/singleton/Client02.java)

```java
public class Client02 {
    public static void main(String[] args) throws Exception {
        SingletonDemo07 s1 = SingletonDemo07.getInstance();
        SingletonDemo07 s2 = SingletonDemo07.getInstance();

        System.out.println(s1);
        System.out.println(s2);

        // 序列化，写入磁盘
        FileOutputStream fos = new FileOutputStream("/tmp/s1.log");
        ObjectOutputStream oos = new ObjectOutputStream(fos);
        oos.writeObject(s1);
        oos.close();

        // 反序列化
        ObjectInputStream ois = new ObjectInputStream(new FileInputStream("/tmp/s1.log"));
        SingletonDemo07 s3 = (SingletonDemo07) ois.readObject();
        System.out.println(s3);
    }
}
```

#### 效率测试

**CountDownLatch**

同步辅助类，在完成一组正在其他线程中执行的操作之前，它允许一个或多个线程一直等待。

`countDown()` 当前线程调用此方法，则计数减一（建议放在finally里执行）
`await()` 调用此方法会一直阻塞当前线程，直到计数器的值为0

[Client03.java](./src/main/java/cn/iliubang/exercises/dp/singleton/Client03.java)

### 工厂模式

实现了创建者和调用者的分离

详细分类：

- 简单工厂模式
- 工厂方法模式
- 抽象工厂模式

**简单工厂模式：**

- 也叫静态工厂模式，就是工厂类一般是使用静态方法，通过接收的参数的不同来返回不同的对象实例。
- 对于增加新产品无能为力！不修改代码的话，是真的没法扩展。

[Client02.java](./src/main/java/cn/iliubang/exercises/dp/factory/simplefactory/Client02.java)

**工厂方法模式：**

- 避免了简单工厂模式中不满足开闭原则的缺点。
- 工厂方法模式和简单工厂模式的区别在于，简单工厂模式只有一个（对于同一个项目或者一个独立模块而言）工厂类，而工厂方法模式有一组实现了相同接口的工厂类。

[Client01.java](./src/main/java/cn/iliubang/exercises/dp/factory/factorymethod/Client01.java)

**抽象工厂模式：**

- 用来生产不同产品族的全部产品。（对于增加新的产品，无能为力；支持增加产品族）
- 抽象工厂模式是工厂方法模式的升级版本，在有多个业务品种、业务分类的时候，通过抽象工厂模式产生需要的对象是一种非常好的解决方式。

[Client01.java](./src/main/java/cn/iliubang/exercises/dp/factory/abstractfactory/Client01.java)

### 建造者模式

- 分离了对象子组件的单独构造（由Builder来负责）和装配（由Director负责）。从而可以构造出复杂的对象。这个模式适用于某个对象的构建过程比较复杂的情况下。
- 由于事先了构建和装配的解耦。不同的构建器，相同的装配，也可以做出不同的对象；相同的构建器，不同的装配也可以做出不同的对象。也就是实现了构建算法和装配算法的解耦和更好的复用。

