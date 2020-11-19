package cn.iliubang.exercises.dp.singleton;

public class SingletonDemo03 {

    private static volatile SingletonDemo03 instance;

    private SingletonDemo03() {

    }

    public static SingletonDemo03 getInstance() {
        if (null == instance) {
            synchronized (SingletonDemo03.class) {
                if (null == instance) {
                    instance = new SingletonDemo03();
                }
            }
        }
        return instance;
    }
}
