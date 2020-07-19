package cn.iliubang.exercises.dp.singleton;

import java.lang.reflect.Constructor;

/**
 * 测试反射和反序列化破解
 */
public class Client01 {
    public static void main(String[] args) {
        SingletonDemo06 s1 = SingletonDemo06.getInstance();
        SingletonDemo06 s2 = SingletonDemo06.getInstance();

        System.out.println(s1);
        System.out.println(s2);

        try {
            Class<SingletonDemo06> clazz = (Class<SingletonDemo06>) Class.forName("cn.iliubang.exercises.dp.singleton.SingletonDemo06");
            Constructor<SingletonDemo06> c = clazz.getDeclaredConstructor();
            c.setAccessible(true);
            SingletonDemo06 s3 = c.newInstance();
            SingletonDemo06 s4 = c.newInstance();
            c.setAccessible(false);
            System.out.println(s3);
            System.out.println(s4);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
