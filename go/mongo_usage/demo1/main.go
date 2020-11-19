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
	"context"
	"fmt"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"
	"time"
)

// 任务执行时间点
type TimePoint struct {
	StartTime int64 `bson:"startTime"`
	EndTime   int64 `bson:"endTime"`
}

// 一条日志
type LogRecord struct {
	JobName   string    `bson:"jobName"`   // 任务名
	Command   string    `bson:"command"`   // 命令
	Err       string    `bson:"err"`       // 错误信息
	Content   string    `bson:"content"`   // 标准输出
	TimePoint TimePoint `bson:"timePoint"` // 执行时间点
}

func main() {
	// 1. 建立连接
	client, err := mongo.Connect(context.TODO(), options.Client().
		ApplyURI("mongodb://mongodb-dev:27017").
		SetConnectTimeout(5*time.Second))

	if err != nil {
		fmt.Println(err)
		return
	}

	// 2. 选择数据库
	database := client.Database("cron")

	// 3. 选择表
	collection := database.Collection("log")

	// 4. 插入记录
	record := &LogRecord{
		JobName: "job10",
		Command: "echo hello",
		Err:     "",
		Content: "hello",
		TimePoint: TimePoint{
			StartTime: time.Now().Unix(),
			EndTime:   time.Now().Unix(),
		},
	}

	insertOneResult, err := collection.InsertOne(context.TODO(), record)

	if err != nil {
		fmt.Println(err)
		return
	}

	id := insertOneResult.InsertedID.(primitive.ObjectID).Hex()
	fmt.Println(id)
}
