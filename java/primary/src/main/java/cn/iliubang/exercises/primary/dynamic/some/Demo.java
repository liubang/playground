package cn.iliubang.exercises.primary.dynamic.some;


import javax.tools.JavaCompiler;
import javax.tools.ToolProvider;
import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */
public class Demo {
    @SuppressWarnings("unchecked")
    public static void main(String[] args) {
        JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
        int result = compiler.run(null, null, null, "/tmp/Demo.java");
        if (result == 0) {
            System.out.println("编译成功");
        } else {
            System.out.println("编译失败");
        }
        System.out.println("=======================方法一=======================");
        try {
            Runtime runtime = Runtime.getRuntime();
            Process process = runtime.exec("java -cp /tmp Demo");
            InputStream inputStream = process.getInputStream();
            BufferedReader bufferedReader = new BufferedReader(new InputStreamReader(inputStream));
            String info = "";
            while ((info = bufferedReader.readLine()) != null) {
                System.out.println(info);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }

        System.out.println("=======================方法二=======================");

        try {
            URL[] urls = new URL[]{new URL("file:///tmp/")};
            URLClassLoader loader = new URLClassLoader(urls);
            Class clazz = loader.loadClass("Demo");
            //调用加载类的main方法
            Method method = clazz.getMethod("main", String[].class);
            method.setAccessible(true);
            method.invoke(null, (Object) new String[]{});
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
