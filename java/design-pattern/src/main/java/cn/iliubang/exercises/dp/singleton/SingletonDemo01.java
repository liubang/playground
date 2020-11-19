package cn.iliubang.exercises.dp.singleton;

/**
 * 饿汉式单例模式
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version : {Version} $ : 2019-07-21 19:11 $
 */
public class SingletonDemo01 {

    /**
     * 由于加载类的时候，天然是线程安全的
     * 没有延迟加载的优势
     */
    private static SingletonDemo01 instance = new SingletonDemo01();

    /**
     * 私有构造器
     */
    private SingletonDemo01() {
    }


    /**
     * 方法没有使用同步，调用效率高
     */
    public static SingletonDemo01 getInstance() {
        return instance;
    }
}
