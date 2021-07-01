package main

import (
	"context"
	"fmt"
)

func main() {
	ctx, cancel := context.WithCancel(context.Background())
	ch := func(ctx context.Context) <-chan int {
		ch := make(chan int)
		go func() {
			for i := 0; ; i++ {
				select {
				case <-ctx.Done():
					break
				case ch <- i:
				}
			}
		}()
		return ch
	}(ctx)

	for n := range ch {
		fmt.Println(n)
		if n > 5 {
			cancel()
			break
		}
	}
}
