package cn.iliubang.exercises.primary.aop.proxy.dynamicProxy;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/2
 */
public class StarHandler implements InvocationHandler {

    Star star;

    public StarHandler(Star star) {
        this.star = star;
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        System.out.println("======before======");
        method.invoke(star, args);
        System.out.println("======after======");
        return null;
    }
}
