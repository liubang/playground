package main

import (
	"log"

	"github.com/robfig/cron/v3"
)

func main() {
	// put your code hare
	c := cron.New()

	c.AddFunc("@every 1s", func() {
		log.Println("OK")
	})

	c.Start()

	select {}
}
