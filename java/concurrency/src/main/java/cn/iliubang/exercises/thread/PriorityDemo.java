package cn.iliubang.exercises.thread;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/10/13 20:46 $
 */
public class PriorityDemo {

    public static class HighPriority extends Thread {
        static int count = 0;

        @Override
        public void run() {
            for (; ; ) {
                synchronized (PriorityDemo.class) {
                    count++;
                    if (count > 1000000) {
                        System.out.println(this.getClass().getName() + " is completed");
                        break;
                    }
                }
            }
        }
    }

    public static class LowPriority extends Thread {
        static int count = 0;

        @Override
        public void run() {
            for (; ; ) {
                synchronized (PriorityDemo.class) {
                    count++;
                    if (count > 1000000) {
                        System.out.println(this.getClass().getName() + " is completed");
                        break;
                    }
                }
            }
        }
    }

    public static void main(String[] args) {
        Thread high = new HighPriority();
        Thread low = new LowPriority();
        high.setPriority(Thread.MAX_PRIORITY);
        low.setPriority(Thread.MIN_PRIORITY);

        low.start();
        high.start();
    }
}
