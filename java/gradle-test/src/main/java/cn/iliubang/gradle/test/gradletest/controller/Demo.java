package cn.iliubang.gradle.test.gradletest.controller;

import cn.iliubang.gradle.test.gradletest.exception.LogicException;
import cn.iliubang.gradle.test.gradletest.vo.Response;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RestController;

import java.util.concurrent.atomic.AtomicInteger;

@RestController
public class Demo {

    private final AtomicInteger i = new AtomicInteger(0);

    @GetMapping(path = "/index")
    public Response<String> index() {
        if (i.addAndGet(1) % 2 == 0) {
            throw new LogicException(400, "exception");
        } else {
            return Response.<String>builder().code(200).message("OK").data("string").build();
        }
    }
}