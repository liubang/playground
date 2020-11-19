package cn.iliubang.exercises.mtomcat.handler;


import cn.iliubang.exercises.mtomcat.entity.Entity;
import cn.iliubang.exercises.mtomcat.entity.Mapping;
import org.xml.sax.Attributes;
import org.xml.sax.SAXException;
import org.xml.sax.helpers.DefaultHandler;

import java.util.ArrayList;
import java.util.List;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/31
 */
public class WebHandler extends DefaultHandler {

    private List<Entity> entityList;
    private List<Mapping> mappingList;
    private Entity entity;
    private Mapping mapping;

    private String beginTag;

    private boolean isMap = false;


    @Override
    public void startDocument() throws SAXException {
        //文档解析开始
        entityList = new ArrayList<>();
        mappingList = new ArrayList<>();
    }

    @Override
    public void startElement(String uri, String localName, String qName, Attributes attributes) throws SAXException {
        //开始标签
        if (null != qName) {
            beginTag = qName;
            if (qName.equals("servlet")) {
                entity = new Entity();
            } else if (qName.equals("servlet-mapping")) {
                mapping = new Mapping();
                isMap = true;
            }
        }
    }

    @Override
    public void characters(char[] ch, int start, int length) throws SAXException {
        //处理内容
        if (null != beginTag) {
            String str = new String(ch, start, length);
            if (isMap) {
                if (beginTag.equals("servlet-name")) {
                    mapping.setName(str);
                } else if (beginTag.equals("url-pattern")) {
                    mapping.getUrlPattarn().add(str);
                }
            } else {
                if (beginTag.equals("servlet-name")) {
                    entity.setName(str);
                } else if (beginTag.equals("servlet-class")) {
                    entity.setClazz(str);
                }
            }
        }
    }

    @Override
    public void endElement(String uri, String localName, String qName) throws SAXException {
        //结束标签
        if (null != qName) {
            if (qName.equals("servlet")) {
                entityList.add(entity);
            } else if (qName.equals("servlet-mapping")) {
                mappingList.add(mapping);
            }
        }
        beginTag = null;
    }

    @Override
    public void endDocument() throws SAXException {
        //文档解析结束
    }

    public List<Entity> getEntityList() {
        return entityList;
    }

    public List<Mapping> getMappingList() {
        return mappingList;
    }

}
