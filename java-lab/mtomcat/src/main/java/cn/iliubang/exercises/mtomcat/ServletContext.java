package cn.iliubang.exercises.mtomcat;

import java.util.HashMap;
import java.util.Map;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */
public class ServletContext {
    //为每一个servlet取一个别名
    //name -> servlet
    private Map<String, String> servlet;
    //uri -> servlet
    private Map<String, String> mapping;

    public ServletContext() {
        servlet = new HashMap<>();
        mapping = new HashMap<>();
    }

    public Map<String, String> getServlet() {
        return servlet;
    }

    public Map<String, String> getMapping() {
        return mapping;
    }

    public void setMapping(Map<String, String> mapping) {
        this.mapping = mapping;
    }
}
