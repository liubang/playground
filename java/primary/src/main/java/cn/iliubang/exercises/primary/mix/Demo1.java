package cn.iliubang.exercises.primary.mix;

import java.util.ArrayList;
import java.util.List;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/8/9 15:41 $
 * @see
 */
public class Demo1 {
    public static void main(String[] args) {
        List<Long> licenseIds = new ArrayList<>();
        for (long i = 0; i < 100; i++) {
            licenseIds.add(i + 3);
        }

        System.out.println(licenseIds);

        long o = 3;
        licenseIds.remove(o);

        System.out.println(licenseIds);
    }
}
