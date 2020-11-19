package cn.iliubang.exercises.primary.reflection.some;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */
public class Demo1 {
    public static void main(String[] args) {
        try {
            Class clazz = Class.forName("cn.iliubang.exercises.primary.reflection.some.Demo1");
            System.out.println(clazz);
            System.out.println(Demo1.class);
            System.out.println((new Demo1()).getClass());
        } catch (ClassNotFoundException e) {
            e.printStackTrace();
        }
    }
}
