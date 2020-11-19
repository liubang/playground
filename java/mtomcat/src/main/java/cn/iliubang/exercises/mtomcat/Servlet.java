package cn.iliubang.exercises.mtomcat;


import cn.iliubang.exercises.mtomcat.request.Request;
import cn.iliubang.exercises.mtomcat.response.Response;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */
public abstract class Servlet {

    public void service(Request request, Response response) throws Exception {
        switch (request.getMethod()) {
            case GET:
                doGet(request, response);
                break;
            case POST:
                doPost(request, response);
                break;
            case PUT:
                doPut(request, response);
                break;
            case DELETE:
                doDelete(request, response);
                break;
            case HEAD:
                doHead(request, response);
                break;
        }
    }


    public abstract void doGet(Request request, Response response) throws Exception;

    public abstract void doPost(Request request, Response response) throws Exception;

    public abstract void doPut(Request request, Response response) throws Exception;

    public abstract void doDelete(Request request, Response response) throws Exception;

    public abstract void doHead(Request request, Response response) throws Exception;
}
