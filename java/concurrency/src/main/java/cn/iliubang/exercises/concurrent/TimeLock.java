package cn.iliubang.exercises.concurrent;

import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/10/21 20:44 $
 * @see
 */
public class TimeLock implements Runnable {

    private static ReentrantLock lock = new ReentrantLock();

    @Override
    public void run() {
        try {
            if (lock.tryLock(5, TimeUnit.SECONDS)) {
                TimeUnit.SECONDS.sleep(6);
            } else {
                System.out.println("get lock failed");
            }
        } catch (InterruptedException ex) {
            ex.printStackTrace();
        } finally {
            if (lock.isHeldByCurrentThread())
                lock.unlock();
        }
    }

    public static void main(String[] args) {
        TimeLock timeLock = new TimeLock();
        Thread t1 = new Thread(timeLock);
        Thread t2 = new Thread(timeLock);
        t1.start();
        t2.start();
    }
}
