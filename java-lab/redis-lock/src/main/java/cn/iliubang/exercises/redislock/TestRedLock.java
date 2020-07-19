package cn.iliubang.exercises.redislock;

import java.util.concurrent.TimeUnit;

public class TestRedLock {

    private static final String resource = "name";

    public static void main(String[] args) {
        Thread t1 = new Thread(() -> {
            RedisLock lock = new RedisLock();
            boolean l = lock.lock(resource, 1000);

            if (l) {
                System.out.println(Thread.currentThread().getName() + ": lock ok");
                try {
                    TimeUnit.MICROSECONDS.sleep(10);
                } catch (InterruptedException e) {
                    e.printStackTrace();
                }
            } else {
                System.out.println(Thread.currentThread().getName() + ": lock failed");
            }

            Runtime.getRuntime().addShutdownHook(new Thread(() -> {
                if (l) {
                    lock.unlock(resource);
                }
            }));

        });

        Thread t2 = new Thread(() -> {
            RedisLock lock = new RedisLock();
            boolean l = lock.lock(resource, 1000);

            if (l) {
                System.out.println(Thread.currentThread().getName() + ": lock ok");
                try {
                    TimeUnit.MICROSECONDS.sleep(10);
                } catch (InterruptedException e) {
                    e.printStackTrace();
                }
            } else {
                System.out.println(Thread.currentThread().getName() + ": lock failed");
            }

            Runtime.getRuntime().addShutdownHook(new Thread(() -> {
                if (l) {
                    lock.unlock(resource);
                }
            }));

        });


        t1.start();
        t2.start();

        try {
            t1.join();
            t2.join();
        } catch (InterruptedException e) {
            e.printStackTrace();
        }

        System.out.println("main: done.");
    }
}
