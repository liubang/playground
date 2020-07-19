package cn.iliubang.exercises.agent;

import java.lang.instrument.Instrumentation;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2019-03-01 22:41 $
 */
public class DefaultAgent {

    public static void premain(String agentArgs, Instrumentation instrumentation) {
        instrumentation.addTransformer(new LogTransformer());
    }
}
