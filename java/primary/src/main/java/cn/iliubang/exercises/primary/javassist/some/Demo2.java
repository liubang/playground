package cn.iliubang.exercises.primary.javassist.some;

import javassist.ClassPool;
import javassist.CtClass;
import javassist.CtMethod;
import javassist.Modifier;

import java.lang.reflect.Method;
import java.util.Arrays;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */
public class Demo2 {

    /**
     * 测试操作类
     *
     * @throws Exception
     */
    public static void show() throws Exception {
        ClassPool classPool = ClassPool.getDefault();
        //获取类
        CtClass ctClass = classPool.get("cn.iliubang.exercises.primary.javassist.some.Emp");
        //获取字节码
        byte[] bytes = ctClass.toBytecode();
        System.out.println(Arrays.toString(bytes));
        //获取类名
        System.out.println(ctClass.getName());
        System.out.println(ctClass.getSimpleName());
        //获取父类
        System.out.println(ctClass.getSuperclass());
    }

    /**
     * 测试产生新的方法
     *
     * @throws Exception
     */
    public static void show2() throws Exception {
        ClassPool classPool = ClassPool.getDefault();
        CtClass ctClass = classPool.get("cn.iliubang.exercises.primary.javassist.some.Emp");

        //CtMethod add = CtNewMethod.make("public int add(int a, int b) { return a + b; }", ctClass);
        //或者
        //声明方法
        CtMethod add = new CtMethod(CtClass.intType, "add", new CtClass[]{CtClass.intType, CtClass.intType}, ctClass);
        add.setModifiers(Modifier.PUBLIC);
        add.setBody("{System.out.println(\"这是构造的方法\"); return $1 + $2;}");
        ctClass.addMethod(add);
        //通过反射调用生成的方法
        Class clazz = ctClass.toClass();
        Object obj = clazz.newInstance();
        Method method = clazz.getDeclaredMethod("add", int.class, int.class);
        Object result = method.invoke(obj, 12, 34);
        System.out.println("result:" + result);
    }

    /**
     * 修改已有的方法
     *
     * @throws Exception
     */
    public static void show3() throws Exception {
        ClassPool classPool = ClassPool.getDefault();
        CtClass ctClass = classPool.get("cn.iliubang.exercises.primary.javassist.some.Emp");
        //获取已有的方法
        CtMethod ctMethod = ctClass.getDeclaredMethod("getEname", new CtClass[]{});
        ctMethod.insertBefore("System.out.println(\"测试修改方法\");");

        //通过反射调用生成的方法
        Class clazz = ctClass.toClass();
        Object obj = clazz.newInstance();
        Method method = clazz.getDeclaredMethod("getEname");
        Object result = method.invoke(obj);
        System.out.println("result:" + result);
    }

    public static void main(String[] args) throws Exception {
        //show();
        //show2();
        show3();
    }

}
