package cn.iliubang.exercises.dp.singleton;

/**
 * 如何防止反射和反序列化漏洞
 */
public class SingletonDemo06 {

    private static SingletonDemo06 instance;

    private SingletonDemo06() {
        if (null != instance) {
            throw new RuntimeException();
        }
    }

    public static synchronized SingletonDemo06 getInstance() {
        if (null == instance) {
            instance = new SingletonDemo06();
        }
        return instance;
    }
}
