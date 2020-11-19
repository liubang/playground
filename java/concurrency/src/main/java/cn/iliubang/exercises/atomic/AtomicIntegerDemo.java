package cn.iliubang.exercises.atomic;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/10/15 22:07 $
 * @see
 */
public class AtomicIntegerDemo {
    static AtomicInteger i = new AtomicInteger();

    static class AddThread implements Runnable {
        @Override
        public void run() {
            for (int k = 0; k < 10000; k++) {
                i.incrementAndGet();
            }
        }
    }

    public static void main(String[] args) throws InterruptedException {
        Thread[] ts = new Thread[10];
        for (int k = 0; k < 10; k++) {
            ts[k] = new Thread(new AddThread());
        }

        for (int k = 0; k < 10; k++) {
            ts[k].start();
        }

        for (int k = 0; k < 10; k++) {
            ts[k].join();
        }

        System.out.println(i);
    }
}
