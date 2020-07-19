package main

import (
	"context"
	"fmt"
	"os/exec"
	"time"
)

type result struct {
	err    error
	output []byte
}

func main() {
	// 在协程里执行一个命令，执行2秒后杀死程序

	var (
		ctx        context.Context
		cancelFunc context.CancelFunc
		cmd        *exec.Cmd
		resultChan chan *result
		res        *result
	)

	// 创建一个结果队列
	resultChan = make(chan *result, 1000)

	ctx, cancelFunc = context.WithCancel(context.TODO())

	go func() {
		var (
			output []byte
			err    error
		)
		cmd = exec.CommandContext(ctx, "bash", "-c", "sleep 2; echo hello;")
		output, err = cmd.CombinedOutput()
		// 把任务输出结果传给main协程
		resultChan <- &result{
			err:    err,
			output: output,
		}
	}()

	time.Sleep(1 * time.Second)
	cancelFunc()
	res = <-resultChan
	fmt.Println(res.err, string(res.output))
}
