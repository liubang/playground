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

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/07/24

package webfetch

import (
	"net/url"
	"strconv"
	"strings"

	"golang.org/x/net/html"
)

// skippedTags never produce output. head is handled separately for <title>.
var skippedTags = map[string]struct{}{
	"script": {}, "style": {}, "noscript": {}, "template": {}, "head": {},
	"iframe": {}, "object": {}, "applet": {}, "canvas": {}, "svg": {},
	"input": {}, "button": {}, "select": {}, "textarea": {},
}

// htmlToMarkdown converts an HTML document to a compact markdown rendering.
// Relative links and image sources are resolved against base.
func htmlToMarkdown(doc string, base *url.URL) (string, error) {
	return renderHTML(doc, base, false)
}

// htmlToText extracts the plain text content of an HTML document.
func htmlToText(doc string) (string, error) {
	return renderHTML(doc, nil, true)
}

func renderHTML(doc string, base *url.URL, textOnly bool) (string, error) {
	root, err := html.Parse(strings.NewReader(doc))
	if err != nil {
		return "", err
	}
	w := &htmlWriter{base: base, textOnly: textOnly}
	if title := extractTitle(root); title != "" {
		if !textOnly {
			w.writeString("# " + title)
		} else {
			w.writeString(title)
		}
		w.ensureBlank()
	}
	w.walk(root)
	return tidyOutput(w.b.String()), nil
}

func extractTitle(n *html.Node) string {
	if n.Type == html.ElementNode && n.Data == "title" {
		return strings.TrimSpace(textContent(n))
	}
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if t := extractTitle(c); t != "" {
			return t
		}
	}
	return ""
}

func textContent(n *html.Node) string {
	var b strings.Builder
	var walk func(*html.Node)
	walk = func(node *html.Node) {
		if node.Type == html.TextNode {
			b.WriteString(node.Data)
		}
		for c := node.FirstChild; c != nil; c = c.NextSibling {
			walk(c)
		}
	}
	walk(n)
	return b.String()
}

// htmlWriter incrementally renders markdown-ish output. Trailing-newline
// state is derived by scanning the builder's tail (strings.Builder.String
// aliases its buffer, so this is O(tail), not O(document)); wholesale
// whitespace cleanup happens once in tidyOutput.
type htmlWriter struct {
	b         strings.Builder
	base      *url.URL
	textOnly  bool
	pre       bool
	listDepth int
}

// newline ensures at least n trailing newlines.
func (w *htmlWriter) newline(n int) {
	s := w.b.String()
	have := 0
	for i := len(s) - 1; i >= 0 && s[i] == '\n'; i-- {
		have++
	}
	for ; have < n; have++ {
		w.b.WriteByte('\n')
	}
}

func (w *htmlWriter) ensureBlank() { w.newline(2) }

func (w *htmlWriter) writeString(s string) { w.b.WriteString(s) }

// writeText collapses whitespace runs into single spaces while preserving
// boundary spaces so adjacent inline text does not glue together.
func (w *htmlWriter) writeText(s string) {
	if w.pre {
		w.b.WriteString(s)
		return
	}
	if s == "" {
		return
	}
	var b strings.Builder
	b.Grow(len(s))
	pendingSpace := false
	for _, r := range s {
		switch r {
		case ' ', '\t', '\n', '\r', '\f':
			pendingSpace = true
			continue
		}
		if pendingSpace {
			b.WriteByte(' ')
			pendingSpace = false
		}
		b.WriteRune(r)
	}
	out := b.String()
	cur := w.b.String()
	if strings.HasPrefix(out, " ") && (cur == "" || strings.HasSuffix(cur, " ") || strings.HasSuffix(cur, "\n")) {
		out = strings.TrimPrefix(out, " ")
	}
	w.b.WriteString(out)
	if pendingSpace {
		w.b.WriteByte(' ')
	}
}

func (w *htmlWriter) walk(n *html.Node) {
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		w.node(c)
	}
}

func (w *htmlWriter) node(n *html.Node) {
	switch n.Type {
	case html.TextNode:
		w.writeText(n.Data)
	case html.ElementNode:
		w.element(n)
	case html.DocumentNode:
		w.walk(n)
	}
}

func (w *htmlWriter) element(n *html.Node) {
	tag := n.Data
	if _, skip := skippedTags[tag]; skip {
		return
	}

	switch tag {
	case "h1", "h2", "h3", "h4", "h5", "h6":
		level := int(tag[1] - '0')
		w.ensureBlank()
		if !w.textOnly {
			w.writeString(strings.Repeat("#", level) + " ")
		}
		w.walk(n)
		w.ensureBlank()
	case "p", "div", "section", "article", "main", "header", "footer",
		"aside", "figure", "figcaption", "details", "summary", "address":
		w.ensureBlank()
		w.walk(n)
		w.ensureBlank()
	case "br":
		w.newline(1)
	case "hr":
		w.ensureBlank()
		w.writeString("---")
		w.ensureBlank()
	case "blockquote":
		w.ensureBlank()
		start := w.b.Len()
		w.walk(n)
		if !w.textOnly {
			w.prefixLines(start, "> ")
		}
		w.ensureBlank()
	case "pre":
		w.ensureBlank()
		fenced := !w.textOnly
		if fenced {
			w.writeString("```\n")
		}
		saved := w.pre
		w.pre = true
		w.walk(n)
		w.pre = saved
		if fenced {
			w.newline(1)
			w.writeString("```")
		}
		w.ensureBlank()
	case "code", "kbd", "samp":
		if w.pre || w.textOnly {
			w.walk(n)
			return
		}
		text := strings.TrimSpace(textContent(n))
		if text == "" {
			return
		}
		w.writeString("`" + text + "`")
	case "em", "i", "strong", "b":
		if w.textOnly {
			w.walk(n)
			return
		}
		marker := "*"
		if tag == "strong" || tag == "b" {
			marker = "**"
		}
		w.writeString(marker)
		w.walk(n)
		w.writeString(marker)
	case "a":
		href := attr(n, "href")
		resolved := w.resolveURL(href)
		if w.textOnly || resolved == "" || strings.HasPrefix(strings.ToLower(href), "javascript:") {
			w.walk(n)
			return
		}
		text := strings.TrimSpace(textContent(n))
		if text == "" {
			text = resolved
		}
		w.writeString("[" + text + "](" + resolved + ")")
	case "img":
		alt := attr(n, "alt")
		src := w.resolveURL(attr(n, "src"))
		if w.textOnly || src == "" {
			if alt != "" {
				w.writeText(alt)
			}
			return
		}
		w.writeString("![" + alt + "](" + src + ")")
	case "ul", "ol":
		w.ensureBlank()
		w.listDepth++
		idx := 0
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			if c.Type != html.ElementNode || c.Data != "li" {
				continue
			}
			idx++
			w.newline(1)
			w.writeString(strings.Repeat("  ", w.listDepth-1))
			if tag == "ol" {
				w.writeString(strconv.Itoa(idx) + ". ")
			} else {
				w.writeString("- ")
			}
			w.node(c)
		}
		w.listDepth--
		w.ensureBlank()
	case "li":
		// li outside a list context: render children inline.
		w.walk(n)
	case "table":
		w.renderTable(n)
	default:
		// html/body and inline or unknown elements: keep the content.
		w.walk(n)
	}
}

// prefixLines rewrites the buffer region from start with a quote prefix on
// each non-empty line.
func (w *htmlWriter) prefixLines(start int, prefix string) {
	s := w.b.String()
	region := s[start:]
	w.b.Reset()
	w.b.WriteString(s[:start])
	lines := strings.Split(region, "\n")
	for i, line := range lines {
		if strings.TrimSpace(line) != "" {
			lines[i] = prefix + line
		}
	}
	w.b.WriteString(strings.Join(lines, "\n"))
}

func (w *htmlWriter) renderTable(n *html.Node) {
	var rows [][]string
	var collect func(node *html.Node)
	collect = func(node *html.Node) {
		if node.Type == html.ElementNode && node.Data == "tr" {
			var cells []string
			for c := node.FirstChild; c != nil; c = c.NextSibling {
				if c.Type == html.ElementNode && (c.Data == "td" || c.Data == "th") {
					cells = append(cells, strings.TrimSpace(collapseWS(textContent(c))))
				}
			}
			if len(cells) > 0 {
				rows = append(rows, cells)
			}
			return
		}
		for c := node.FirstChild; c != nil; c = c.NextSibling {
			collect(c)
		}
	}
	collect(n)
	if len(rows) == 0 {
		return
	}

	w.ensureBlank()
	cols := 0
	for _, r := range rows {
		if len(r) > cols {
			cols = len(r)
		}
	}
	for i, r := range rows {
		for len(r) < cols {
			r = append(r, "")
		}
		w.writeString("| " + strings.Join(r, " | ") + " |")
		w.newline(1)
		if i == 0 && !w.textOnly {
			sep := make([]string, cols)
			for j := range sep {
				sep[j] = "---"
			}
			w.writeString("| " + strings.Join(sep, " | ") + " |")
			w.newline(1)
		}
	}
	w.ensureBlank()
}

func (w *htmlWriter) resolveURL(raw string) string {
	if raw == "" || w.base == nil {
		return raw
	}
	ref, err := url.Parse(strings.TrimSpace(raw))
	if err != nil {
		return ""
	}
	return w.base.ResolveReference(ref).String()
}

func attr(n *html.Node, key string) string {
	for _, a := range n.Attr {
		if a.Key == key {
			return a.Val
		}
	}
	return ""
}

func collapseWS(s string) string {
	return strings.Join(strings.Fields(s), " ")
}

// tidyOutput strips trailing whitespace per line and collapses blank-line
// runs to a single blank line. Fenced code blocks keep their content
// verbatim so blank lines inside code survive.
func tidyOutput(s string) string {
	lines := strings.Split(s, "\n")
	var b strings.Builder
	b.Grow(len(s))
	inFence := false
	blank := 0
	for _, line := range lines {
		if strings.HasPrefix(line, "```") {
			inFence = !inFence
			b.WriteString(line)
			b.WriteByte('\n')
			blank = 0
			continue
		}
		if inFence {
			b.WriteString(line)
			b.WriteByte('\n')
			continue
		}
		trimmed := strings.TrimRight(line, " \t")
		if trimmed == "" {
			blank++
			if blank > 1 {
				continue
			}
		} else {
			blank = 0
		}
		b.WriteString(trimmed)
		b.WriteByte('\n')
	}
	return strings.TrimSpace(b.String()) + "\n"
}
