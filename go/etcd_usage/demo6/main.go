package main

import (
	"context"
	"fmt"
	"go.etcd.io/etcd/clientv3"
	"time"
)

func main() {

	var (
		config         clientv3.Config
		client         *clientv3.Client
		lease          clientv3.Lease
		leaseGrantResp *clientv3.LeaseGrantResponse
		leaseId        clientv3.LeaseID
		kv             clientv3.KV
		putResp        *clientv3.PutResponse
		getResp        *clientv3.GetResponse
		keepResp       *clientv3.LeaseKeepAliveResponse
		keepRespChan   <-chan *clientv3.LeaseKeepAliveResponse
		err            error
	)

	config = clientv3.Config{
		Endpoints:   []string{"localhost:2379"},
		DialTimeout: 5 * time.Second,
	}

	if client, err = clientv3.New(config); err != nil {
		fmt.Println(err)
		return
	}

	// 申请一个lease（租约）
	lease = clientv3.NewLease(client)
	// 申请一个10秒的租约
	if leaseGrantResp, err = lease.Grant(context.TODO(), 10); err != nil {
		fmt.Println(err)
	} else {
		// 拿到租约ID
		leaseId = leaseGrantResp.ID
		// 自动续租
		if keepRespChan, err = lease.KeepAlive(context.TODO(), leaseId); err != nil {
			fmt.Println(err)
			return
		}

		go func() {
			for {
				select {
				case keepResp = <-keepRespChan:
					if keepRespChan == nil {
						fmt.Println("租约已经失效了")
						goto END
					} else {
						// 每秒会续租一次
						fmt.Println("收到自动续租应答：", keepResp.ID)
					}
				}
			}
		END:
		}()

		kv = clientv3.NewKV(client)

		// put一个kv，让它与租约关联起来
		if putResp, err = kv.Put(context.TODO(), "/cron/lock/job1", "", clientv3.WithLease(leaseId)); err != nil {
			fmt.Println(err)
		} else {
			fmt.Println("写入成功: ", putResp.Header.Revision)

		}

		// 定时看key过期没有
		for {
			if getResp, err = kv.Get(context.TODO(), "/cron/lock/job1"); err != nil {
				fmt.Println(err)
			} else {
				if getResp.Count == 0 {
					fmt.Println("kv过期了")
					break
				} else {
					fmt.Println("还没过期：", getResp.Kvs)
					time.Sleep(2 * time.Second)
				}
			}
		}
	}

}
