package cn.iliubang.exercises.primary.aop.proxy.dynamicProxy;

import java.lang.reflect.Proxy;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/2
 */
public class Client {
    public static void main(String[] args) {
        Star star = new RealStar();
        StarHandler starHandler = new StarHandler(star);

        Star proxy = (Star) Proxy.newProxyInstance(ClassLoader.getSystemClassLoader(), new Class[]{Star.class}, starHandler);
        proxy.bookTicket();
        proxy.sing();
    }
}
