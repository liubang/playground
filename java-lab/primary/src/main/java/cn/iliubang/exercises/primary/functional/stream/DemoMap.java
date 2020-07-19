package cn.iliubang.exercises.primary.functional.stream;

import org.junit.Assert;
import org.junit.Test;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.stream.Collectors;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/9/27 15:51 $
 * @see
 */
public class DemoMap {

    @Test
    public void test() {
        List<String> collected = new ArrayList<>();
        collected.add("a");
        collected.add("b");
        collected.add("hello");

        List<String> upper = collected.stream().map(String::toUpperCase).collect(Collectors.toList());

        System.out.println(upper.hashCode());
        System.out.println(collected.hashCode());

        Assert.assertEquals(Arrays.asList("A", "B", "HELLO"), upper);
    }
}
