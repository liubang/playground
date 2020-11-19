package cn.iliubang.exercises.primary.classloader.some;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */
public class Demo {

    public static void main(String[] args) {
//        Foo a = new Foo();
//        System.out.println(Foo.age);
        System.out.println(Bar.age);
        System.out.println(Bar.msg);
    }
}

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
    public static final String msg = "hello world";

    static {
        System.out.println("Bar 静态初始化代码块");
    }
}
