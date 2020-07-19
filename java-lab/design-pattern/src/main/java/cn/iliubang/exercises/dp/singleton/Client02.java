package cn.iliubang.exercises.dp.singleton;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;

/**
 * 测试反射和反序列化破解
 */
public class Client02 {
    public static void main(String[] args) throws Exception {
        SingletonDemo07 s1 = SingletonDemo07.getInstance();
        SingletonDemo07 s2 = SingletonDemo07.getInstance();

        System.out.println(s1);
        System.out.println(s2);

        // 序列化，写入磁盘
        FileOutputStream fos = new FileOutputStream("/tmp/s1.log");
        ObjectOutputStream oos = new ObjectOutputStream(fos);
        oos.writeObject(s1);
        oos.close();

        // 反序列化
        ObjectInputStream ois = new ObjectInputStream(new FileInputStream("/tmp/s1.log"));
        SingletonDemo07 s3 = (SingletonDemo07) ois.readObject();
        System.out.println(s3);
    }
}
