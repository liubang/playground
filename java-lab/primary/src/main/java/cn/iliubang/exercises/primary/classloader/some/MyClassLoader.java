package cn.iliubang.exercises.primary.classloader.some;

import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/2
 */
public class MyClassLoader extends ClassLoader {

    /**
     * class path
     */
    private String classPath;

    public MyClassLoader(String classPath) {
        this.classPath = classPath;
    }

    @Override
    protected Class<?> findClass(String name) throws ClassNotFoundException {
        // 先查找是否加载过
        Class<?> clazz = findLoadedClass(name);
        // 如果已经加载过，则直接返回查找到的Class实例
        if (null != clazz) {
            return clazz;
        }
        // 父类加载器
        ClassLoader parent = getParent();
        try {
            clazz = parent.loadClass(name);
            if (null != clazz) {
                return clazz;
            }
        } catch (ClassNotFoundException e) {

        }
        // 只能自己干了
        byte[] bytes = getClassData(name);
        if (bytes == null) {
            throw new ClassNotFoundException();
        } else {
            clazz = defineClass(name, bytes, 0, bytes.length);
            return clazz;
        }
    }

    private byte[] getClassData(String name) {
        String path = classPath + "/" + name.replace('.', '/') + ".class";
        InputStream inputStream = null;
        ByteArrayOutputStream byteArrayOutputStream = new ByteArrayOutputStream();
        try {
            inputStream = new FileInputStream(path);
            byte[] buffer = new byte[1024];
            int temp = 0;
            while ((temp = inputStream.read(buffer)) != -1) {
                byteArrayOutputStream.write(buffer, 0, temp);
            }
            return byteArrayOutputStream.toByteArray();
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        } finally {
            try {
                if (null != inputStream)
                    inputStream.close();
                byteArrayOutputStream.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
        }
    }

}

