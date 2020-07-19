package cn.iliubang.exercises.primary.classloader.some;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */
public class Demo2 {
    public static void main(String[] args) {
        System.out.println(ClassLoader.getSystemClassLoader());
        System.out.println(ClassLoader.getSystemClassLoader().getParent());
        System.out.println(ClassLoader.getSystemClassLoader().getParent().getParent());

        System.out.println("==================================");
        String name = "liubang";
        System.out.println(name.getClass().getClassLoader());
        System.out.println(name);
    }
}
