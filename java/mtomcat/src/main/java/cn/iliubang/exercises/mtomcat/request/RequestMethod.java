package cn.iliubang.exercises.mtomcat.request;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */
public enum RequestMethod {

    GET("GET"), POST("POST"), PUT("PUT"), DELETE("DELETE"), HEAD("HEAD"), UNKNOWN("UNKNOWN");

    private String name;

    private RequestMethod(String name) {
        this.name = name;
    }

    public void setName(String name) {
        this.name = name;
    }
}
