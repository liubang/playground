// Copyright (c) 2023 The Authors. All rights reserved.
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
// Created: 2023/04/01 20:48

package main

import (
	"fmt"
	"log"
	"os"
	"time"

	"github.com/gocolly/colly"
	"github.com/gocolly/colly/debug"
)

type item struct {
	StoryURL  string
	Source    string
	comments  string
	CrawledAt time.Time
	Comments  string
	Title     string
}

func main() {
	c := colly.NewCollector(
		// colly.AllowedDomains("baike.baidu.com"),
		colly.Debugger(&debug.LogDebugger{
			Output: os.Stdout,
		}),
		colly.MaxDepth(1),
		colly.UserAgent("Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/67.0.3396.99 Safari/537.36"),
	)

	c.DisableCookies()
	// extensions.RandomUserAgent(c)
	// extensions.Referer(c)

	c.OnHTML("dd.lemmaWgt-lemmaTitle-title", func(h *colly.HTMLElement) {
		log.Println("OK")
		log.Println(h.Text)
	})

	c.OnRequest(func(r *colly.Request) {
		r.Headers.Add("Accept-Encoding", "gzip, deflate")
		r.Headers.Add("Accept", "*/*")
		r.Headers.Add("Connection", "keep-alive")
		fmt.Println("Visiting", r.URL.String())
	})

	c.OnResponse(func(r *colly.Response) {
		fmt.Println(*r.Request.Headers)
		// log.Println(string(r.Body))
	})

	c.Visit("https://baike.baidu.com/item/ChatGPT")
}
