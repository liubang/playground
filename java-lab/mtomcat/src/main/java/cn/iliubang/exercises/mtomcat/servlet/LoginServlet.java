package cn.iliubang.exercises.mtomcat.servlet;


import cn.iliubang.exercises.mtomcat.Servlet;
import cn.iliubang.exercises.mtomcat.request.Request;
import cn.iliubang.exercises.mtomcat.response.Response;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */
public class LoginServlet extends Servlet {
    @Override
    public void doGet(Request request, Response response) throws Exception {
        response.print("<!doctype html>\n" +
                "<html lang=\"en\">\n" +
                "<head>\n" +
                "    <meta charset=\"UTF-8\">\n" +
                "    <meta name=\"viewport\"\n" +
                "          content=\"width=device-width, user-scalable=no, initial-scale=1.0, maximum-scale=1.0, minimum-scale=1.0\">\n" +
                "    <meta http-equiv=\"X-UA-Compatible\" content=\"ie=edge\">\n" +
                "    <title>server</title>\n" +
                "</head>\n" +
                "<body>\n" +
                "<h1>hello world</h1>\n" +
                "</body>\n" +
                "</html>");
    }

    @Override
    public void doPost(Request request, Response response) throws Exception {
        String username = request.getPost("username");
        String password = request.getPost("password");
        if (username.equals("liubang") && password.equals("admin")) {
            response.print("<html>登录成功</html>");
        } else {
            response.print("<html>登录失败</html>");
        }
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
