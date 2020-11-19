package cn.iliubang.exercises.primary.classloader.some;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/2
 */
public class TestMyClassLoader {

    public static void main(String[] args) throws Exception {
        MyClassLoader classLoader = new MyClassLoader("/tmp");
        Class clazz = classLoader.loadClass("Demo");
        System.out.println(clazz);
    }

}
