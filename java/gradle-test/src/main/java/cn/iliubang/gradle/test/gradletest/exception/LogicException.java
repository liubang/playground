package cn.iliubang.gradle.test.gradletest.exception;

import lombok.AllArgsConstructor;
import lombok.Data;
import lombok.EqualsAndHashCode;

@Data
@AllArgsConstructor
@EqualsAndHashCode(callSuper = true)
public class LogicException extends RuntimeException {
    private int code;
    private String message;
}
