package cn.iliubang.gradle.test.gradletest.aspect;

import cn.iliubang.gradle.test.gradletest.exception.LogicException;
import cn.iliubang.gradle.test.gradletest.vo.Response;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.ControllerAdvice;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.context.request.WebRequest;
import org.springframework.web.servlet.mvc.method.annotation.ResponseEntityExceptionHandler;

import java.util.Optional;

@ControllerAdvice
public class ErrorHandler extends ResponseEntityExceptionHandler {

    @ExceptionHandler({LogicException.class})
    public ResponseEntity<Response<String>> handleLogicException(LogicException ex, WebRequest request) {
        System.out.println("===========================================");
        System.out.println("this is exception");
        System.out.println("===========================================");
        Response<String> resp = Response.<String>builder().code(ex.getCode()).message(ex.getMessage()).build();
        return ResponseEntity.of(Optional.of(resp));
    }
}
