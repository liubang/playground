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
 * @version $Revision: {Version} $ $Date: 2018/9/27 15:39 $
 * @see
 */
public class DemoCollectToList {

    @Test
    public void test() {
        List<String> a = new ArrayList<>();
        a.add("a");
        a.add("b");
        a.add("c");
        a.add("d");

        List<String> b = a.stream().filter(e -> !e.equals("b")).collect(Collectors.toList());
        Assert.assertEquals(Arrays.asList("a", "c", "d"), b);
    }
}
