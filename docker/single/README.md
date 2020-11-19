```shell
docker-compose up -d
```

修改host文件

```
127.0.0.1    test.iliubang.cn
```

浏览器访问:`http://localhost:8500`来查看consul中注册的服务信息，`http://test.iliubang.cn/ok`测试test_service服务正常运行

将`test_server`动态增加到3个节点

```shell
docker-compose up --scale test_server=3
```
