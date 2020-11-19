// Copyright (c) 2020 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2020/07/20 00:32

package main

import (
	"fmt"
	"github.com/gorhill/cronexpr"
	"time"
)

type CronJob struct {
	expr     *cronexpr.Expression
	nextTime time.Time
}

func main() {
	// 需要有1个调度协程，定时检查所有cron任务，谁过期了调度谁

	var (
		cronJob       *CronJob
		expr          *cronexpr.Expression
		now           time.Time
		scheduleTable map[string]*CronJob // key: 任务名字
	)

	scheduleTable = make(map[string]*CronJob)

	// 当前时间
	now = time.Now()

	// 定义两个job
	expr = cronexpr.MustParse("*/5 * * * * * *")
	cronJob = &CronJob{
		expr:     expr,
		nextTime: expr.Next(now),
	}
	// 任务注册到任务表中
	scheduleTable["job1"] = cronJob

	expr = cronexpr.MustParse("*/6 * * * * * *")
	cronJob = &CronJob{
		expr:     expr,
		nextTime: expr.Next(now),
	}
	// 任务注册到任务表中
	scheduleTable["job2"] = cronJob

	// 启动调度协程
	go func() {
		var (
			jobName string
			cronJob *CronJob
			now     time.Time
		)
		// 定时检查
		for {
			now = time.Now()
			for jobName, cronJob = range scheduleTable {
				// 判断是否过期
				if cronJob.nextTime.Before(now) || cronJob.nextTime.Equal(now) {
					// 启动一个协程
					go func(jobName string) {
						fmt.Println("执行:", jobName)
					}(jobName)

					// 计算下一次的调度时间
					cronJob.nextTime = cronJob.expr.Next(now)
					fmt.Println(jobName, "下次执行时间：", cronJob.nextTime)
				}
			}

			// 睡眠100毫秒
			select {
			case <-time.NewTimer(100 * time.Millisecond).C:
			}
			// 或者
			// time.Sleep(100 * time.Millisecond)
		}
	}()

	time.Sleep(100 * time.Second)
}
