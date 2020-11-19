package main

import (
	"context"
	"fmt"
	"github.com/coreos/etcd/mvcc/mvccpb"
	"go.etcd.io/etcd/clientv3"
	"time"
)

func main() {

	var (
		config  clientv3.Config
		client  *clientv3.Client
		kv      clientv3.KV
		delResp *clientv3.DeleteResponse
		kvpair  *mvccpb.KeyValue
		err     error
	)

	config = clientv3.Config{
		Endpoints:   []string{"localhost:2379"},
		DialTimeout: 5 * time.Second,
	}

	if client, err = clientv3.New(config); err != nil {
		fmt.Println(err)
		return
	}

	kv = clientv3.NewKV(client)

	if delResp, err = kv.Delete(context.TODO(), "/cron/jobs/", clientv3.WithPrevKV(), clientv3.WithPrefix()); err != nil {
		fmt.Println(err)
	} else {

		// 被删除之前的value是什么
		if len(delResp.PrevKvs) != 0 {
			for _, kvpair = range delResp.PrevKvs {
				fmt.Println("删除了：", string(kvpair.Key), ",", string(kvpair.Value))
			}
		}
	}
}
