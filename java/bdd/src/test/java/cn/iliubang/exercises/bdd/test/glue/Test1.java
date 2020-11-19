package cn.iliubang.exercises.bdd.test.glue;

import cucumber.api.java.zh_cn.假如;
import cucumber.api.java.zh_cn.当;
import cucumber.api.java.zh_cn.那么;
import org.junit.Assert;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2019-03-16 18:24 $
 */
public class Test1 {


    private Integer a;
    private Integer b;
    private Integer c;

    @假如("我们有两个整数{int}和{int}")
    public void 我们有两个整数_和(Integer int1, Integer int2) {
        a = int1;
        b = int2;
    }

    @当("我们将其求和运算")
    public void 我们将其求和运算() {
        c = a + b;
    }

    @那么("我们能得到数值 {int}")
    public void 我们能得到数值(Integer int1) {
        Assert.assertEquals(int1, c);
    }
}
