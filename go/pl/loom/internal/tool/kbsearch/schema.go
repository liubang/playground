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
// Created: 2026/08/17

package kbsearch

import (
	"encoding/json"
	"fmt"
	"strings"
)

// searchInputSchema builds the kb_search JSON Schema. The collection
// argument appears only when more than one collection is configured, as
// an enum of the configured names so the model routes by topic without a
// discovery round-trip. With a single collection it is omitted entirely
// (the server always targets the default), keeping the schema minimal.
func searchInputSchema(sh *shared) string {
	props := map[string]any{
		"query": map[string]any{
			"type":        "string",
			"minLength":   1,
			"maxLength":   maxQueryBytes,
			"description": "Natural-language query. The knowledge base runs hybrid BM25 + vector retrieval, so keyword-exact and semantic queries both work.",
		},
		"top_k": map[string]any{
			"type":        "integer",
			"minimum":     1,
			"maximum":     maxTopK,
			"description": fmt.Sprintf("Max results to return. Omit for the default (%d). Use more for broad exploration, fewer for a focused lookup.", sh.defaultTopK),
		},
	}
	required := []string{"query"}
	if sh.multi {
		props["collection"] = map[string]any{
			"type":        "string",
			"enum":        collectionNames(sh),
			"description": collectionEnumDescription(sh),
		}
	}
	schema := map[string]any{
		"type":                 "object",
		"additionalProperties": false,
		"properties":           props,
		"required":             required,
	}
	b, _ := json.Marshal(schema)
	return string(b)
}

func readInputSchema(sh *shared) string {
	props := map[string]any{
		"id": map[string]any{
			"type":        "string",
			"minLength":   1,
			"description": "Document id, copied verbatim from the `id` field of a kb_search result (chunk-level; may contain `/` or `#chunk_N`). Never guess or derive an id — only ids returned by kb_search exist.",
		},
	}
	required := []string{"id"}
	if sh.multi {
		props["collection"] = map[string]any{
			"type":        "string",
			"enum":        collectionNames(sh),
			"description": collectionEnumDescription(sh),
		}
	}
	schema := map[string]any{
		"type":                 "object",
		"additionalProperties": false,
		"properties":           props,
		"required":             required,
	}
	b, _ := json.Marshal(schema)
	return string(b)
}

func searchDescription(sh *shared) string {
	var b strings.Builder
	b.WriteString("Search the knowledge base (hybrid BM25 + vector retrieval with rerank) and return ranked excerpts — each as {id, score, fields}, where id identifies a chunk-level document. ")
	b.WriteString("Use it when the task may benefit from domain documentation beyond the local workspace, then call kb_read with a result's exact id to read that chunk untruncated. ")
	b.WriteString("Long field values are truncated in results; kb_read returns them in full. ")
	b.WriteString("An empty results array means the knowledge base has nothing relevant — answer from your own knowledge rather than retrying. ")
	b.WriteString("A 'note' field instead means the knowledge base was unreachable and the query was NOT answered: do not conclude 'nothing relevant' — say the knowledge base is down.")
	if sh.multi {
		b.WriteString(" Collections: " + collectionEnumDescription(sh))
	} else {
		b.WriteString(" The collection is fixed by configuration.")
	}
	return b.String()
}

func readDescription(sh *shared) string {
	var b strings.Builder
	b.WriteString("Read one knowledge base document in full by its id, copied verbatim from the `id` field of a kb_search result. ")
	b.WriteString("Ids are chunk-level: a markdown document is stored as chunks named `<doc>#chunk_N`, and there is no document-level entry — reading `<doc>` without the `#chunk_N` suffix returns found=false. ")
	b.WriteString("Use it after kb_search when an excerpt looks relevant but was truncated. ")
	b.WriteString("found=false means the document does not exist (wrong id or deleted) — pick another result instead of retrying the same id. ")
	b.WriteString("A 'note' field instead means the knowledge base was unreachable and the document's existence was not verified — not the same as found=false.")
	if sh.multi {
		b.WriteString(" Collections: " + collectionEnumDescription(sh))
	} else {
		b.WriteString(" The collection is fixed by configuration.")
	}
	return b.String()
}

func collectionNames(sh *shared) []string {
	names := make([]string, len(sh.collections))
	for i, c := range sh.collections {
		names[i] = c.Name
	}
	return names
}

func collectionEnumDescription(sh *shared) string {
	parts := make([]string, 0, len(sh.collections))
	for _, c := range sh.collections {
		if c.Description != "" {
			parts = append(parts, fmt.Sprintf("%s: %s", c.Name, c.Description))
		} else {
			parts = append(parts, c.Name)
		}
	}
	return fmt.Sprintf("Knowledge base collection to search. %s", strings.Join(parts, "; "))
}
