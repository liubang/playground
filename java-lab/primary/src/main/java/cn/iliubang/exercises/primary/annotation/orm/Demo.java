package cn.iliubang.exercises.primary.annotation.orm;

import cn.iliubang.exercises.primary.annotation.orm.annotation.Table;

import java.lang.annotation.Annotation;
import java.lang.reflect.Field;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */

/*
 * 使用反射读取注解的信息，模拟处理信息的流程
 */
public class Demo {
    public static void main(String[] args) {
        try {

            Class clazz = Class.forName("cn.iliubang.exercises.primary.annotation.orm.entity.Student");
            // 获取该类的所有注解
            Annotation[] annotations = clazz.getAnnotations();
            // 判断该类是否有某注解
            if (clazz.isAnnotationPresent(Table.class)) {
                // 获取特定注解
                Table table = (Table) clazz.getAnnotation(Table.class);
                System.out.println(table.value());
            }

            // 获取类的属性的注解
            Field[] fields = clazz.getDeclaredFields();
            for (Field field : fields) {
                if (field.isAnnotationPresent(cn.iliubang.exercises.primary.annotation.orm.annotation.Field.class)) {
                    cn.iliubang.exercises.primary.annotation.orm.annotation.Field field1 = field.getAnnotation(cn.iliubang.exercises.primary.annotation.orm.annotation.Field.class);
                    System.out.println(field.getName() + ":" + field1.columnName() + "===>" + field1.type());
                }
            }
        } catch (ClassNotFoundException e) {
            e.printStackTrace();
        }
    }
}
