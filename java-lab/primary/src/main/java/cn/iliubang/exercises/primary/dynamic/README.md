**Java 6.0引入了动态编译机制**

### 动态编译应用场景

- 可以做一个浏览器端编写JAVA代码，上传服务器编译和运行的在线评测系统
- 服务器动态加载某些类文件进行编译

### 动态编译的两种做法

- 通过Runtime调用javac，启动新的进程去操作

```java
Runtime run = Runtime.getRuntime();
Process process = run.exec("javac -cp /xxx/xxx/ Demo.java");
```

- 通过JavaCompiler动态编译

```java
JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
int result = compiler.run(null, null, null, "/tmp/Demo.java");
if (result == 0) {
    System.out.println("编译成功");
} else {
    System.out.println("编译失败");
}
```

### 动态运行编译好的类

- 通过Runtime.getRuntime()运行启动新的进程

```java
try {
    Runtime runtime = Runtime.getRuntime();
    Process process = runtime.exec("java -cp /tmp Demo");
    InputStream inputStream = process.getInputStream();
    BufferedReader bufferedReader = new BufferedReader(new InputStreamReader(inputStream));
    String info = "";
    while ((info = bufferedReader.readLine()) != null) {
        System.out.println(info);
    }
} catch (Exception e) {
    e.printStackTrace();
}
```

- 通过反射运行编译好的类

```java
try {
    URL[] urls = new URL[] {new URL("file:///tmp/")};
    URLClassLoader loader = new URLClassLoader(urls);
    Class clazz = loader.loadClass("Demo");
    //调用加载类的main方法
    Method method = clazz.getMethod("main", String[].class);
    method.setAccessible(true);
    method.invoke(null, (Object)new String[]{});
} catch (Exception e) {
    e.printStackTrace();
}
```


[完整示例代码](some/Demo.java)