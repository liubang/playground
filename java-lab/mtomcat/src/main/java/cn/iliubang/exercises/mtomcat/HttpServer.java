package cn.iliubang.exercises.mtomcat;

import java.io.IOException;
import java.net.ServerSocket;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/30
 */
public class HttpServer {

    private ServerSocket serverSocket;
    private boolean isStart = true;

    public static void main(String[] args) {
        HttpServer server = new HttpServer();
        server.start();
    }

    public void start() {
        start(8080);
    }

    public void start(int port) {
        try {
            serverSocket = new ServerSocket(port);
            while (isStart)
                this.receive();
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    /**
     * 接收客户端
     */
    private void receive() {
        try {
            new Thread(new Dispatcher(serverSocket.accept())).start();
        } catch (IOException e) {
            stop();
            e.printStackTrace();
        }
    }

    /**
     * 停止
     */
    public void stop() {
        isStart = false;
    }
}
