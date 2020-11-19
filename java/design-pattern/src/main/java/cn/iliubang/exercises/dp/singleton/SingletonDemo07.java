package cn.iliubang.exercises.dp.singleton;

import java.io.ObjectStreamException;
import java.io.Serializable;

/**
 * 如何防止反射和反序列化漏洞
 */
public class SingletonDemo07 implements Serializable {

    private static SingletonDemo07 instance;

    private SingletonDemo07() {
        if (null != instance) {
            throw new RuntimeException();
        }
    }

    public static synchronized SingletonDemo07 getInstance() {
        if (null == instance) {
            instance = new SingletonDemo07();
        }
        return instance;
    }

    /**
     * 反序列化时，如果定义了这个方法，会调用这个方法，直接返回原来的对象，而不需要产生新的实例
     */
    private Object readResolve() throws ObjectStreamException {
        return instance;
    }
}
