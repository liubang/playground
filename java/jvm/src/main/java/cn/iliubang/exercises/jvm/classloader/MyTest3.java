package cn.iliubang.exercises.jvm.classloader;

/**
 * 常量在编译阶段会存入到调用这个常量的方法所在的类的常量池中。
 * 本质上，调用类并没有直接引用到定义常量的类，因此并不会触发定义常量的类的初始化。
 * 注意：这里指的是将常量存储到了MyTest3的常量池中，之后MyTest3跟Demo1根本就没有关系了。
 * 甚至我们可以将Demo1的class文件删除，程序都能直接运行。
 * <p>
 * 助记符：
 * ldc: 表示将int,float或String类型的常量值从常量池中推送至栈顶。
 * bipush: 表示将单字节[-128, 127]的常量值推送至栈顶。
 * sipush: 表示将一个短整型常量值[-32768, 32767]推送至栈顶。
 * iconst_1: 表示将int型的1推送至栈顶(iconst_m1~iconst_5, -1~5)。
 */
public class MyTest3 {
    public static void main(String[] args) {
        System.out.println(Demo1.m);
    }
}

class Demo1 {
    public static final String str = "hello world";

    public static final short s = 127;

    public static final int i = 128;

    public static final int m = 6;

    static {
        System.out.println("Demo1 static block");
    }
}
