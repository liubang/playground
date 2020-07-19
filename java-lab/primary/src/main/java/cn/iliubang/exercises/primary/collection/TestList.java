package cn.iliubang.exercises.primary.collection;

import java.util.ArrayList;
import java.util.Date;
import java.util.List;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/8
 */
public class TestList {
    @SuppressWarnings("all")
    public static void main(String[] args) {
        List list = new ArrayList();
        list.add("aaa");
        list.add(new Date());
        list.add(1234);
        System.out.println(list.size());
        System.out.println(list.get(2));
        System.out.println(list.remove("aaa"));
    }
}
