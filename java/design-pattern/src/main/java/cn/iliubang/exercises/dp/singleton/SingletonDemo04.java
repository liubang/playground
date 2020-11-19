package cn.iliubang.exercises.dp.singleton;

/**
 * 静态内部类式
 * 要点：
 * 1. 外部类没有static属性，则不会像饿汉式那样立即加载对象
 * 2. 只有真正调用getInstance()，才会加载静态内部类。加载类是线程安全的。instance是static final类型，保证了
 * 内存中只有一个这样的实例存在，而且只能被赋值一次，从而保证了线程安全。
 * 3. 兼备了并发高效调用和延迟加载的优势
 */
public class SingletonDemo04 {

    private SingletonDemo04() {
    }

    private static class SingletonClassInstance {
        private static final SingletonDemo04 instance = new SingletonDemo04();
    }

    public static SingletonDemo04 getInstance() {
        return SingletonClassInstance.instance;
    }
}
