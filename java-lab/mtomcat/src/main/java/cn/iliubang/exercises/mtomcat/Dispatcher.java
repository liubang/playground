package cn.iliubang.exercises.mtomcat;


import cn.iliubang.exercises.mtomcat.request.Request;
import cn.iliubang.exercises.mtomcat.response.Response;
import cn.iliubang.exercises.mtomcat.response.ResponseStatus;

import java.io.IOException;
import java.net.Socket;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */
public class Dispatcher implements Runnable {

    private Socket client;
    private Request request;
    private Response response;
    private ResponseStatus status = ResponseStatus.OK;

    public Dispatcher(Socket client) {
        this.client = client;
        try {
            this.request = new Request(this.client.getInputStream());
            this.response = new Response(this.client.getOutputStream());
        } catch (IOException e) {
            status = ResponseStatus.InternalServerError;
        }
    }

    @Override
    public void run() {
        Servlet servlet = WebApp.getServlet(request.getUri());
        if (null == servlet) {
            status = ResponseStatus.NotFound;
        } else {
            try {
                servlet.service(request, response);
            } catch (Exception e) {
                status = ResponseStatus.InternalServerError;
                e.printStackTrace();
            }

        }
        response.send(status);
        response.close();
    }
}
