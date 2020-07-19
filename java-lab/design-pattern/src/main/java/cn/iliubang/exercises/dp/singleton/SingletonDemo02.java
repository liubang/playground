package cn.iliubang.exercises.dp.singleton;

/**
 * 懒汉式单例模式
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version : {Version} $ : 2019-07-21 19:20 $
 */
public class SingletonDemo02 {

    private static SingletonDemo02 instance;

    private SingletonDemo02() {
    }

    public static synchronized SingletonDemo02 getInstance() {
        if (null == instance) {
            instance = new SingletonDemo02();
        }
        return instance;
    }
}
