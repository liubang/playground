package cn.iliubang.exercises.primary.annotation.some;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/1
 */

@Target(value = {ElementType.METHOD, ElementType.CONSTRUCTOR})
@Retention(RetentionPolicy.RUNTIME)
public @interface MyAnnotation02 {
}
