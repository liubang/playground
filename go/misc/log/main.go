package main

import "log/slog"

func main() {
	s := "hello"
	slog.Info("this is test", "user", s)
}
