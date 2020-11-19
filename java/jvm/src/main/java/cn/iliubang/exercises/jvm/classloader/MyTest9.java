package cn.iliubang.exercises.jvm.classloader;

class FinalTest {
    public static final int x = 3;

    static {
        System.out.println(FinalTest.class.getName());
    }
}

public class MyTest9 {
    public static void main(String[] args) {
        System.out.println(FinalTest.x);
    }
}
