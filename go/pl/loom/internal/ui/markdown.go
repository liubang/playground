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

package ui

import (
	"crypto/sha256"
	"encoding/hex"
	"strings"
	"sync"

	"github.com/charmbracelet/glamour"
	"github.com/charmbracelet/glamour/styles"
)

// markdownCacheCap bounds the render cache. When exceeded the cache resets
// wholesale: chat transcripts are append-mostly, so a cold rebuild stays
// cheap while the map never grows without limit.
const markdownCacheCap = 512

// markdownRenderer wraps glamour with loom's theme awareness and a
// per-(content, width, style) render cache. Finalized assistant blocks are
// immutable, so caching is safe and avoids re-rendering the full transcript
// on every frame.
type markdownRenderer struct {
	mu        sync.Mutex
	renderers map[markdownRendererKey]*glamour.TermRenderer
	cache     map[markdownCacheKey]string
}

type markdownRendererKey struct {
	wordWrap int
	profile  string // "dark" | "light" | "notty"
}

type markdownCacheKey struct {
	digest   string
	wordWrap int
	profile  string
}

func newMarkdownRenderer() *markdownRenderer {
	return &markdownRenderer{
		renderers: make(map[markdownRendererKey]*glamour.TermRenderer),
		cache:     make(map[markdownCacheKey]string),
	}
}

// render converts markdown into a styled terminal string. wordWrap <= 0
// falls back to glamour's default width (used when the terminal size is not
// known yet); an empty profile selects the Everforest dark style.
func (r *markdownRenderer) render(content string, wordWrap int, profile string) string {
	if wordWrap < 0 {
		wordWrap = 0
	}
	if profile == "" {
		profile = "dark"
	}
	content = escapeLoneTildes(content)
	sum := sha256.Sum256([]byte(content))
	key := markdownCacheKey{digest: hex.EncodeToString(sum[:]), wordWrap: wordWrap, profile: profile}

	r.mu.Lock()
	defer r.mu.Unlock()
	if rendered, ok := r.cache[key]; ok {
		return rendered
	}
	renderer, err := r.termRenderer(markdownRendererKey{wordWrap: wordWrap, profile: profile})
	if err != nil {
		return content
	}
	rendered, err := renderer.Render(content)
	if err != nil {
		return content
	}
	// Strip the document-level blank lines without eating meaningful leading
	// indentation of the first content line (indented code, nested lists).
	rendered = strings.TrimLeft(rendered, "\n")
	rendered = strings.TrimRight(rendered, " \n\t")
	if len(r.cache) >= markdownCacheCap {
		r.cache = make(map[markdownCacheKey]string)
	}
	r.cache[key] = rendered
	return rendered
}

func (r *markdownRenderer) termRenderer(key markdownRendererKey) (*glamour.TermRenderer, error) {
	if renderer, ok := r.renderers[key]; ok {
		return renderer, nil
	}
	var options []glamour.TermRendererOption
	if key.wordWrap > 0 {
		options = append(options, glamour.WithWordWrap(key.wordWrap))
	}
	switch key.profile {
	case "notty":
		// notty is glamour's ASCII style: no ANSI sequences at all, which is
		// the only safe choice for NO_COLOR and dumb terminals.
		options = append(options, glamour.WithStandardStyle(styles.NoTTYStyle))
	case "light":
		options = append(options, glamour.WithStyles(styles.LightStyleConfig))
	default:
		options = append(options, glamour.WithStylesFromJSONBytes([]byte(everforestStyleJSON)))
	}
	renderer, err := glamour.NewTermRenderer(options...)
	if err != nil {
		return nil, err
	}
	r.renderers[key] = renderer
	return renderer, nil
}

// escapeLoneTildes escapes every single "~" as "\~" so goldmark's GFM
// strikethrough (which accepts both ~ and ~~) cannot strike through
// temperature ranges and similar everyday text ("31~33°C，体感 38~41°C"
// would otherwise render crossed out). "~~" pairs pass through
// untouched, keeping deliberate strikethrough available. Code spans and
// fenced code blocks are exempt: an escape backslash would render
// literally there. Indented code blocks are not detected (rare in
// model output; fences are the norm).
func escapeLoneTildes(s string) string {
	if !strings.Contains(s, "~") {
		return s
	}
	var b strings.Builder
	b.Grow(len(s) + 8)
	inFence := false
	for line := range strings.Lines(s) {
		trimmed := strings.TrimLeft(line, " \t")
		if strings.HasPrefix(trimmed, "```") || strings.HasPrefix(trimmed, "~~~") {
			inFence = !inFence
			b.WriteString(line)
			continue
		}
		if inFence {
			b.WriteString(line)
			continue
		}
		escapeLoneTildesInLine(&b, line)
	}
	return b.String()
}

// escapeLoneTildesInLine escapes lone tildes in one line, leaving inline
// code spans (backtick runs) untouched.
func escapeLoneTildesInLine(b *strings.Builder, line string) {
	i := 0
	for i < len(line) {
		if line[i] == '`' {
			j := i
			for j < len(line) && line[j] == '`' {
				j++
			}
			end := findClosingTicks(line, j, j-i)
			if end < 0 {
				// No closing run: the rest of the line is literal.
				b.WriteString(line[i:])
				return
			}
			b.WriteString(line[i:end])
			i = end
			continue
		}
		if line[i] == '~' {
			if i+1 < len(line) && line[i+1] == '~' {
				b.WriteString("~~")
				i += 2
				continue
			}
			b.WriteString("&#126;")
			i++
			continue
		}
		b.WriteByte(line[i])
		i++
	}
}

// findClosingTicks locates the closing run of n backticks starting the
// search at from, returning the index just past it (-1 when absent).
func findClosingTicks(s string, from, n int) int {
	idx := strings.Index(s[from:], strings.Repeat("`", n))
	if idx < 0 {
		return -1
	}
	return from + idx + n
}

// everforestStyleJSON is the loom dark theme as a glamour style sheet, using
// the same Everforest Dark Hard palette as the rest of the TUI.
const everforestStyleJSON = `{
  "document": {
    "block_prefix": "\n",
    "block_suffix": "\n",
    "color": "#d3c6aa",
    "margin": 0
  },
  "block_quote": {
    "indent": 1,
    "indent_token": "│ ",
    "color": "#859289",
    "italic": true
  },
  "paragraph": {},
  "list": {
    "level_indent": 2
  },
  "heading": {
    "block_suffix": "\n",
    "color": "#7fbbb3",
    "bold": true
  },
  "h1": {
    "prefix": "# ",
    "color": "#7fbbb3",
    "bold": true
  },
  "h2": {
    "prefix": "## "
  },
  "h3": {
    "prefix": "### "
  },
  "h4": {
    "prefix": "#### "
  },
  "h5": {
    "prefix": "##### "
  },
  "h6": {
    "prefix": "###### ",
    "color": "#859289",
    "bold": false
  },
  "text": {},
  "strikethrough": {
    "crossed_out": true
  },
  "emph": {
    "italic": true
  },
  "strong": {
    "bold": true
  },
  "hr": {
    "color": "#859289",
    "format": "\n--------\n"
  },
  "item": {
    "block_prefix": "• "
  },
  "enumeration": {
    "block_prefix": ". "
  },
  "task": {
    "ticked": "[✓] ",
    "unticked": "[ ] "
  },
  "link": {
    "color": "#7fbbb3",
    "underline": true
  },
  "link_text": {
    "color": "#83c092",
    "bold": true
  },
  "image": {
    "color": "#7fbbb3",
    "underline": true
  },
  "image_text": {
    "color": "#859289",
    "format": "Image: {{.text}} →"
  },
  "code": {
    "prefix": " ",
    "suffix": " ",
    "color": "#e69875",
    "background_color": "#2e383c"
  },
  "code_block": {
    "color": "#a7c080",
    "margin": 0,
    "chroma": {
      "text": {
        "color": "#d3c6aa"
      },
      "error": {
        "color": "#e67e80"
      },
      "comment": {
        "color": "#859289",
        "italic": true
      },
      "comment_preproc": {
        "color": "#e69875"
      },
      "keyword": {
        "color": "#e67e80"
      },
      "keyword_reserved": {
        "color": "#d699b6"
      },
      "keyword_namespace": {
        "color": "#d699b6"
      },
      "keyword_type": {
        "color": "#dbbc7f"
      },
      "operator": {
        "color": "#7fbbb3"
      },
      "punctuation": {
        "color": "#d3c6aa"
      },
      "name": {
        "color": "#d3c6aa"
      },
      "name_builtin": {
        "color": "#d699b6"
      },
      "name_tag": {
        "color": "#e67e80"
      },
      "name_attribute": {
        "color": "#83c092"
      },
      "name_class": {
        "color": "#dbbc7f",
        "bold": true
      },
      "name_function": {
        "color": "#a7c080"
      },
      "literal_number": {
        "color": "#d699b6"
      },
      "literal_string": {
        "color": "#83c092"
      },
      "literal_string_escape": {
        "color": "#7fbbb3"
      },
      "generic_deleted": {
        "color": "#e67e80"
      },
      "generic_inserted": {
        "color": "#a7c080"
      },
      "generic_emph": {
        "italic": true
      },
      "generic_strong": {
        "bold": true
      },
      "generic_subheading": {
        "color": "#859289"
      },
      "background": {
        "background_color": "#2e383c"
      }
    }
  },
  "table": {},
  "definition_list": {},
  "definition_term": {},
  "definition_description": {},
  "html_block": {},
  "html_span": {}
}`
