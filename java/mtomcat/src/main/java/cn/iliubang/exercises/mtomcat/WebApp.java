package cn.iliubang.exercises.mtomcat;

import cn.iliubang.exercises.mtomcat.entity.Entity;
import cn.iliubang.exercises.mtomcat.entity.Mapping;
import cn.iliubang.exercises.mtomcat.handler.WebHandler;
import org.xml.sax.SAXException;

import javax.xml.parsers.ParserConfigurationException;
import javax.xml.parsers.SAXParser;
import javax.xml.parsers.SAXParserFactory;
import java.io.IOException;
import java.util.List;
import java.util.Map;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */
public class WebApp {

    private static ServletContext context;

    static {
        context = new ServletContext();
        try {
            SAXParserFactory factory = SAXParserFactory.newInstance();
            SAXParser parser = factory.newSAXParser();

            //执行xml+handler
            WebHandler handler = new WebHandler();
            parser.parse(Thread.currentThread().getContextClassLoader().getResourceAsStream("cn/iliubang/exercises/mtomcat/web.xml"), handler);

            Map<String, String> servlet = context.getServlet();
            for (Entity entity : handler.getEntityList()) {
                servlet.put(entity.getName(), entity.getClazz());
            }

            Map<String, String> mapping = context.getMapping();
            for (Mapping map : handler.getMappingList()) {
                List<String> urls = map.getUrlPattarn();
                for (String url : urls) {
                    mapping.put(url, map.getName());
                }
            }

        } catch (ParserConfigurationException e) {
            e.printStackTrace();
        } catch (SAXException e) {
            e.printStackTrace();
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    public static Servlet getServlet(String url) {
        if (null == url || (url = url.trim()).equals("")) {
            return null;
        }
        String className = context.getServlet().get(context.getMapping().get(url));
        if (null == className) {
            return null;
        }

        try {
            Class<?> clazz = Class.forName(className);
            try {
                return (Servlet) clazz.newInstance();
            } catch (InstantiationException e) {
                e.printStackTrace();
            } catch (IllegalAccessException e) {
                e.printStackTrace();
            }
        } catch (ClassNotFoundException e) {
            e.printStackTrace();
        }

        return null;
    }
}
