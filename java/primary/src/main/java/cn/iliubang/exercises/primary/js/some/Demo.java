package cn.iliubang.exercises.primary.js.some;

import javax.script.Bindings;
import javax.script.Invocable;
import javax.script.ScriptEngine;
import javax.script.ScriptEngineManager;
import java.io.FileReader;
import java.util.List;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */
public class Demo {
    public static void main(String[] args) {
        ScriptEngineManager scriptEngineManager = new ScriptEngineManager();
        ScriptEngine scriptEngine = scriptEngineManager.getEngineByName("js");
        // 定义变量
        scriptEngine.put("msg", "hello world");
        String js =
                "var console = {" +
                        "    log: print," +
                        "    warn: print," +
                        "    error: print" +
                        "};";

        js += "var user = {name: 'liubang', age: 24};";
        js += "console.log(user.name);";
        js += "user.desc = msg;";
        js += "console.log(user.desc);";

        //执行js脚本
        try {
            scriptEngine.eval(js);

            //JAVA和js代码交互
            Bindings obj = (Bindings) scriptEngine.eval("user;");
            Integer age = (Integer) obj.get("age");
            String name = (String) obj.get("name");
            System.out.println(name + "::" + age);

            //执行js函数
            scriptEngine.eval("function add(a, b) {return a + b;}");
            Invocable invocable = (Invocable) scriptEngine;
            Object result = invocable.invokeFunction("add", new Object[]{2017, 1});
            System.out.println(result);

            //js中导入JAVA包，使用JAVA类
            js =
                    "var Arrays = Java.type('java.util.Arrays');" +
                            "var list = Arrays.asList([\"刘邦\", \"新年好\"]);";
            scriptEngine.eval(js);
            List list = (List) scriptEngine.get("list");
            System.out.println(list);

            //执行js文件
            FileReader fileReader = new FileReader("primary/target/classes/cn/iliubang/exercises/primary/js/some/demo.js");
            scriptEngine.eval(fileReader);
            fileReader.close();
        } catch (Exception e) {
            e.printStackTrace();
        }

    }
}
