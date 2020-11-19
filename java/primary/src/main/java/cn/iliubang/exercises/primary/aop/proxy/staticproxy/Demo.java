package cn.iliubang.exercises.primary.aop.proxy.staticproxy;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/2
 */
public class Demo {
    public static void main(String[] args) {
        Star star = new RealStar();
        ProxyStar proxyStar = new ProxyStar(star);

        proxyStar.confer();
        proxyStar.signContract();
        proxyStar.bookTicket();
        proxyStar.sing();
        proxyStar.collectMoney();
    }
}
