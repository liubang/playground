package cn.iliubang.exercises.primary.io.file;

import java.io.File;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/2
 */
public class Demo1 {

    /**
     * 两个常量：
     * 1. 路径分隔符
     * 2. 文件分隔符
     * <p>
     * 相对路径与绝对路径构造File对象:
     *
     * @param args
     */
    public static void main(String[] args) {
        System.out.println(File.pathSeparator);
        System.out.println(File.separator);

        // 路径的几种表示形式
        String path = "/tmp/Demo.java";
        path = File.separator + "tmp" + File.separator + "Demo.java";

        File src = new File("/tmp/Demo.java");
        src = new File("/tmp/", "Demo.java");
        System.out.println(src.getName());
        System.out.println(src.getPath());
    }
}