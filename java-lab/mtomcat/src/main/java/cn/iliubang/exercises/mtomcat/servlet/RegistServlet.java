package cn.iliubang.exercises.mtomcat.servlet;


import cn.iliubang.exercises.mtomcat.Servlet;
import cn.iliubang.exercises.mtomcat.request.Request;
import cn.iliubang.exercises.mtomcat.response.Response;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */
public class RegistServlet extends Servlet {
    @Override
    public void doGet(Request request, Response response) throws Exception {
        response.print("<h1>注册</h1>");
    }

    @Override
    public void doPost(Request request, Response response) throws Exception {
        response.print("<h1>POST 注册</h1>");
    }

    @Override
    public void doPut(Request request, Response response) throws Exception {

    }

    @Override
    public void doDelete(Request request, Response response) throws Exception {

    }

    @Override
    public void doHead(Request request, Response response) throws Exception {

    }
}
