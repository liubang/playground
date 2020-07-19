package cn.iliubang.exercises.mtomcat.response;

import java.io.BufferedWriter;
import java.io.IOException;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.util.Date;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/30
 */
public class Response {
    private static final String CRLF = "\r\n";
    private StringBuilder headerInfo;
    private StringBuilder content;
    private BufferedWriter bufferedWriter;
    private int len = 0;

    public Response() {
        headerInfo = new StringBuilder();
        content = new StringBuilder();
    }

    public Response(OutputStream outputStream) {
        this();
        this.bufferedWriter = new BufferedWriter(new OutputStreamWriter(outputStream));
    }

    private void createHeaderInfo(ResponseStatus status) {
        headerInfo.append("HTTP/1.1 " + status.getCode() + " " + status.getDesc()).append(CRLF)
                .append("HttpServer: Apache Tomcat 9").append(CRLF)
                .append("Date: ").append(new Date()).append(CRLF)
                .append("Content-Type: text/html;charset=utf-8").append(CRLF)
                .append("Content-Length: ").append(len).append(CRLF)
                .append(CRLF);
    }

    public Response print(String content) {
        this.content.append(content);
        this.len += content.getBytes().length;
        return this;
    }

    public void send(ResponseStatus status) {
        createHeaderInfo(status);
        try {
            bufferedWriter.append(headerInfo.toString());
            bufferedWriter.append(content.toString());
            bufferedWriter.flush();
        } catch (IOException e) {
            e.printStackTrace();
        } finally {
            close();
        }
    }

    public void close() {
        try {
            bufferedWriter.close();
        } catch (IOException e) {
            e.printStackTrace();
        }
    }
}
