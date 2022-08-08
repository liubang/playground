### 简介

Java动态性的两种实现方式：

- 字节码操作
- 反射

运行时操作字节码可以让我们实现如下功能：

- 动态生成新的类
- 动态改变某个类的结构（添加/删除/修改 新的属性/方法）

### 常见的字节码操作类库

#### BCEL (Byte Code Engineering Library)

[https://commons.apache.org/proper/commons-bcel/](https://commons.apache.org/proper/commons-bcel/)

#### ASM

是一个轻量级JAVA字节码操作框架，直接涉及到JVM底层的操作和指令

#### CGLIB (Code Generation Library)

一个强大的，高性能，高质量的Code生成类库，基于ASM实现

#### Javassist

[https://jboss-javassist.github.io/javassist/tutorial/tutorial.html](https://jboss-javassist.github.io/javassist/tutorial/tutorial.html)

使用Javassist创建如下类：

```java
class Emp {
    private int empno;
    private String ename;

    public Emp(int empno, String ename) {
        this.empno = empno;
        this.ename = ename;
    }

    public int getEmpno() {
        return empno;
    }

    public void setEmpno(int empno) {
        this.empno = empno;
    }

    public String getEname() {
        return ename;
    }

    public void setEname(String ename) {
        this.ename = ename;
    }
}
```

```java
public static void main(String[] args) throws Exception {
    ClassPool classPool = ClassPool.getDefault();
    //创建类
    CtClass ctClass = classPool.makeClass("cn.iliubang.exercises.primary.javassist.some.Emp");
    //创建属性
    CtField empno = CtField.make("private int empno;", ctClass);
    CtField ename = CtField.make("private String ename;", ctClass);
    ctClass.addField(empno);
    ctClass.addField(ename);

    //创建方法
    CtMethod getEmpno = CtMethod.make("public int getEmpno() {\n" +
            "        return empno;\n" +
            "    }", ctClass);
    CtMethod setEmpno = CtMethod.make("public void setEmpno(int empno) {\n" +
            "        this.empno = empno;\n" +
            "    }", ctClass);
    CtMethod getEname = CtMethod.make("public String getEname() {\n" +
            "        return ename;\n" +
            "    }", ctClass);
    CtMethod setEname = CtMethod.make("public void setEname(String ename) {\n" +
            "        this.ename = ename;\n" +
            "    }", ctClass);

    ctClass.addMethod(getEmpno);
    ctClass.addMethod(setEmpno);
    ctClass.addMethod(getEname);
    ctClass.addMethod(setEname);

    //创建构造器
    CtConstructor ctConstructor = new CtConstructor(new CtClass[]{CtClass.intType, classPool.get("java.lang.String")}, ctClass);
    ctConstructor.setBody("{\n" +
            "        this.empno = $1;\n" +
            "        this.ename = $2;\n" +
            "    }");
    ctClass.addConstructor(ctConstructor);

    //将构建好的类写入到文件中，生成编译好的字节码文件
    ctClass.writeFile("/Users/liubang/workspace/java/exercises/primary/target/classes");
}
```

操作类：

```java
ClassPool classPool = ClassPool.getDefault();
//获取类
CtClass ctClass = classPool.get("cn.iliubang.exercises.primary.javassist.some.Emp");
//获取字节码
byte[] bytes = ctClass.toBytecode();
System.out.println(Arrays.toString(bytes));
//获取类名
System.out.println(ctClass.getName());
System.out.println(ctClass.getSimpleName());
//获取父类
System.out.println(ctClass.getSuperclass());
```

创建新方法：

```java
ClassPool classPool = ClassPool.getDefault();
CtClass ctClass = classPool.get("cn.iliubang.exercises.primary.javassist.some.Emp");

//CtMethod add = CtNewMethod.make("public int add(int a, int b) { return a + b; }", ctClass);
//或者
//声明方法
CtMethod add = new CtMethod(CtClass.intType, "add", new CtClass[]{CtClass.intType, CtClass.intType}, ctClass);
add.setModifiers(Modifier.PUBLIC);
add.setBody("{System.out.println(\"这是构造的方法\"); return $1 + $2;}");
ctClass.addMethod(add);
//通过反射调用生成的方法
Class clazz = ctClass.toClass();
Object obj = clazz.newInstance();
Method method = clazz.getDeclaredMethod("add", int.class, int.class);
Object result = method.invoke(obj, 12, 34);
System.out.println("result:" + result);
```

修改已有的方法：

```java
ClassPool classPool = ClassPool.getDefault();
CtClass ctClass = classPool.get("cn.iliubang.exercises.primary.javassist.some.Emp");
//获取已有的方法
CtMethod ctMethod = ctClass.getDeclaredMethod("getEname", new CtClass[]{});
ctMethod.insertBefore("System.out.println(\"测试修改方法\");");

//通过反射调用生成的方法
Class clazz = ctClass.toClass();
Object obj = clazz.newInstance();
Method method = clazz.getDeclaredMethod("getEname");
Object result = method.invoke(obj);
System.out.println("result:" + result);
```

**参考文档**

- [用 BCEL 设计字节码](https://www.ibm.com/developerworks/cn/java/j-dyn0414/index.html)
- [Javassist 使用指南](https://www.jianshu.com/p/43424242846b)
- [用 Javassist 进行类转换](https://www.ibm.com/developerworks/cn/java/j-dyn0916/)
- [JavaSsist的使用](https://github.com/bingbo/blog/wiki/JavaSsist%E7%9A%84%E4%BD%BF%E7%94%A8)


