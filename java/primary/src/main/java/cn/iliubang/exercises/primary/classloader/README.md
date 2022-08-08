### 类加载全过程

**为什么要研究类加载过程**

- 有助于了解JVM运行过程
- 更深入了解Java动态性，提高程序灵活性

#### 类加载机制

JVM把class文件加载到内存，并对数据进行校验、解析和初始化，最终形成JVM可以直接使用的Java类型的过程。

类从被加载到虚拟机内存中开始，到卸载出内存为止，它的整个生命周期包括：加载、验证、准备、解析、初始化、使用和卸载七个阶段。它们开始的顺序如下图所示：

![classload](classonload.png)

加载：

- 通过一个类的全限定名来获取其定义的二进制字节流。
- 将这个字节流所代表的静态存储结构转化为方法区的运行时数据结构。
- 在 Java 堆中生成一个代表这个类的 java.lang.Class 对象，作为对方法区中这些数据的访问入口。

站在 Java 虚拟机的角度来讲，只存在两种不同的类加载器：

- 启动类加载器：它使用 C++ 实现（这里仅限于 Hotspot，也就是 JDK1.5 之后默认的虚拟机，有很多其他的虚拟机是用 Java
  语言实现的），是虚拟机自身的一部分。
- 所有其他的类加载器：这些类加载器都由 Java 语言实现，独立于虚拟机之外，并且全部继承自抽象类
  java.lang.ClassLoader，这些类加载器需要由启动类加载器加载到内存中之后才能去加载其他的类。

站在 Java 开发人员的角度来看，类加载器可以大致划分为以下三类：

- 启动类加载器：Bootstrap ClassLoader，跟上面相同。它负责加载存放在JDK\jre\li(JDK 代表 JDK 的安装目录，下同)
  下，或被-Xbootclasspath参数指定的路径中的，并且能被虚拟机识别的类库（如 rt.jar，所有的java.*开头的类均被 Bootstrap
  ClassLoader 加载）。启动类加载器是无法被 Java 程序直接引用的。
- 扩展类加载器：Extension ClassLoader，该加载器由sun.misc.Launcher$ExtClassLoader实现，它负责加载JDK\jre\lib\ext目录中，或者由
  java.ext.dirs 系统变量指定的路径中的所有类库（如javax.*开头的类），开发者可以直接使用扩展类加载器。
- 应用程序类加载器：Application ClassLoader，该类加载器由 sun.misc.Launcher$AppClassLoader
  来实现，它负责加载用户类路径（ClassPath）所指定的类，开发者可以直接使用该类加载器，如果应用程序中没有自定义过自己的类加载器，一般情况下这个就是程序中默认的类加载器。

链接：

- 验证：确保加载的类信息符合JVM规范，没有安全方面的问题
- 准备：正式为类变量（static变量）分配内存并设置变量初始阶段，这些内存都将在方法区中进行分配
- 解析：虚拟机常量池中的符号引用替换为直接引用的过程

初始化：

- 初始化阶段是执行类构造器<clinit>()方法的过程。类构造器<clinit>()方法是由编译器自动收集类中所有类变量的赋值动作和静态语句块（static代码块）中的语句合并产生的。
- 当初始化一个类的时候，如果发现其父类还没有进行初始化，则需要先对其父类进行初始化
- 虚拟机会保证一个类的<clinit>()方法在多线程环境中被正确加锁和同步
- 当访问一个Java类的静态域时，只有真正声明这个域的类才会被初始化

类的主动引用（一定会发生类的初始化）

- new一个类的对象
- 调用类的静态成员（除了final修饰的常量）和静态方法
- 使用java.lang.reflect包的方法对类进行反射调用
- 当虚拟机启动，先启动main方法所在的类
- 当初始化一个类，如果其父类没有初始化，则先初始化父类

类的被动引用（不会发生类的初始化）

- 当访问一个静态域时，只有真正声明这个域的类才会被初始化，通过子类引用父类的静态变量，不会导致子类的初始化
- 通过数组定义类引用，不会触发此类的初始化
- 引用常量不会触发此类的初始化（常量在编译阶段就存入调用类的常量池中了）

```java
class Foo {

    static int age;

    static {
        System.out.println("Foo 静态初始化代码块");
        age = 24;
    }

    public Foo() {
        System.out.println("构造方法");
    }
}

class Bar extends Foo {
    static {
        System.out.println("Bar 静态初始化代码块");
    }
}

public class Demo {
    public static void main(String[] args) {
//        Foo a = new Foo();
//        System.out.println(Foo.age);
        System.out.println(Bar.age);
    }
}

// output
Foo 静态初始化代码块
24
```

#### ClassLoader的作用

java.lang.ClassLoader类的基本职责就是根据一个指定的类的名称，找到或者生成其对应的字节码代码，然后从这些字节码代码中定义出一个Java类，即java.lang.Class类的一个实例。除此之外
ClassLoader还负责加载Java应用所需要的资源，如图像，文件和配置文件等。

![](loader.png)

```java
public class Demo2 {
    public static void main(String[] args) {
        System.out.println(ClassLoader.getSystemClassLoader());
        System.out.println(ClassLoader.getSystemClassLoader().getParent());
        System.out.println(ClassLoader.getSystemClassLoader().getParent().getParent());
    }
}
```

#### 类加载器的代理模式

**代理模式**

交给其他加载器来加载指定的类

**双亲委托机制**

就是某个特定的类加载器在接到加载类的请求时，首先将加载任务委托给父加载器，依次追溯，直到最高的爷爷辈，如果父类加载器能够完成加载任务
就成功返回；只有父类加载器无法完成此加载任务的时候，才会自己去加载。

双亲委托机制是为了保证Java核心库的类型安全。这种机制能保证不会出现用户自己定义java.lang.Object类的情况。

类加载器除了用于加载类，也是安全的最基本的屏障。

双亲委托机制是代理模式的一种，并不是所有的类加载器都采用双亲委托机制。tomcat服务器类加载器也使用代理模式，所不同的是它首先尝试去加载某个类
，如果找不到再代理给父类加载器。这与一般加载器顺序是相反的。

#### 自定义实现类加载器

**自定义类加载器的流程**

- 继承java.lang.ClassLoader
- 首先检查请求的类型是否已经被这个类加载器加载到命名空间，如果已经加载，则直接返回
- 委派类加载请求给父类加载器，如果父类加载器能够完成，则返回父类加载器加载的Class实例
- 调用本类加载器的findClass方法，试图获取对应的字节码，如果获取到，则调用defineClass导入类型到方法区；如果获取不到对应的字节码
  或者其他原因失败，返回异常给loadClass，loadClass转抛异常，终止加载过程

```java
package cn.iliubang.exercises.primary.classloader.some;

import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/2
 */
public class MyClassLoader extends ClassLoader {

    /**
     * class path
     */
    private String classPath;

    public MyClassLoader(String classPath) {
        this.classPath = classPath;
    }

    @Override
    protected Class<?> findClass(String name) throws ClassNotFoundException {
        // 先查找是否加载过
        Class<?> clazz = findLoadedClass(name);
        // 如果已经加载过，则直接返回查找到的Class实例
        if (null != clazz) {
            return clazz;
        }
        // 父类加载器
        ClassLoader parent = getParent();
        try {
            clazz = parent.loadClass(name);
            if (null != clazz) {
                return clazz;
            }
        } catch (ClassNotFoundException e) {

        }
        // 只能自己干了
        byte[] bytes = getClassData(name);
        if (bytes == null) {
            throw new ClassNotFoundException();
        } else {
            clazz = defineClass(name, bytes, 0, bytes.length);
            return clazz;
        }
    }

    private byte[] getClassData(String name) {
        String path = classPath + "/" + name.replace('.', '/') + ".class";
        InputStream inputStream = null;
        ByteArrayOutputStream byteArrayOutputStream = new ByteArrayOutputStream();
        try {
            inputStream = new FileInputStream(path);
            byte[] buffer = new byte[1024];
            int temp = 0;
            while ((temp = inputStream.read(buffer)) != -1) {
                byteArrayOutputStream.write(buffer, 0, temp);
            }
            return byteArrayOutputStream.toByteArray();
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        } finally {
            try {
                if (null != inputStream)
                    inputStream.close();
                byteArrayOutputStream.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
        }
    }

}
```

测试

```java
package cn.iliubang.exercises.primary.classloader.some;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/2
 */
public class TestMyClassLoader {

    public static void main(String[] args) throws Exception{
        MyClassLoader classLoader = new MyClassLoader("/tmp");
        Class clazz = classLoader.loadClass("Demo");
        System.out.println(clazz);
    }

}
```

**参考文档**

- [深入探讨 Java 类加载器](https://www.ibm.com/developerworks/cn/java/j-lo-classloader/index.html)
- [类加载机制](http://wiki.jikexueyuan.com/project/java-vm/class-loading-mechanism.html)