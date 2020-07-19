package cn.iliubang.exercises.jvm.classloader;

/**
 * 类加载的流程是 准备->解析->初始化，在准备阶段为类的静态变量分配内存，并将其初始化为默认值，
 * 然后再根据代码顺序，从上到下对成员初始化，counter1初始化默认值为0，singleton，需要调用构造方法，
 * 由于counter2在准备阶段已经被分配内存，默认为0，故在构造方法内，counter2执行操作后也变成了1，然后程序
 * 继续初始化，到counter2初始化阶段，被赋值为0。
 */
public class MyTest7 {
    public static void main(String[] args) {
        Singleton singleton = Singleton.getInstance();
        System.out.println("counter1:" + Singleton.counter1);
        System.out.println("counter2:" + Singleton.counter2);
    }
}

class Singleton {
    public static int counter1;

    private static Singleton singleton = new Singleton();

    private Singleton() {
        counter1++;
        counter2++; // 准备阶段的重要意义
    }

    public static int counter2 = 0;

    public static Singleton getInstance() {
        return singleton;
    }
}
