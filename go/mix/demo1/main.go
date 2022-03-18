package main

import (
	"fmt"
	"runtime"
	"time"
)

func main() {
	var x int
	a := []string{"a", "b", "c"}
	for idx, _ := range a {
		fmt.Println(a[idx])
	}
	threads := runtime.GOMAXPROCS(0)
	for i := 0; i < threads; i++ {
		go func() {
			for {
				x++
			}
		}()
	}
	time.Sleep(time.Second)
	fmt.Println("x =", x)
}
