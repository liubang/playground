使用[https://github.com/iliubang/vagrant-boxs/tree/master/docker-vagrant](https://github.com/iliubang/vagrant-boxs/tree/master/docker-vagrant)快速创建两个ip分别为192.168.8.20和192.168.8.21和192.168.8.22的虚拟机

然后在8.20上执行

```shell
docker-compose -f node-820.yml up
```

在8.21上执行

```shell
docker-compose -f node-821.yml up
```

在8.22上执行

```shell
docker-compose -f node-822.yml up
```

绑定host

```
192.168.8.20   test.iliubang.cn
```


浏览器访问`http://test.iliubang.cn/ok`
