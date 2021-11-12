package main

import (
	"log"
	"time"
)

func main() {
	data := "2021-11-01T03:08:30Z"
	// t, _ := time.Parse("2006-01-02T15:04:05Z", data)
	// log.Println(t)
	t, _ := time.Parse(time.RFC3339, data)
	// log.Println(t)
	log.Println(t.In(time.Local))
	// t, _ = time.ParseInLocation(time.RFC3339, data, time.Local)
	// log.Println(t)
	log.Println(time.Now())
}
