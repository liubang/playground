package main

import (
	"context"
	"fmt"
	"go.etcd.io/etcd/clientv3"
	"time"
)

func main() {
	var (
		config clientv3.Config
		client *clientv3.Client
		kv     clientv3.KV
		putOp  clientv3.Op
		getOp  clientv3.Op
		opResp clientv3.OpResponse
		err    error
	)

	// 客户端配置
	config = clientv3.Config{
		Endpoints:   []string{"localhost:2379"},
		DialTimeout: 5 * time.Second,
	}

	if client, err = clientv3.New(config); err != nil {
		fmt.Println(err)
		return
	}

	kv = clientv3.NewKV(client)
	// 创建一个op
	putOp = clientv3.OpPut("/cron/jobs/job8", "dsfsadd")

	// 执行op
	if opResp, err = kv.Do(context.TODO(), putOp); err != nil {
		fmt.Println(err)
		return
	}

	fmt.Println("写入Revision:", opResp.Put().Header.Revision)

	// 创建一个get操作
	getOp = clientv3.OpGet("/cron/jobs/job8")
	// 执行op
	if opResp, err = kv.Do(context.TODO(), getOp); err != nil {
		fmt.Println(err)
		return
	}

	fmt.Println("数据Revision:", opResp.Get().Kvs[0].ModRevision)
	fmt.Println("数据value:", string(opResp.Get().Kvs[0].Value))
}
