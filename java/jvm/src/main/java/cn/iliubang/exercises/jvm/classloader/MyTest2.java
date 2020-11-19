package cn.iliubang.exercises.jvm.classloader;

/**
 * 当一个类在初始化时，要求其父类全部都已经初始化完毕
 */
public class MyTest2 {
    public static void main(String[] args) {
        System.out.println(MyChild2.str1);
    }
}

class MyParent2 {
    public static String str = "hello world";

    static {
        System.out.println("MyParent2 static block");
    }
}

class MyChild2 extends MyParent2 {
    public static String str1 = "ok";

    static {
        System.out.println("MyChild2 static block");
    }
}