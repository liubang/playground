package main

import (
	"fmt"
	"time"
)

func g() {
	panic("hahah")
}

func main() {
	fmt.Println("start")

	defer func() {
		if r := recover(); r != nil {
			fmt.Printf("%v\n", r)
		}
	}()

	g()

	time.Sleep(time.Second * 2)
	fmt.Println("end")
}
