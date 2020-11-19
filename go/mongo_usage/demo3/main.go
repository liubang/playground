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
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"
	"time"
)

type TimeBeforeCond struct {
	Before int64 `bson:"$lt"`
}

type DeleteCond struct {
	beforeCond TimeBeforeCond `bson:"timePoint.startTime"`
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

	// 4. 删除开始时间早于当前时间的所有日志
	delCond := &DeleteCond{
		beforeCond: TimeBeforeCond{
			Before: time.Now().Unix(),
		},
	}

	delResult, err := collection.DeleteMany(context.TODO(), delCond)

	if err != nil {
		fmt.Println(err)
		return
	}

	fmt.Println("删除的行数:", delResult.DeletedCount)
}
