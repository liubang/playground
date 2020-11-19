package cn.iliubang.exercises.primary.annotation.some;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */

@Target(ElementType.METHOD)
@Retention(RetentionPolicy.RUNTIME)
public @interface MyAnnotation01 {

    String name() default "";

    int id() default -1;

    String[] offers() default {"百度", "腾讯", "阿里"};

}
