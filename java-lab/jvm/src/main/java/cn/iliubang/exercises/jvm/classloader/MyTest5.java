package cn.iliubang.exercises.jvm.classloader;

/**
 * 对于数组类型实例来说，其类型是由JVM运行期动态生成的，表示为[Lxxx.xxx.Xxx这种形式。
 * 动态生成的类型其父类型就是Object，对于数组来说，JavaDoc经常将构成数组的元素称为Component，实际上
 * 就是将数组降低一个维度后的类型。
 *
 * 助记符：
 * anewarray: 表示创建一个引用类型的（如类、接口、数组）数组，并将其引用值压入栈顶。
 * newarray: 表示创建一个指定的原始类型（如int,float,char等）数组，并将其引用值压入栈顶。
 */
public class MyTest5 {
    public static void main(String[] args) {
        Demo3[] demo3s = new Demo3[1];
        System.out.println(demo3s.getClass());
        System.out.println(demo3s.getClass().getSuperclass());

        Demo3[][] demo3s1 = new Demo3[1][1];
        System.out.println(demo3s1.getClass());
        System.out.println(demo3s1.getClass().getSuperclass());

        int[] ints = new int[1];
        System.out.println(ints.getClass());
        System.out.println(ints.getClass().getSuperclass());

        char[] chars = new char[1];
        System.out.println(chars.getClass());
        System.out.println(chars.getClass().getSuperclass());

        boolean[] booleans = new boolean[1];
        System.out.println(booleans.getClass());
        System.out.println(booleans.getClass().getSuperclass());

        short[] shorts = new short[1];
        System.out.println(shorts.getClass());
        System.out.println(shorts.getClass().getSuperclass());

        byte[] bytes = new byte[1];
        System.out.println(bytes.getClass());
        System.out.println(bytes.getClass().getSuperclass());
    }
}

class Demo3 {
    static {
        System.out.println(Demo2.class.getName());
    }
}
