package cn.iliubang.exercises.primary.annotation.orm.entity;

import cn.iliubang.exercises.primary.annotation.orm.annotation.Field;
import cn.iliubang.exercises.primary.annotation.orm.annotation.Table;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */

@Table("tb_student")
public class Student {

    @Field(columnName = "id", type = "int")
    private int id;

    @Field(columnName = "name", type = "varchar")
    private String name;

    @Field(columnName = "age", type = "int")
    private int age;

    public int getId() {
        return id;
    }

    public void setId(int id) {
        this.id = id;
    }

    public String getName() {
        return name;
    }

    public void setName(String name) {
        this.name = name;
    }

    public int getAge() {
        return age;
    }

    public void setAge(int age) {
        this.age = age;
    }
}
