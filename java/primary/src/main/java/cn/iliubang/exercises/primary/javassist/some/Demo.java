package cn.iliubang.exercises.primary.javassist.some;

import javassist.*;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */
public class Demo {

    public static void main(String[] args) throws Exception {
        ClassPool classPool = ClassPool.getDefault();
        //创建类
        CtClass ctClass = classPool.makeClass("cn.iliubang.exercises.primary.javassist.some.Emp");
        //创建属性
        CtField empno = CtField.make("private int empno;", ctClass);
        CtField ename = CtField.make("private String ename;", ctClass);
        ctClass.addField(empno);
        ctClass.addField(ename);

        //创建方法
        CtMethod getEmpno = CtMethod.make("public int getEmpno() {\n" +
                "        return empno;\n" +
                "    }", ctClass);
        CtMethod setEmpno = CtMethod.make("public void setEmpno(int empno) {\n" +
                "        this.empno = empno;\n" +
                "    }", ctClass);
        CtMethod getEname = CtMethod.make("public String getEname() {\n" +
                "        return ename;\n" +
                "    }", ctClass);
        CtMethod setEname = CtMethod.make("public void setEname(String ename) {\n" +
                "        this.ename = ename;\n" +
                "    }", ctClass);

        ctClass.addMethod(getEmpno);
        ctClass.addMethod(setEmpno);
        ctClass.addMethod(getEname);
        ctClass.addMethod(setEname);

        //创建无参构造器
        CtConstructor ctConstructor1 = new CtConstructor(new CtClass[]{}, ctClass);
        ctConstructor1.setBody("{}");
        ctClass.addConstructor(ctConstructor1);

        //创建构有参造器
        CtConstructor ctConstructor = new CtConstructor(new CtClass[]{CtClass.intType, classPool.get("java.lang.String")}, ctClass);
        ctConstructor.setBody("{\n" +
                "        this.empno = $1;\n" +
                "        this.ename = $2;\n" +
                "    }");
        ctClass.addConstructor(ctConstructor);

        //将构建好的类写入到文件中，生成编译好的字节码文件
        ctClass.writeFile("/Users/liubang/workspace/java/exercises/primary/target/classes");
    }

}

/*
class Emp {
    private int empno;
    private String ename;

    public Emp(int empno, String ename) {
        this.empno = empno;
        this.ename = ename;
    }

    public int getEmpno() {
        return empno;
    }

    public void setEmpno(int empno) {
        this.empno = empno;
    }

    public String getEname() {
        return ename;
    }

    public void setEname(String ename) {
        this.ename = ename;
    }
}
*/
