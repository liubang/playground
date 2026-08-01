// Copyright (c) 2026 The Authors. All rights reserved.
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

package websearch

import (
	"net/url"
	"strings"

	"golang.org/x/net/html"
)

// parseDuckDuckGoHTML extracts search results from DuckDuckGo's HTML
// response. The layout uses div.result with a.result__a for the title
// and link, and a.result__snippet for the abstract.
func parseDuckDuckGoHTML(data []byte, maxResults int) []searchResult {
	doc, err := html.Parse(strings.NewReader(string(data)))
	if err != nil {
		return nil
	}

	var results []searchResult
	var crawl func(*html.Node)
	crawl = func(n *html.Node) {
		if n.Type == html.ElementNode && n.Data == "div" && hasClass(n, "result") {
			r := extractResult(n)
			if r.URL != "" {
				results = append(results, r)
				if len(results) >= maxResults {
					return
				}
			}
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			if len(results) >= maxResults {
				return
			}
			crawl(c)
		}
	}
	crawl(doc)
	return results
}

func extractResult(n *html.Node) searchResult {
	var r searchResult
	var find func(*html.Node)
	find = func(node *html.Node) {
		if node.Type == html.ElementNode {
			switch {
			case node.Data == "a" && hasClass(node, "result__a"):
				r.Title = textContent(node)
				for _, attr := range node.Attr {
					if attr.Key == "href" {
						r.URL = cleanURL(attr.Val)
						break
					}
				}
			case node.Data == "a" && hasClass(node, "result__snippet"):
				r.Snippet = textContent(node)
			case node.Data == "td" && hasClass(node, "result__snippet"):
				r.Snippet = textContent(node)
			}
		}
		for c := node.FirstChild; c != nil; c = c.NextSibling {
			find(c)
		}
	}
	find(n)
	return r
}

func hasClass(n *html.Node, class string) bool {
	for _, attr := range n.Attr {
		if attr.Key == "class" {
			for _, c := range strings.Fields(attr.Val) {
				if c == class {
					return true
				}
			}
		}
	}
	return false
}

func textContent(n *html.Node) string {
	var sb strings.Builder
	var walk func(*html.Node)
	walk = func(node *html.Node) {
		if node.Type == html.TextNode {
			sb.WriteString(node.Data)
		}
		for c := node.FirstChild; c != nil; c = c.NextSibling {
			walk(c)
		}
	}
	walk(n)
	return strings.TrimSpace(sb.String())
}

// cleanURL strips DuckDuckGo redirect wrappers (uddg= parameter).
func cleanURL(raw string) string {
	if !strings.HasPrefix(raw, "//duckduckgo.com/l/") && !strings.HasPrefix(raw, "https://duckduckgo.com/l/") {
		return raw
	}
	parsed := raw
	if strings.HasPrefix(raw, "//") {
		parsed = "https:" + raw
	}
	u, err := url.Parse(parsed)
	if err != nil {
		return raw
	}
	if uddg := u.Query().Get("uddg"); uddg != "" {
		return uddg
	}
	return raw
}
