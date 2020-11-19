package cn.iliubang.exercises.primary.io.file;

import java.io.FileReader;
import java.io.FileWriter;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/3
 */
public class Demo2 {
    public static void main(String[] args) throws Exception {
        FileReader fileReader = new FileReader("/tmp/Demo.java");
        FileWriter fileWriter = new FileWriter("/tmp/Demo.bak.java");
        char cbuf[] = new char[1024];
        int len = 0;
        while ((len = fileReader.read(cbuf)) >= 0) {
            System.out.print(new String(cbuf, 0, len));
            fileWriter.write(cbuf);
        }
        fileReader.close();
        fileWriter.flush();
        fileWriter.close();
    }
}
