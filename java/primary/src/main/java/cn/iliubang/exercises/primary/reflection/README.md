**Java的动态性让编程的时候更加灵活**

### 反射机制

指的是可以于运行时加载、探知、使用编译期间完成的未知的类。

程序在运行状态中，可以动态加载一个只有名称的类，对于任意一个已加载的类，都能够知道这个类的所有属性和方法；对于任意一个对象，都能够调用它的
任意一个方法和属性；

```java
Class clazz = Class.forName("xx.xx.xxx");
```

加载完类之后，在堆内存中就产生了一个`Class`类型的对象（一个类，只有一个`Class`对象），这个对象就包含了完整的类的结构信息。
我们可以通过这个对象看到类的结构。这个对象就像一面镜子，透过这个镜子看到类的结构，所以我们形象称之为：反射。

### Class对象如何获取

- 运用`someobj.getClass()`
- 运用`Class.forName()`
- 运用`someclass.class`

```java
Class clazz = Class.forName("cn.iliubang.exercises.primary.reflection.some.Demo1");
System.out.println(clazz);
System.out.println(Demo1.class);
System.out.println((new Demo1()).getClass());
```

### 反射机制性功问题

- setAccesible: 启用和禁用访问安全检查的开关，值为true则表示反射的对象在使用时应该取消Java语言访问检查。禁止安全检查，可以提高反射的运行速度。

- 可以考虑使用cglib/javaassist字节码操作