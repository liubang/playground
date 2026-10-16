// Copyright (c) 2026 The Authors. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

package memory

import (
	"context"
	"errors"
	"fmt"
	"io"
	"slices"
	"strings"
	"sync"
	"testing"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// consolidationRepo models publication fences without importing session (which
// imports memory), and always returns an independent, consistent snapshot.
type consolidationRepo struct {
	mu             sync.Mutex
	snapshots      map[string]Snapshot
	publishes      int
	snapshotErrors map[string]error
	publishErrors  map[string]error
	publishHook    func(string)
}

func (r *consolidationRepo) MutateMemory(context.Context, Mutation) (Record, error) {
	return Record{}, errors.New("not used")
}

func (r *consolidationRepo) MemorySnapshot(ctx context.Context, scope string) (Snapshot, error) {
	if err := ctx.Err(); err != nil {
		return Snapshot{}, err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if err := r.snapshotErrors[scope]; err != nil {
		return Snapshot{}, err
	}
	s := r.snapshots[scope]
	s.Records = slices.Clone(s.Records)
	return s, nil
}

func (r *consolidationRepo) MemoryContext(ctx context.Context, workspace string) ([]Snapshot, error) {
	return fakeMemoryContext(ctx, workspace, r.MemorySnapshot)
}

func (r *consolidationRepo) ReadMemory(ctx context.Context, workspace, id string) (Record, error) {
	return fakeReadMemory(ctx, workspace, id, r.MemorySnapshot)
}

func (r *consolidationRepo) QueryMemory(ctx context.Context, workspace, query string, limit int) (SearchResult, error) {
	return fakeQueryMemory(ctx, workspace, query, limit, r.MemorySnapshot)
}

func (r *consolidationRepo) MemoryScopes(ctx context.Context) ([]string, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	var scopes []string
	for scope, s := range r.snapshots {
		if s.Version > 0 && (!s.ValidView() || s.View.Version < s.Version) {
			scopes = append(scopes, scope)
		}
	}
	slices.Sort(scopes)
	return scopes, nil
}

func (r *consolidationRepo) PublishMemoryView(ctx context.Context, s Snapshot, main, summary string) (bool, error) {
	if err := ctx.Err(); err != nil {
		return false, err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.publishHook != nil {
		r.publishHook(s.Scope)
	}
	if err := ctx.Err(); err != nil {
		return false, err
	}
	if err := r.publishErrors[s.Scope]; err != nil {
		return false, err
	}
	current := r.snapshots[s.Scope]
	if current.Epoch != s.Epoch || current.Version < s.Version || s.Version <= 0 || (current.ValidView() && current.View.Version >= s.Version) {
		return false, nil
	}
	current.View = View{Version: s.Version, Epoch: s.Epoch, Main: main, Summary: summary}
	r.snapshots[s.Scope] = current
	r.publishes++
	return true, nil
}

func (r *consolidationRepo) change(scope string, fn func(*Snapshot)) {
	r.mu.Lock()
	defer r.mu.Unlock()
	s := r.snapshots[scope]
	fn(&s)
	r.snapshots[scope] = s
}

func consolidationFixture() *consolidationRepo {
	return &consolidationRepo{snapshots: map[string]Snapshot{"": {Version: 1, Epoch: 1, Records: []Record{{ID: "auto", Kind: Automatic, Sequence: 1, Content: "use Go"}}}}}
}

type consolidationModel struct {
	requests []domain.ModelRequest
	streams  []*consolidationStream
	hook     func(int)
}

func (m *consolidationModel) Stream(_ context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	index := len(m.requests)
	m.requests = append(m.requests, req)
	if m.hook != nil {
		m.hook(index)
	}
	if index < len(m.streams) {
		return m.streams[index], nil
	}
	return normalConsolidationStream("view"), nil
}

type consolidationStream struct {
	events   []domain.ModelEvent
	err      error
	closeErr error
	closed   bool
	onEOF    func()
}

func (s *consolidationStream) Recv() (domain.ModelEvent, error) {
	if len(s.events) > 0 {
		event := s.events[0]
		s.events = s.events[1:]
		return event, nil
	}
	if s.onEOF != nil {
		s.onEOF()
	}
	if s.err != nil {
		return domain.ModelEvent{}, s.err
	}
	return domain.ModelEvent{}, io.EOF
}

func (s *consolidationStream) Close() error { s.closed = true; return s.closeErr }

func normalConsolidationStream(text string) *consolidationStream {
	return &consolidationStream{events: []domain.ModelEvent{
		{Kind: domain.ModelEventTextDelta, TextDelta: text},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
	}}
}

func TestCompleteTextStrictStream(t *testing.T) {
	failure := errors.New("transport failure")
	cases := []struct {
		name    string
		stream  *consolidationStream
		wantErr string
	}{
		{"normal", normalConsolidationStream("hello"), ""},
		{"missing terminal", &consolidationStream{events: []domain.ModelEvent{{Kind: domain.ModelEventTextDelta, TextDelta: "partial"}}}, "without response_end"},
		{"event error", &consolidationStream{events: []domain.ModelEvent{{Kind: domain.ModelEventStreamError, Error: "provider failure"}}}, "provider failure"},
		{"unknown event error", &consolidationStream{events: []domain.ModelEvent{{Kind: domain.ModelEventStreamError}}}, "unknown"},
		{"recv error", &consolidationStream{err: failure}, "transport failure"},
		{"oversized", normalConsolidationStream(strings.Repeat("x", maxModelTextBytes+1)), "text limit"},
		{"invalid UTF8", normalConsolidationStream("\xff"), "UTF-8"},
		{"close error", &consolidationStream{events: normalConsolidationStream("ok").events, closeErr: failure}, "transport failure"},
		{"error after terminal", &consolidationStream{events: normalConsolidationStream("ok").events, err: failure}, "transport failure"},
		{"text after terminal", &consolidationStream{events: append(normalConsolidationStream("ok").events, domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: "late"})}, "after response_end"},
		{"duplicate terminal", &consolidationStream{events: []domain.ModelEvent{{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn}, {Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn}}}, "duplicate"},
		{"tool event", &consolidationStream{events: []domain.ModelEvent{{Kind: domain.ModelEventToolCallStart}}}, "tool call"},
	}
	for _, reason := range []domain.StopReason{domain.StopMaxOutput, domain.StopToolUse, domain.StopContentFilter, domain.StopCancelled, domain.StopProviderError, domain.StopUnknown, ""} {
		cases = append(cases, struct {
			name    string
			stream  *consolidationStream
			wantErr string
		}{string(reason), &consolidationStream{events: []domain.ModelEvent{{Kind: domain.ModelEventResponseEnd, StopReason: reason}}}, "abnormally"})
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			model := &consolidationModel{streams: []*consolidationStream{tc.stream}}
			text, err := completeText(t.Context(), model, domain.ModelRequest{})
			if tc.wantErr == "" {
				if err != nil || text != "hello" {
					t.Fatalf("text=%q err=%v", text, err)
				}
			} else if err == nil || !strings.Contains(err.Error(), tc.wantErr) || text != "" {
				t.Fatalf("text=%q err=%v; want %q", text, err, tc.wantErr)
			}
			if !tc.stream.closed {
				t.Fatal("stream not closed")
			}
		})
	}
}

func TestCompleteTextCancellationAtEOF(t *testing.T) {
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	stream := normalConsolidationStream("ok")
	stream.onEOF = cancel
	text, err := completeText(ctx, &consolidationModel{streams: []*consolidationStream{stream}}, domain.ModelRequest{})
	if !errors.Is(err, context.Canceled) || text != "" || !stream.closed {
		t.Fatalf("text=%q err=%v closed=%v", text, err, stream.closed)
	}
}

func TestConsolidationIncrementalAutomaticWithExplicitConstraints(t *testing.T) {
	repo := consolidationFixture()
	repo.change("", func(s *Snapshot) {
		s.Version = 4
		s.View = View{Version: 1, Epoch: 1, Main: "previous view", Summary: "previous summary"}
		// This explicit constraint was already present when version 1 was built;
		// only automatic additions may retain the same epoch.
		s.Records = append(s.Records, Record{Kind: Automatic, Sequence: 4, Content: "four"}, Record{Kind: Explicit, Sequence: 1, Content: "explicit original"}, Record{Kind: Automatic, Sequence: 3, Content: "three"})
	})
	model := &consolidationModel{streams: []*consolidationStream{normalConsolidationStream("```markdown\nmerged\n```"), normalConsolidationStream("summary")}}
	c := NewConsolidator(NewStore(repo), model, "real-model")
	changed, err := c.Consolidate(t.Context())
	if err != nil || !changed || len(model.requests) != 2 {
		t.Fatalf("changed=%v err=%v calls=%d", changed, err, len(model.requests))
	}
	var prompt string
	for _, msg := range model.requests[0].Messages {
		prompt += strings.Join(msg.TextParts(), "\n")
	}
	if !strings.Contains(prompt, "previous view") || strings.Contains(prompt, "use Go") || !strings.Contains(prompt, "explicit original") || !strings.Contains(prompt, "Do not copy or summarize explicit constraints") || strings.Index(prompt, "three") > strings.Index(prompt, "four") {
		t.Fatalf("bad incremental prompt: %s", prompt)
	}
	if model.requests[0].ModelName != "real-model" {
		t.Fatal("model name lost")
	}
	s, _ := repo.MemorySnapshot(t.Context(), "")
	if s.View.Version != 4 || s.View.Main != "merged" || s.View.Summary != "summary" {
		t.Fatalf("view=%+v", s.View)
	}
	changed, err = c.Consolidate(t.Context())
	if changed || err != nil || len(model.requests) != 2 {
		t.Fatalf("repeated consolidation called model: %v %v", changed, err)
	}
}

func TestConsolidationNoAutomaticInput(t *testing.T) {
	for _, valid := range []bool{false, true} {
		t.Run(fmt.Sprint(valid), func(t *testing.T) {
			repo := consolidationFixture()
			repo.change("", func(s *Snapshot) {
				s.Version = 2
				if valid {
					s.View = View{Version: 1, Epoch: 1, Main: "preserved", Summary: "summary"}
				} else {
					s.Epoch = 2
					s.View = View{Version: 1, Epoch: 1, Main: "deleted"}
				}
				s.Records = []Record{{Kind: Explicit, Sequence: 2, Content: "verbatim"}}
			})
			model := &consolidationModel{}
			changed, err := NewConsolidator(NewStore(repo), model, "").Consolidate(t.Context())
			if err != nil || !changed || len(model.requests) != 0 {
				t.Fatalf("%v %v calls=%d", changed, err, len(model.requests))
			}
			s, _ := repo.MemorySnapshot(t.Context(), "")
			if s.View.Version != 2 || s.View.Main != "" || s.View.Summary != "" {
				t.Fatalf("view=%+v", s.View)
			}
		})
	}
}

func TestConsolidationModelFailuresNeverPublish(t *testing.T) {
	for _, phase := range []int{0, 1} {
		for _, mode := range []string{"EOF", "stream error", "max output", "empty", "oversized", "refusal"} {
			t.Run(fmt.Sprintf("%d/%s", phase, mode), func(t *testing.T) {
				stream := normalConsolidationStream("ok")
				switch mode {
				case "EOF":
					stream.events = stream.events[:1]
				case "stream error":
					stream.events = append(stream.events, domain.ModelEvent{Kind: domain.ModelEventStreamError, Error: "failure"})
				case "max output":
					stream.events[1].StopReason = domain.StopMaxOutput
				case "refusal":
					stream.events[1].StopReason = domain.StopContentFilter
				case "empty":
					stream.events[0].TextDelta = "```md\n\n```"
				case "oversized":
					limit := maxMainBytes
					if phase == 1 {
						limit = maxSummaryBytes
					}
					stream.events[0].TextDelta = strings.Repeat("x", limit+1)
				}
				streams := []*consolidationStream{normalConsolidationStream("main"), normalConsolidationStream("summary")}
				streams[phase] = stream
				repo := consolidationFixture()
				changed, err := NewConsolidator(NewStore(repo), &consolidationModel{streams: streams}, "").Consolidate(t.Context())
				pending, _ := repo.MemoryScopes(t.Context())
				if changed || err == nil || repo.publishes != 0 || len(pending) != 1 || !stream.closed {
					t.Fatalf("changed=%v err=%v publishes=%d pending=%v", changed, err, repo.publishes, pending)
				}
			})
		}
	}
}

func TestConsolidationBoundedBatch(t *testing.T) {
	repo := consolidationFixture()
	repo.change("", func(s *Snapshot) {
		s.Version = 107
		s.Records = nil
		for i := 100; i > 0; i-- {
			s.Records = append(s.Records, Record{ID: fmt.Sprint(i), Kind: Automatic, Sequence: int64(i), Content: strings.Repeat("x", 4000)})
		}
		for i := range 7 {
			s.Records = append(s.Records, Record{ID: fmt.Sprintf("constraint-%d", i), Kind: Explicit, Sequence: int64(101 + i), Content: "explicit constraint " + strings.Repeat("界", 1300)})
		}
	})
	model := &consolidationModel{}
	c := NewConsolidator(NewStore(repo), model, "")
	if changed, err := c.Consolidate(t.Context()); err != nil || !changed {
		t.Fatalf("%v %v", changed, err)
	}
	s, _ := repo.MemorySnapshot(t.Context(), "")
	if s.View.Version <= 1 || s.View.Version >= 100 {
		t.Fatalf("batch marked unprocessed records covered: %d", s.View.Version)
	}
	total := 0
	for _, msg := range model.requests[0].Messages {
		for _, text := range msg.TextParts() {
			total += len(text)
		}
	}
	if total > maxConsolidationInputBytes {
		t.Fatalf("input exceeds budget: %d", total)
	}
	var prompt string
	for _, msg := range model.requests[0].Messages {
		prompt += strings.Join(msg.TextParts(), "\n")
	}
	if strings.Count(prompt, "explicit constraint 界") != 7 {
		t.Fatal("batch omitted explicit constraints")
	}
	first := s.View.Version
	if changed, err := c.Consolidate(t.Context()); err != nil || !changed {
		t.Fatalf("second batch: %v %v", changed, err)
	}
	s, _ = repo.MemorySnapshot(t.Context(), "")
	if s.View.Version <= first {
		t.Fatal("batch did not advance")
	}
}

func TestConsolidationOversizedRecordNotCovered(t *testing.T) {
	repo := consolidationFixture()
	repo.change("", func(s *Snapshot) { s.Records[0].Rollout = strings.Repeat("x", maxConsolidationInputBytes) })
	model := &consolidationModel{}
	changed, err := NewConsolidator(NewStore(repo), model, "").Consolidate(t.Context())
	if changed || err == nil || repo.publishes != 0 || len(model.requests) != 0 {
		t.Fatalf("changed=%v err=%v", changed, err)
	}
}

func TestConsolidationRedactionAndNilModel(t *testing.T) {
	secret := "sk-abcdefghijklmnopqrstuvwx"
	for _, useModel := range []bool{false, true} {
		t.Run(fmt.Sprint(useModel), func(t *testing.T) {
			repo := consolidationFixture()
			repo.change("", func(s *Snapshot) { s.Records[0].Content = "use " + secret + strings.Repeat("界", 2000) })
			var model domain.Model
			m := &consolidationModel{streams: []*consolidationStream{normalConsolidationStream("main " + secret), normalConsolidationStream("summary " + secret)}}
			if useModel {
				model = m
			}
			changed, err := NewConsolidator(NewStore(repo), model, "").Consolidate(t.Context())
			if err != nil || !changed {
				t.Fatalf("%v %v", changed, err)
			}
			s, _ := repo.MemorySnapshot(t.Context(), "")
			if strings.Contains(s.View.Main+s.View.Summary, secret) || !utf8.ValidString(s.View.Summary) || len(s.View.Summary) > maxSummaryBytes {
				t.Fatalf("unsafe view: %+v", s.View)
			}
			for _, req := range m.requests {
				for _, msg := range req.Messages {
					if strings.Contains(strings.Join(msg.TextParts(), ""), secret) {
						t.Fatal("secret leaked to model")
					}
				}
			}
		})
	}
}

func TestConsolidationConcurrentChanges(t *testing.T) {
	for _, mode := range []string{"addition", "explicit addition", "forget", "newer view"} {
		t.Run(mode, func(t *testing.T) {
			repo := consolidationFixture()
			started, resume := make(chan struct{}), make(chan struct{})
			model := &consolidationModel{hook: func(i int) {
				if i == 0 {
					close(started)
					<-resume
				}
			}}
			var changed bool
			var err error
			var wg sync.WaitGroup
			wg.Go(func() { changed, err = NewConsolidator(NewStore(repo), model, "").Consolidate(t.Context()) })
			<-started
			repo.change("", func(s *Snapshot) {
				s.Version++
				switch mode {
				case "addition":
					s.Records = append(s.Records, Record{Kind: Automatic, Sequence: 2, Content: "new"})
				case "explicit addition":
					s.Epoch++
					s.Records = append(s.Records, Record{Kind: Explicit, Sequence: 2, Content: "prefer Rust instead"})
				case "forget":
					s.Epoch++
					s.Records = nil
				case "newer view":
					s.View = View{Version: 2, Epoch: s.Epoch, Main: "newer", Summary: "newer"}
				}
			})
			close(resume)
			wg.Wait()
			if err != nil || changed != (mode == "addition") {
				t.Fatalf("changed=%v err=%v", changed, err)
			}
			s, _ := repo.MemorySnapshot(t.Context(), "")
			if mode == "addition" && s.View.Version != 1 {
				t.Fatalf("new addition was marked covered: %+v", s.View)
			}
			if mode == "explicit addition" {
				if s.ValidView() || repo.publishes != 0 {
					t.Fatal("inflight result ignoring explicit addition was published")
				}
				pending, _ := repo.MemoryScopes(t.Context())
				if len(pending) != 1 {
					t.Fatal("explicit addition did not remain pending")
				}
			}
			if mode == "newer view" && s.View.Main != "newer" {
				t.Fatal("older result overwrote newer view")
			}
			if mode == "forget" {
				if s.ValidView() {
					t.Fatal("revoked evidence published")
				}
				if ok, err := NewConsolidator(NewStore(repo), nil, "").Consolidate(t.Context()); !ok || err != nil {
					t.Fatalf("rebuild: %v %v", ok, err)
				}
				s, _ = repo.MemorySnapshot(t.Context(), "")
				if !s.ValidView() || s.View.Main != "" {
					t.Fatalf("forgotten evidence survives: %+v", s.View)
				}
			}
		})
	}
}

func TestCompleteTextBoundaryAndSplitUTF8(t *testing.T) {
	for _, text := range []string{strings.Repeat("x", maxModelTextBytes), "界"} {
		stream := &consolidationStream{events: []domain.ModelEvent{
			{Kind: domain.ModelEventTextDelta, TextDelta: text[:1]},
			{Kind: domain.ModelEventTextDelta, TextDelta: text[1:]},
			{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
		}}
		got, err := completeText(t.Context(), &consolidationModel{streams: []*consolidationStream{stream}}, domain.ModelRequest{})
		if err != nil || got != text {
			t.Fatalf("boundary/split output: length=%d err=%v", len(got), err)
		}
	}
}

func TestConsolidationCancellationDoesNotPublish(t *testing.T) {
	repo := consolidationFixture()
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	stream := normalConsolidationStream("summary")
	stream.onEOF = cancel
	model := &consolidationModel{streams: []*consolidationStream{normalConsolidationStream("main"), stream}}
	changed, err := NewConsolidator(NewStore(repo), model, "").Consolidate(ctx)
	if changed || !errors.Is(err, context.Canceled) || repo.publishes != 0 {
		t.Fatalf("changed=%v err=%v", changed, err)
	}
}

func TestConsolidationAllPendingScopes(t *testing.T) {
	repo := consolidationFixture()
	repo.snapshots["/other-workspace"] = Snapshot{Scope: "/other-workspace", Version: 1, Epoch: 1, Records: []Record{{Kind: Automatic, Sequence: 1, Content: "other evidence"}}}
	repo.snapshots["/settled"] = Snapshot{Scope: "/settled", Version: 1, Epoch: 1, View: View{Version: 1, Epoch: 1, Main: "settled"}}
	model := &consolidationModel{}
	changed, err := NewConsolidator(NewStore(repo).ForWorkspace("/current"), model, "").Consolidate(t.Context())
	if !changed || err != nil || repo.publishes != 2 || len(model.requests) != 4 {
		t.Fatalf("changed=%v err=%v publishes=%d calls=%d", changed, err, repo.publishes, len(model.requests))
	}
}

func TestConsolidationNilModelBatchKeepsPending(t *testing.T) {
	repo := consolidationFixture()
	repo.change("", func(s *Snapshot) {
		s.Version = 20
		s.Records = nil
		for i := 1; i <= 20; i++ {
			s.Records = append(s.Records, Record{Kind: Automatic, Sequence: int64(i), Content: strings.Repeat("界", 1300)})
		}
	})
	changed, err := NewConsolidator(NewStore(repo), nil, "").Consolidate(t.Context())
	if !changed || err != nil {
		t.Fatalf("changed=%v err=%v", changed, err)
	}
	s, _ := repo.MemorySnapshot(t.Context(), "")
	if s.View.Version <= 0 || s.View.Version >= 20 || len(s.View.Main) > maxMainBytes || !utf8.ValidString(s.View.Main) {
		t.Fatalf("incorrect raw batch: version=%d bytes=%d", s.View.Version, len(s.View.Main))
	}
}

func TestConsolidationScopeFailuresDoNotBlockHealthyScopes(t *testing.T) {
	for _, mode := range []string{"build", "snapshot", "publish"} {
		t.Run(mode, func(t *testing.T) {
			repo := consolidationFixture()
			failure := errors.New("scope repository failure")
			switch mode {
			case "build":
				repo.change("", func(s *Snapshot) {
					for i := range 9 {
						s.Records = append(s.Records, Record{Kind: Explicit, Sequence: int64(i + 2), Content: strings.Repeat("x", 4000)})
					}
					s.Version = 10
				})
			case "snapshot":
				repo.snapshotErrors = map[string]error{"": failure}
			case "publish":
				repo.publishErrors = map[string]error{"": failure}
			}
			// A second failing scope verifies errors.Join retains every error.
			secondFailure := errors.New("second scope failure")
			if repo.snapshotErrors == nil {
				repo.snapshotErrors = make(map[string]error)
			}
			repo.snapshotErrors["/middle"] = secondFailure
			repo.snapshots["/middle"] = Snapshot{Scope: "/middle", Version: 1, Epoch: 1}
			repo.snapshots["/healthy"] = Snapshot{Scope: "/healthy", Version: 1, Epoch: 1, Records: []Record{{Kind: Automatic, Sequence: 1, Content: "healthy evidence"}}}
			model := &consolidationModel{}
			changed, err := NewConsolidator(NewStore(repo), model, "").Consolidate(t.Context())
			if !changed || err == nil || !errors.Is(err, secondFailure) || (mode != "build" && !errors.Is(err, failure)) {
				t.Fatalf("changed=%v err=%v", changed, err)
			}
			if mode == "build" && !strings.Contains(err.Error(), "explicit constraints") {
				t.Fatalf("missing build error: %v", err)
			}
			pending, pendingErr := repo.MemoryScopes(t.Context())
			if pendingErr != nil || !slices.Equal(pending, []string{"", "/middle"}) || repo.publishes != 1 {
				t.Fatalf("pending=%v err=%v publishes=%d", pending, pendingErr, repo.publishes)
			}
			healthy, _ := repo.MemorySnapshot(t.Context(), "/healthy")
			if !healthy.ValidView() || healthy.View.Main != "view" || healthy.View.Summary != "view" {
				t.Fatalf("healthy scope not published: %+v", healthy)
			}
		})
	}
}

func TestConsolidationProviderContextErrorDoesNotStopHealthyScopes(t *testing.T) {
	for _, providerErr := range []error{context.DeadlineExceeded, context.Canceled} {
		for _, phase := range []string{"main", "summary"} {
			t.Run(providerErr.Error()+"/"+phase, func(t *testing.T) {
				repo := consolidationFixture()
				repo.snapshots["/healthy"] = Snapshot{Scope: "/healthy", Version: 1, Epoch: 1, Records: []Record{{Kind: Automatic, Sequence: 1, Content: "healthy evidence"}}}
				failed := &consolidationStream{err: fmt.Errorf("provider-local timeout/cancellation: %w", providerErr)}
				streams := []*consolidationStream{failed, normalConsolidationStream("healthy main"), normalConsolidationStream("healthy summary")}
				if phase == "summary" {
					streams = append([]*consolidationStream{normalConsolidationStream("failed scope main")}, streams...)
				}
				model := &consolidationModel{streams: streams}
				ctx := t.Context()
				changed, err := NewConsolidator(NewStore(repo), model, "model").Consolidate(ctx)
				if ctx.Err() != nil || !changed || !errors.Is(err, providerErr) || repo.publishes != 1 || len(model.requests) != len(streams) || !failed.closed {
					t.Fatalf("provider context error stopped pass: changed=%v err=%v calls=%d parent=%v", changed, err, len(model.requests), ctx.Err())
				}
				pending, pendingErr := repo.MemoryScopes(ctx)
				if pendingErr != nil || !slices.Equal(pending, []string{""}) {
					t.Fatalf("pending=%v err=%v, want only failed scope", pending, pendingErr)
				}
				healthy, err := repo.MemorySnapshot(ctx, "/healthy")
				if err != nil || !healthy.ValidView() || healthy.View.Main != "healthy main" || healthy.View.Summary != "healthy summary" {
					t.Fatalf("healthy scope was not published: snapshot=%+v err=%v", healthy, err)
				}
			})
		}
	}
}

func TestConsolidationCancellationPreservesProgressAndStopsScopes(t *testing.T) {
	for _, phase := range []string{"build", "publish"} {
		t.Run(phase, func(t *testing.T) {
			repo := consolidationFixture()
			priorFailure := errors.New("prior scope failed")
			repo.snapshotErrors = map[string]error{"": priorFailure}
			for _, scope := range []string{"/a-success", "/b-cancel", "/c-unvisited"} {
				repo.snapshots[scope] = Snapshot{Scope: scope, Version: 1, Epoch: 1, Records: []Record{{Kind: Automatic, Sequence: 1, Content: scope}}}
			}
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			model := &consolidationModel{}
			if phase == "build" {
				model.hook = func(i int) {
					if i == 2 {
						cancel()
					}
				}
			} else {
				repo.publishHook = func(scope string) {
					if scope == "/b-cancel" {
						cancel()
					}
				}
			}
			changed, err := NewConsolidator(NewStore(repo), model, "").Consolidate(ctx)
			if !changed || !errors.Is(err, context.Canceled) || !errors.Is(err, priorFailure) || repo.publishes != 1 {
				t.Fatalf("changed=%v err=%v publishes=%d", changed, err, repo.publishes)
			}
			for _, scope := range []string{"/b-cancel", "/c-unvisited"} {
				snapshot, _ := repo.MemorySnapshot(t.Context(), scope)
				if snapshot.ValidView() {
					t.Fatalf("cancelled/unvisited scope published: %s", scope)
				}
			}
			wantCalls := 3
			if phase == "publish" {
				wantCalls = 4
			}
			if len(model.requests) != wantCalls {
				t.Fatalf("continued model work after cancellation: calls=%d want=%d", len(model.requests), wantCalls)
			}
		})
	}
}

func TestConsolidationExplicitAdditionRebuildKeepsSources(t *testing.T) {
	repo := consolidationFixture()
	repo.change("", func(s *Snapshot) {
		s.Version = 2
		s.Epoch = 2
		s.View = View{Version: 1, Epoch: 1, Main: "old derived claim use Go"}
		s.Records = append(s.Records, Record{ID: "explicit", Kind: Explicit, Sequence: 2, Content: "prefer Rust instead"})
	})
	model := &consolidationModel{streams: []*consolidationStream{normalConsolidationStream("unrelated automatic knowledge"), normalConsolidationStream("unrelated summary")}}
	if ok, err := NewConsolidator(NewStore(repo), model, "").Consolidate(t.Context()); !ok || err != nil {
		t.Fatalf("%v %v", ok, err)
	}
	var prompt string
	for _, msg := range model.requests[0].Messages {
		prompt += strings.Join(msg.TextParts(), "\n")
	}
	if !strings.Contains(prompt, "prefer Rust instead") || !strings.Contains(prompt, "use Go") || strings.Contains(prompt, "old derived claim") {
		t.Fatalf("rebuild input=%s", prompt)
	}
	s, _ := repo.MemorySnapshot(t.Context(), "")
	if s.View.Version != 2 || s.View.Epoch != 2 || len(s.Records) != 2 || s.Records[0].Content != "use Go" || s.Records[1].Content != "prefer Rust instead" {
		t.Fatalf("sources modified or view wrong: %+v", s)
	}
	if strings.Contains(s.View.Main+s.View.Summary, "prefer Rust instead") {
		t.Fatal("explicit constraints duplicated into view")
	}
}

func TestConsolidationExplicitConstraintsBudget(t *testing.T) {
	for _, count := range []int{7, 9} {
		t.Run(fmt.Sprint(count), func(t *testing.T) {
			repo := consolidationFixture()
			repo.change("", func(s *Snapshot) {
				s.Version = int64(count + 1)
				for i := range count {
					s.Records = append(s.Records, Record{ID: fmt.Sprint(i), Kind: Explicit, Sequence: int64(i + 2), Content: strings.Repeat("界", 1300)})
				}
			})
			model := &consolidationModel{}
			ok, err := NewConsolidator(NewStore(repo), model, "").Consolidate(t.Context())
			if count == 9 {
				pending, _ := repo.MemoryScopes(t.Context())
				if ok || err == nil || repo.publishes != 0 || len(model.requests) != 0 || len(pending) != 1 {
					t.Fatalf("constraints silently truncated: %v %v", ok, err)
				}
				return
			}
			if !ok || err != nil {
				t.Fatalf("bounded constraints rejected: %v %v", ok, err)
			}
			total := 0
			for _, msg := range model.requests[0].Messages {
				for _, text := range msg.TextParts() {
					total += len(text)
				}
			}
			if total > maxConsolidationInputBytes {
				t.Fatalf("oversized input: %d", total)
			}
		})
	}
}

func TestConsolidationNilModelWithExplicitConstraintsLeavesPending(t *testing.T) {
	repo := consolidationFixture()
	repo.change("", func(s *Snapshot) {
		s.Version++
		s.Epoch++
		s.Records = append(s.Records, Record{Kind: Explicit, Sequence: 2, Content: "prefer Rust instead"})
	})
	ok, err := NewConsolidator(NewStore(repo), nil, "").Consolidate(t.Context())
	pending, _ := repo.MemoryScopes(t.Context())
	if ok || err == nil || repo.publishes != 0 || len(pending) != 1 {
		t.Fatalf("unsafe raw view published: %v %v", ok, err)
	}
}

func TestConsolidationEpochRebuildIgnoresOldView(t *testing.T) {
	repo := consolidationFixture()
	repo.change("", func(s *Snapshot) { s.Epoch = 2; s.View = View{Version: 1, Epoch: 1, Main: "revoked old view"} })
	model := &consolidationModel{}
	if ok, err := NewConsolidator(NewStore(repo), model, "").Consolidate(t.Context()); !ok || err != nil {
		t.Fatalf("%v %v", ok, err)
	}
	for _, msg := range model.requests[0].Messages {
		if strings.Contains(strings.Join(msg.TextParts(), ""), "revoked old view") {
			t.Fatal("invalid epoch view reused")
		}
	}
}
