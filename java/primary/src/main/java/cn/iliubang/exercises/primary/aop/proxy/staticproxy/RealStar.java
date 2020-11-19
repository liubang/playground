package cn.iliubang.exercises.primary.aop.proxy.staticproxy;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/2
 */
public class RealStar implements Star {

    @Override
    public void confer() {
        System.out.println("RealStar.confer()");
    }

    @Override
    public void signContract() {
        System.out.println("RealStar.signContract()");
    }

    @Override
    public void bookTicket() {
        System.out.println("RealStar.bookTicket()");
    }

    @Override
    public void sing() {
        System.out.println("RealStar.sing()");
    }

    @Override
    public void collectMoney() {
        System.out.println("RealStar.collectMoney()");
    }
}
