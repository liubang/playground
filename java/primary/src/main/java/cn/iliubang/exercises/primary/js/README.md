### 脚本引擎执行JavaScript代码

- Java脚本引擎从JDK6.0之后添加的新功能

#### 脚本引擎介绍

使得Java应用可以通过一套固定的接口与各种脚本引擎交互，从而达到在Java平台上调用各种脚本语言的目的

Java脚本API是连通Java平台和脚本语言的桥梁

可以把一些复杂易变的业务逻辑交给脚本语言处理，这又大大提高了开发效率

#### 使用方法

```java
ScriptEngineManager scriptEngineManager = new ScriptEngineManager();
ScriptEngine scriptEngine = scriptEngineManager.getEngineByName("js");
// 定义变量
scriptEngine.put("msg", "hello world");
String js =
        "var console = {" +
        "    log: print," +
        "    warn: print," +
        "    error: print" +
        "};";

js += "var user = {name: 'liubang', age: 24};";
js += "console.log(user.name);";
js += "user.desc = msg;";
js += "console.log(user.desc);";

//执行js脚本
try {
    scriptEngine.eval(js);
    
    //JAVA和js代码交互
    Bindings obj = (Bindings) scriptEngine.eval("user;");
    Integer age = (Integer)obj.get("age");
    String name = (String) obj.get("name");
    System.out.println(name + "::" + age);
    
    //执行js函数
    scriptEngine.eval("function add(a, b) {return a + b;}");
    Invocable invocable = (Invocable) scriptEngine;
    Object result = invocable.invokeFunction("add", new Object[]{2017, 1});
    System.out.println(result);
    
    //js中导入JAVA包，使用JAVA类
    js =
            "var Arrays = Java.type('java.util.Arrays');" +
            "var list = Arrays.asList([\"刘邦\", \"新年好\"]);";
    scriptEngine.eval(js);
    List list = (List)scriptEngine.get("list");
    System.out.println(list);
} catch (ScriptException e) {
    e.printStackTrace();
}
```

**参考文档**

- [Java 8 Nashorn Tutorial](http://winterbe.com/posts/2014/04/05/java8-nashorn-tutorial/)