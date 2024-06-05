package cn.iliubang.exercises.mdc;

import org.apache.log4j.Logger;
import org.apache.log4j.MDC;

import java.util.concurrent.TimeUnit;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2019-03-02 15:35 $
 */
public class Demo {

    public static void main(String[] args) throws Exception {
        Logger logger = Logger.getLogger(Demo.class);
        MDC.put("name", "liubang");
        MDC.put("age", 20);
        logger.info("OK");

        TimeUnit.SECONDS.sleep(1);

        Thread t1 = new Thread(() -> {
            logger.info("t1");
            Object name = MDC.get("name");
            logger.info(name);
        });
        t1.start();

        TimeUnit.SECONDS.sleep(1);

        Thread t2 = new Thread(() -> {
            logger.info("t2");
            Object age = MDC.get("age");
            logger.info(age);
        });

        t2.start();

        t1.join();
        t2.join();

        logger.info("END");
    }

}
