package cn.iliubang.exercises.primary.functional.stream;

import org.junit.Test;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/9/27 16:59 $
 * @see
 */
public class DemoMinAndMax {


    @Test
    public void test() {
        List<Integer> list = new ArrayList<>();
        list.add(2);
        list.add(3);
        list.add(1);
        list.add(6);
        list.add(18);

        System.out.println(list.stream().min(Comparator.comparing(e -> e)).get());
        System.out.println(list.stream().max(Comparator.comparing(e -> e)).get());
    }
}
