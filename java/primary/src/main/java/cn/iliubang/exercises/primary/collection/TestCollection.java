package cn.iliubang.exercises.primary.collection;

import cn.iliubang.exercises.primary.oop.EqualsDemo;

import java.util.ArrayList;
import java.util.Collection;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/15
 */
public class TestCollection {

    public static void main(String[] args) {
        Collection c = new ArrayList();
        c.add(153);
        c.add(new Integer(153));
        c.add("hello");
        c.add(new EqualsDemo("liu", "bang"));
        c.add(new EqualsDemo("liu", "bang"));
        System.out.println(c.size());
        System.out.println(c);
        c.remove(new EqualsDemo("liu", "bang"));
        System.out.println(c.size());
        System.out.println(c);
    }
}
