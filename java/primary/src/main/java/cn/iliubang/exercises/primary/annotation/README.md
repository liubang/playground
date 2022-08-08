使用`@interface`自定义注解时，自动继承了`java.lang.annotation.Annotation`接口

**要点**

- `@interface`用来声明一个注解，格式为`public @interface 注解名 {定义体}`
- 其中的每一个方法实际上是声明了一个配置参数
-
    - 方法名称就是参数名称
-
    - 返回值类型就是参数类型（返回值类型只能是基本类型，Class, String, enum）
-
    - 可以通过`default`来声明参数默认值
-
    - 如果只有一个参数成员，一般参数名为`value`

**元注解**

元注解的作用就是负责注解其他注解。Java定义了4个标准的`meta-annotation`类型，它们被用来提供对其它annotation类型作说明

这些类型和他们所支持的类在`java.lang.annotation`包中可以找到

- `@Target`
- `@Retention`
- `@Documented`
- `@Inherited`

**注意**

- 注解元素必须要有值。我们定义注解元素的时候，经常使用空字符串，0作为默认值
- 也经常使用负数（例如：-1）表示不存在的含义

#### `@Target`

**作用** 用于描述注解的使用范围（即：被描述的注解可以用在什么地方）

| 所修饰范围                                     | 取值 ElementType                                                       |
| ---------------------------------------------- | ---------------------------------------------------------------------- |
| package包                                      | PACKAGE                                                                |
| 类、接口、枚举、Annotation类型                 | TYPE                                                                   |
| 类型成员（方法、构造方法、成员变量、枚举值）   | CONSTRUCTOR: 用于描述构造器, FIELD: 用于描述域, METHOD: 用于描述方法   |
| 方法参数和本地变量                             | LOCAL_VARIABLE: 描述局部变量, PARAMETER: 用于描述参数                  |

#### `@Retention`

**作用** 表示需要在什么级别保存该注解信息，用于描述注解的生命周期

| 取值RetentionPolicy  | 作用                                  |
| -------------------- | ------------------------------------- |
| SOURCE               | 在源文件中有效                        |
| CLASS                | 在class文件中有效                     |
| RUNTIME              | 在运行时有效，为Runtime可以被反射读取 |

### 使用反射读取注解信息

```java
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
                Table table =  (Table) clazz.getAnnotation(Table.class);
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
```
