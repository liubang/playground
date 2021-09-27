// Copyright (c) 2021 The Authors. All rights reserved.
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
// Created: 2021/09/27 19:36

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
