package cn.iliubang.exercises.primary.reflection.some;


import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */
public class Demo2 {
    public int age;
    private String name;

    public Demo2(int age, String name) {
        this.age = age;
        this.name = name;
    }

    public void show() {

    }

    private void show2() {

    }

    public static void main(String[] args) {
        Class clazz = Demo2.class;
        //获取类的名字
        //获取 包名.类名
        System.out.println(clazz.getName());
        //获取类名
        System.out.println(clazz.getSimpleName());

        //获取属性，只能获取public的属性
        System.out.println("===================获取public属性===================");
        Field[] fields = clazz.getFields();
        for (Field field : fields) {
            System.out.println(field.getName() + "::" + field.getModifiers() + "::" + field.getType());
        }
        //获取所有属性
        System.out.println("===================获取所有属性===================");
        fields = clazz.getDeclaredFields();
        for (Field field : fields) {
            System.out.println(field.getName() + "::" + field.getModifiers() + "::" + field.getType());
        }

        //获取方法，只能获取public修饰的方法
        System.out.println("===================获取public修饰的方法===================");
        Method[] methods = clazz.getMethods();
        for (Method method : methods) {
            method.setAccessible(true);
            System.out.println(method.getName() + "::" + method.getModifiers() + "::" + method.getReturnType());
        }
        //获取所有方法
        System.out.println("===================获取所有方法===================");
        methods = clazz.getDeclaredMethods();
        for (Method method : methods) {
            System.out.println(method.getName() + "::" + method.getModifiers() + "::" + method.getReturnType());
        }
    }
}
