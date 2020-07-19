package cn.iliubang.exercises.jvm.classloader;

import java.util.Random;

/**
 * 当一个接口在初始化时，并不要求其父接口都完成了初始化。
 * 只有在真正使用到父接口的时候（如引用接口中所定义的常量时），才会初始化。
 */
public class MyTest6 {
    public static void main(String[] args) {
        System.out.println(Demo4Child.b);
    }
}

interface Demo4 {
    int a = new Random().nextInt(3);
}

interface Demo4Child extends Demo4 {
    int b = 5;
}
