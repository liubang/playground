package cn.iliubang.exercises.jvm.classloader;

/**
 *
 */
public class MyTest8 {
    public static void main(String[] args) throws Exception {
        Class<?> clazz = Class.forName("java.lang.String");
        System.out.println(clazz.getClassLoader());

        clazz = Class.forName("cn.iliubang.exercises.jvm.classloader.Demo5");
        System.out.println(clazz.getClassLoader());
    }
}

class Demo5 {

}
