### File

两个常量：

1. 路径分隔符：`File.pathSeparator`
2. 文件分隔符：`File.separator`

```java
public static void main(String[] args) {
    System.out.println(File.pathSeparator);
    System.out.println(File.separator);

    // 路径的几种表示形式
    String path = "/tmp/Demo.java";
    path = File.separator + "tmp" + File.separator + "Demo.java";

    File src = new File("/tmp/Demo.java");
    src = new File("/tmp/", "Demo.java");
    System.out.println(src.getName());
    System.out.println(src.getPath());
}
```

### IO流的原理

**概念**

流：流动、流向，从一端移动到另一端 源头与目的地
程序与文件|数组|网络连接|数据库，以程序为中心

**分类**

流向：

- 输入流
- 输出流

数据：

- 字节流：二进制流，可以处理一切文件
- 字符流：只能处理纯文本

功能：

- 节点流：包裹源头的
- 处理流：增强功能，提高性能

**字符流与字节流**

字节流：

- 输入流：InputStream read(byte[] b), read(byte[] b, int off, int len), close()
-
    - FileInputStream

- 输出流：OutputStream write(byte[]), write(byte[]b, int off, int len), flush(), close()
-
    - FileOutputStream

字符流：

- 输入流：Reader read(char[] cbuf), read(char[] cbuf, int off, int len), flush(), close()
-
    - FileReader

- 输出流：Writer write(char[] cbuf) write(char[] cbuf, int off, int len), flush(), close()
-
    - FileWriter

**操作**

举例：搬家

```text
读取文件
关联房子  -> 建立与文件的联系  
选择房子  -> 选择对应的流  
搬家     -> 读取、写出  
卡车大小  -> 数组大小  
运输      -> 读取、写出  
打发over  -> 释放资源  

```
