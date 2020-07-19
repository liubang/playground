package cn.iliubang.exercises.jvm.classloader;

/**
 * 对于静态字段磊说，只有直接定义了该字段的类才会被初始化
 * -XX:+TraceClassLoading，用于追踪类的加载信息并打印出来
 *
 * -XX:+<option>, 表示开启option选项
 * -XX:-<option>, 表示关闭option选项
 * -XX:<option>=<value>, 表示将option选项的值设置为value
 */
public class MyTest1 {
    public static void main(String[] args) {
        System.out.println(MyChild1.str);
    }
}

class MyParent1 {
    public static String str = "hello world";

    static {
        System.out.println("MyParent1 static block");
    }
}

class MyChild1 extends MyParent1 {
    static {
        System.out.println("MyChild1 static block");
    }
}