package cn.iliubang.exercises.primary.annotation.some;

import java.util.ArrayList;
import java.util.List;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */


/*
 * 内置注解:
 *
 */
public class Demo01 {

    @Override
    public String toString() {
        return "Demo01{}";
    }

    @Deprecated
    public static void show() {
        System.out.println("show");
    }

    @SuppressWarnings("unchecked")
    public static void show2() {
        List list = new ArrayList();
    }

    @SuppressWarnings(value = {"unchecked", "path"})
    public static void show3() {
        System.out.println("hello");
    }

    public static void main(String[] args) {
        Demo01.show();
    }
}
