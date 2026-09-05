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
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

const (
	toolSearchName = "kb_search"
	toolReadName   = "kb_read"

	maxQueryBytes    = 1024
	maxTopK          = 20
	defaultTopK      = 5
	searchFieldChars = 500   // per-field cap in search results
	readFieldChars   = 16000 // per-field cap in kb_read results
	defaultTimeoutMs = 10000
	maxTimeoutMs     = 60000
)

// Collection names one searchable minisearch collection with the
// human-readable description surfaced to the model in the tool schema so
// it can route by topic without a discovery round-trip.
type Collection struct {
	Name        string
	Description string
}

// Options carries the resolved knowledge_base configuration. The
// constructor validates it once at assembly so Prepare/Execute stay fast.
type Options struct {
	BaseURL           string
	APIKey            string
	Timeout           time.Duration
	DefaultTopK       int
	DefaultCollection string
	Collections       []Collection
}

// shared holds the client plus the schema-level knobs both tools derive
// from the same configuration: the collection set, the default, and the
// "more than one collection" flag (the collection argument is generated
// only when the model has a real choice).
type shared struct {
	client            *Client
	collections       []Collection
	collectionSet     map[string]struct{}
	defaultCollection string
	defaultTopK       int
	multi             bool
	host              string // for the policy URLRequest
}

// New builds the kb_search and kb_read tools sharing one minisearch
// client. Both are registered together: search discovers, read deepens.
func New(opts Options) (*SearchTool, *ReadTool, error) {
	if err := validateOptions(opts); err != nil {
		return nil, nil, err
	}
	sh := &shared{
		client:            newClient(opts.BaseURL, opts.APIKey, opts.Timeout),
		collections:       opts.Collections,
		collectionSet:     make(map[string]struct{}, len(opts.Collections)),
		defaultCollection: opts.DefaultCollection,
		defaultTopK:       opts.DefaultTopK,
		multi:             len(opts.Collections) > 1,
	}
	for _, c := range opts.Collections {
		sh.collectionSet[c.Name] = struct{}{}
	}
	if _, ok := sh.collectionSet[sh.defaultCollection]; !ok {
		return nil, nil, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("default_collection %q is not in collections", sh.defaultCollection))
	}
	if host, ok := domain.HostFromURL(opts.BaseURL); ok {
		sh.host = host
	}

	search, err := newSearchTool(sh)
	if err != nil {
		return nil, nil, err
	}
	read, err := newReadTool(sh)
	if err != nil {
		return nil, nil, err
	}
	return search, read, nil
}

func validateOptions(opts Options) error {
	if opts.BaseURL == "" {
		return domain.NewError(domain.ErrInvalidInput, "knowledge_base.base_url is required")
	}
	if opts.Timeout <= 0 {
		return domain.NewError(domain.ErrInvalidInput, "knowledge_base.timeout_ms must be positive")
	}
	if len(opts.Collections) == 0 {
		return domain.NewError(domain.ErrInvalidInput, "knowledge_base.collections is required (at least one)")
	}
	if opts.DefaultTopK <= 0 || opts.DefaultTopK > maxTopK {
		return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("knowledge_base.default_top_k must be between 1 and %d", maxTopK))
	}
	return nil
}

func (sh *shared) resolveCollection(name string) (string, error) {
	if name == "" {
		return sh.defaultCollection, nil
	}
	if _, ok := sh.collectionSet[name]; !ok {
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("collection %q is not configured", name))
	}
	return name, nil
}

// SearchTool implements kb_search.
type SearchTool struct {
	base baseTool
	sh   *shared
}

func newSearchTool(sh *shared) (*SearchTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name:         toolSearchName,
		Description:  searchDescription(sh),
		InputSchema:  json.RawMessage(searchInputSchema(sh)),
		Capabilities: []domain.Capability{domain.CapNetworkConnect},
		Source:       domain.ToolSourceBuiltin,
	})
	if err != nil {
		return nil, err
	}
	return &SearchTool{base: base, sh: sh}, nil
}

func (t *SearchTool) Definition() domain.ToolDefinition { return t.base.Def }

// ConcurrentSafe implements domain.ConcurrentSafely: read-only HTTP
// requests against independent queries cannot interfere with each other.
func (t *SearchTool) ConcurrentSafe() bool { return true }

type searchArgs struct {
	Query      string `json:"query"`
	Collection string `json:"collection,omitempty"`
	TopK       int    `json:"top_k,omitempty"`
}

type searchResultItem struct {
	ID     string         `json:"id"`
	Score  float64        `json:"score"`
	Fields map[string]any `json:"fields"`
}

type searchOutput struct {
	Query      string             `json:"query"`
	Collection string             `json:"collection"`
	Count      int                `json:"count"`
	Results    []searchResultItem `json:"results"`
	// Note distinguishes a genuine zero-result answer from a degraded one:
	// when the knowledge base was unreachable the query was NOT answered,
	// and the model must not read the empty array as "nothing relevant".
	Note string `json:"note,omitempty"`
}

func (t *SearchTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := toolkit.DecodeStrict[searchArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if len(args.Query) == 0 || len(args.Query) > maxQueryBytes {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("query must be 1..%d bytes", maxQueryBytes))
	}
	if args.TopK == 0 {
		args.TopK = t.sh.defaultTopK
	}
	if args.TopK < 1 || args.TopK > maxTopK {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("top_k must be between 1 and %d", maxTopK))
	}
	collection, err := t.sh.resolveCollection(args.Collection)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args.Collection = collection

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Search knowledge base %q for %q", collection, args.Query)
	var urlReq *domain.URLRequest
	if t.sh.host != "" {
		urlReq = &domain.URLRequest{Host: t.sh.host}
	}
	return t.base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{ApprovalDesc: approvalDesc, URLRequest: urlReq})
}

func (t *SearchTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.VerifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := toolkit.DecodeStrict[searchArgs](prepared.Call.Arguments)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}

	hits, err := t.sh.client.Search(ctx, args.Collection, args.Query, args.TopK)
	if err != nil {
		// A minisearch outage must not block the coding agent: render an
		// empty result so the model can still answer from its own
		// knowledge — but degrade EXPLICITLY. An un-note'd empty array
		// would read as "nothing relevant exists", a silent lie when the
		// query was never answered; the note tells the model the answer
		// may be incomplete and to say so. The raw cause stays in the
		// operator log only.
		slog.Default().Warn("kb_search: minisearch unavailable, answering without knowledge base",
			"collection", args.Collection, "error", err)
		return toolkit.SuccessResult(prepared.Call.ID, startedAt, searchOutput{
			Query: args.Query, Collection: args.Collection, Count: 0, Results: []searchResultItem{},
			Note: "knowledge base unreachable (minisearch unavailable) — this query was NOT answered; " +
				"do not treat the empty result as 'no relevant content'. Answer from general knowledge " +
				"and note to the user that the knowledge base is down.",
		})
	}

	results := make([]searchResultItem, 0, len(hits))
	for _, h := range hits {
		results = append(results, searchResultItem{
			ID:     h.ID,
			Score:  h.Score,
			Fields: truncateFields(h.Fields, searchFieldChars),
		})
	}
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, searchOutput{
		Query: args.Query, Collection: args.Collection, Count: len(results), Results: results,
	})
}

// ReadTool implements kb_read.
type ReadTool struct {
	base baseTool
	sh   *shared
}

func newReadTool(sh *shared) (*ReadTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name:         toolReadName,
		Description:  readDescription(sh),
		InputSchema:  json.RawMessage(readInputSchema(sh)),
		Capabilities: []domain.Capability{domain.CapNetworkConnect},
		Source:       domain.ToolSourceBuiltin,
	})
	if err != nil {
		return nil, err
	}
	return &ReadTool{base: base, sh: sh}, nil
}

func (t *ReadTool) Definition() domain.ToolDefinition { return t.base.Def }

func (t *ReadTool) ConcurrentSafe() bool { return true }

type readArgs struct {
	ID         string `json:"id"`
	Collection string `json:"collection,omitempty"`
}

type readOutput struct {
	ID         string         `json:"id"`
	Collection string         `json:"collection"`
	Found      bool           `json:"found"`
	Fields     map[string]any `json:"fields,omitempty"`
	// Note distinguishes a genuine found=false (wrong/deleted id) from a
	// degraded one: when the knowledge base was unreachable the document's
	// existence was never verified, so the model must not conclude "does
	// not exist" from the outage path.
	Note string `json:"note,omitempty"`
}

func (t *ReadTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := toolkit.DecodeStrict[readArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if strings.TrimSpace(args.ID) == "" {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "id is required")
	}
	collection, err := t.sh.resolveCollection(args.Collection)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args.Collection = collection

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Read knowledge base document %q", args.ID)
	var urlReq *domain.URLRequest
	if t.sh.host != "" {
		urlReq = &domain.URLRequest{Host: t.sh.host}
	}
	return t.base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{ApprovalDesc: approvalDesc, URLRequest: urlReq})
}

func (t *ReadTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.VerifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := toolkit.DecodeStrict[readArgs](prepared.Call.Arguments)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}

	fields, found, err := t.sh.client.GetDocument(ctx, args.Collection, args.ID)
	if err != nil {
		// Same graceful-degradation contract as kb_search, made explicit:
		// an outage renders as found=false plus a note — never a bare
		// found=false, which would claim the document is verifiably
		// missing. The raw cause stays in the operator log only.
		slog.Default().Warn("kb_read: minisearch unavailable, document unavailable",
			"collection", args.Collection, "id", args.ID, "error", err)
		return toolkit.SuccessResult(prepared.Call.ID, startedAt, readOutput{
			ID: args.ID, Collection: args.Collection, Found: false,
			Note: "knowledge base unreachable (minisearch unavailable) — the document's existence was " +
				"not verified (not the same as found=false). Retry later or proceed without it.",
		})
	}
	if !found {
		return toolkit.SuccessResult(prepared.Call.ID, startedAt, readOutput{
			ID: args.ID, Collection: args.Collection, Found: false,
		})
	}
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, readOutput{
		ID: args.ID, Collection: args.Collection, Found: true,
		Fields: truncateFields(fields, readFieldChars),
	})
}

// truncateFields caps every string field at maxRunes (suffix-ellipsis)
// to bound the token budget the model spends on excerpts. Truncation is
// rune-aware so CJK text never splits a multi-byte character.
func truncateFields(in map[string]any, maxRunes int) map[string]any {
	if in == nil {
		return nil
	}
	out := make(map[string]any, len(in))
	for k, v := range in {
		if s, ok := v.(string); ok {
			runes := []rune(s)
			if len(runes) > maxRunes {
				out[k] = string(runes[:maxRunes]) + "…"
				continue
			}
		}
		out[k] = v
	}
	return out
}
