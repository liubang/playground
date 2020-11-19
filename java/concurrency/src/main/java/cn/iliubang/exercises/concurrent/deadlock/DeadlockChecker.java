package cn.iliubang.exercises.concurrent.deadlock;

import java.lang.management.ManagementFactory;
import java.lang.management.ThreadInfo;
import java.lang.management.ThreadMXBean;
import java.util.Arrays;
import java.util.concurrent.TimeUnit;

public class DeadlockChecker {

    private final static ThreadMXBean mbean = ManagementFactory.getThreadMXBean();
    private final static Runnable deadlockCheck = () -> {
        for (; ; ) {
            long[] deadlockedThreadIds = mbean.findDeadlockedThreads();
            if (deadlockedThreadIds != null) {
                ThreadInfo[] threadInfos = mbean.getThreadInfo(deadlockedThreadIds);
                Arrays.asList(threadInfos).forEach(t -> Thread.getAllStackTraces().forEach((k, v) -> {
                    if (t.getThreadId() == k.getId()) {
                        k.interrupt();
                    }
                }));
            }
            try {
                TimeUnit.MILLISECONDS.sleep(5000);
            } catch (InterruptedException ex) {
                ex.printStackTrace();
            }
        }
    };

    public static void check() {
        Thread t = new Thread(deadlockCheck);
        t.setDaemon(true);
        t.start();
    }
}
