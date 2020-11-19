package main

import (
	"context"
	"fmt"
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

// JobName过滤条件
type FindByJobName struct {
	JobName string `bson:"jobName"`
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

	// 4. 按照jobname字段过滤
	cond := &FindByJobName{
		JobName: "job10",
	}
	cursor, err := collection.Find(context.TODO(), cond, options.Find().SetSkip(0), options.Find().SetLimit(2))

	if err != nil {
		fmt.Println(err)
		return
	}

	// 释放游标
	defer func() {
		fmt.Println("释放游标")
		if err := cursor.Close(context.TODO()); err != nil {
			fmt.Println(err)
		}
	}()

	// 遍历结果集，并反序列化
	for cursor.Next(context.TODO()) {
		record := &LogRecord{}
		if err := cursor.Decode(record); err != nil {
			fmt.Println(err)
			return
		}

		fmt.Println(*record)
	}

}
