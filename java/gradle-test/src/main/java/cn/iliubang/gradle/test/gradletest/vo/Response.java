package cn.iliubang.gradle.test.gradletest.vo;

import lombok.Builder;
import lombok.Data;

@Data
@Builder
public class Response<T> {
    private String message;
    private int code;
    private T data;
}
