package cn.iliubang.exercises.primary.functional.lambda;

import javax.swing.text.DateFormatter;
import java.text.SimpleDateFormat;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/9/27 14:54 $
 * @see
 */
public class ThreadLocalTest {
    /*
     * Java中有一个ThreadLocal类，作为容器保存了当前线程里局部变量的值。
     * Java8为该类新加了一个工厂方法，接受一个Lambda表达式，并产生一个新
     * 的ThreadLocal对象，而不适用继承，语法上更简洁。
     *
     * DateFormatter是非线程安全的，使用构造函数创建一个线程安全的DateFormatter对象
     */
    public final static ThreadLocal<DateFormatter> formatter = ThreadLocal.withInitial(() -> new DateFormatter(new SimpleDateFormat("yyyy-MM-dd")));


}
