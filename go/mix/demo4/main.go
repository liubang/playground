package main

import (
	"fmt"
	"net/http"
	"time"

	tally "github.com/uber-go/tally/v4"
	promreporter "github.com/uber-go/tally/v4/prometheus"
)

func main() {
	r := promreporter.NewReporter(promreporter.Options{})

	scope, closer := tally.NewRootScope(tally.ScopeOptions{
		Prefix:         "my_service",
		Tags:           map[string]string{},
		CachedReporter: r,
		Separator:      promreporter.DefaultSeparator,
	}, 1*time.Second)
	defer closer.Close()

	http.Handle("/metrics", r.HTTPHandler())
	fmt.Printf("Serving :8080/metrics\n")
	fmt.Printf("%v\n", http.ListenAndServe(":8080", nil))
	select {}
}
