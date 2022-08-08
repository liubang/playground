package cn.iliubang.exercises.mtomcat.request;

import java.io.IOException;
import java.io.InputStream;
import java.io.UnsupportedEncodingException;
import java.net.URLDecoder;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.StringTokenizer;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/30
 */
public class Request {
    private static final String CRLF = "\r\n";

    private RequestMethod method;

    private String uri;

    private String protocol;

    private Map<String, List<String>> query;

    private Map<String, List<String>> post;

    private Map<String, String> headers;

    private String rowContent;

    private InputStream inputStream;

    public Request() {
        method = RequestMethod.UNKNOWN;
        uri = "";
        query = new HashMap<>();
        headers = new HashMap<>();
        post = new HashMap<>();
        rowContent = "";
    }

    public Request(InputStream inputStream) {
        this();
        this.inputStream = inputStream;
        try {
            byte[] data = new byte[2048];
            int len = inputStream.read(data);
            if (len > 0) {
                rowContent = new String(data, 0, len);
            }
        } catch (IOException e) {
            return;
        }
        parseRequestInfo();
    }

    private void parseRequestInfo() {
        if (null == rowContent || (rowContent = rowContent.trim()).equals("")) {
            return;
        }
        String firstLine = rowContent.substring(0, rowContent.indexOf(CRLF));
        int t1 = firstLine.indexOf(" ");
        // method
        parseRequestMethod(firstLine.substring(0, t1).trim().toUpperCase());

        int t2 = firstLine.lastIndexOf(" ");
        String uriStr = firstLine.substring(t1 + 1, t2).trim();
        try {
            uriStr = URLDecoder.decode(uriStr, "utf-8");
        } catch (UnsupportedEncodingException e) {
            e.printStackTrace();
        }

        if (uriStr.contains("?")) {
            String[] uriArr = uriStr.split("\\?");
            uri = uriArr[0];
            // query
            parseParams(uriArr[1], this.query);

        } else {
            uri = uriStr;
        }
        // protocol
        this.protocol = firstLine.substring(t2 + 1).trim();
        t2 = rowContent.indexOf(CRLF, t2 + 1);

        // header
        String currentLine = null;
        int step = 0;
        int rowContentLen = rowContent.getBytes().length;
        for (; ; ) {
            t2 += 2;
            if (t2 > rowContentLen) {
                break;
            }
            step = rowContent.indexOf(CRLF, t2);
            if (step < 0) {
                break;
            }
            currentLine = rowContent.substring(t2, step).trim();
            t2 = step;
            if (!currentLine.equals("")) {
                parseHeader(currentLine);
            }
        }

        // content
        if (t2 + 2 < rowContentLen) {
            currentLine = rowContent.substring(t2).trim();
            if (!currentLine.equals("")) {
                parseParams(currentLine, this.post);
            }
        }

    }

    private void parseRequestMethod(String m) {
        if (m.equals("GET"))
            method = RequestMethod.GET;
        else if (m.equals("POST"))
            method = RequestMethod.POST;
        else if (m.equals("PUT"))
            method = RequestMethod.PUT;
        else if (m.equals("DELETE"))
            method = RequestMethod.DELETE;
        else
            method = RequestMethod.UNKNOWN;
    }

    private void parseHeader(String header) {
        if (header.contains(":")) {
            String[] headers = header.split(":");
            String key = headers[0].trim();
            String val = null;
            if (headers.length > 1) {
                val = headers[1].trim();
            }
            this.headers.put(key, val);
        }
    }

    private void parseParams(String str, Map<String, List<String>> map) {
        StringTokenizer token = new StringTokenizer(str, "&");
        while (token.hasMoreTokens()) {
            String keyValue = token.nextToken();
            String[] keyValues = keyValue.split("=");
            String key = null;
            String value = null;
            if (2 == keyValues.length) {
                key = keyValues[0];
                value = keyValues[1];
            } else if (1 == keyValues.length) {
                key = keyValues[0];
            }
            assert key != null;
            if (key.contains("[")) {
                key = key.substring(0, key.indexOf("["));
            }
            if (map.containsKey(key)) {
                map.get(key).add(value);
            } else {
                List<String> list = new ArrayList<>();
                list.add(value);
                map.put(key, list);
            }
        }
    }

    public RequestMethod getMethod() {
        return method;
    }

    public String getUri() {
        return uri;
    }

    public String getProtocol() {
        return protocol;
    }

    public Map<String, List<String>> getQuery() {
        return query;
    }

    public Map<String, List<String>> getPost() {
        return post;
    }

    public String getPost(String key) {
        List<String> list = post.get(key);
        if (null == list) {
            return null;
        }
        return list.get(0);
    }

    public List<String> getPosts(String key) {
        List<String> list = post.get(key);
        return list;
    }

    public String getQuery(String key) {
        List<String> list = query.get(key);
        if (null == list) {
            return null;
        }
        return list.get(0);
    }

    public List<String> getQuerys(String key) {
        List<String> list = query.get(key);
        return list;
    }

    public Map<String, String> getHeaders() {
        return headers;
    }

    public String getRowContent() {
        return rowContent;
    }
}
