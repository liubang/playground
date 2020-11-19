package cn.iliubang.exercises.atomic;

import java.util.concurrent.atomic.AtomicReference;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/10/15 22:11 $
 * @see
 */
public class AtomicReferenceTest {

    final static AtomicReference<String> atomicStr = new AtomicReference<>("abc");

    public static void main(String[] args) {
        for (int i = 0; i < 10; i++) {
            new Thread(() -> {
                try {
                    Thread.sleep(Math.abs((int) (Math.random() * 100)));
                } catch (InterruptedException e) {
                    e.printStackTrace();
                }

                if (atomicStr.compareAndSet("abc", "def")) {
                    System.out.println("Thread:" + Thread.currentThread().getId() + " changed");
                } else {
                    System.out.println("Thread:" + Thread.currentThread().getId() + " failed");
                }
            }).start();
        }
    }
}
