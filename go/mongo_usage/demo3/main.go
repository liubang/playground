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
