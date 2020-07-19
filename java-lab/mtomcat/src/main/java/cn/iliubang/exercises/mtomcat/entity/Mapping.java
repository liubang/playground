package cn.iliubang.exercises.mtomcat.entity;

import java.util.ArrayList;
import java.util.List;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */
public class Mapping {

    private String name;

    private List<String> urlPattarn;

    public Mapping() {
        urlPattarn = new ArrayList<>();
    }

    public String getName() {
        return name;
    }

    public void setName(String name) {
        this.name = name;
    }

    public List<String> getUrlPattarn() {
        return urlPattarn;
    }

    public void setUrlPattarn(List<String> urlPattarn) {
        this.urlPattarn = urlPattarn;
    }
}
