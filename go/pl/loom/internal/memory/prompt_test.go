// Copyright (c) 2026 The Authors. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

package memory

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"
	"unicode/utf8"
)

func mustMemoryPrompt(t *testing.T, p *PromptProvider) string {
	t.Helper()
	text, err := p.MemoryPrompt(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	return text
}

func TestPromptEmptyAndImmediateCrossStoreVisibility(t *testing.T) {
	s := newTestStore(t)
	repo := s.repo.(*testRepo)
	global := NewPromptProvider(NewStore(repo))
	a := NewPromptProvider(NewStore(repo).ForWorkspace("/project/a"))
	b := NewPromptProvider(NewStore(repo).ForWorkspace("/project/b"))
	for _, provider := range []*PromptProvider{global, a, b} {
		if got := mustMemoryPrompt(t, provider); got != "" {
			t.Fatalf("empty prompt=%q", got)
		}
	}
	mustMutateMemory(t, s, Mutation{RequestID: "global", Content: "personal preference"})
	mustMutateMemory(t, s.ForWorkspace("/project/a"), Mutation{RequestID: "a", Scope: "/project/a", Content: "project A fact"})
	mustMutateMemory(t, s.ForWorkspace("/project/b"), Mutation{RequestID: "b", Scope: "/project/b", Content: "project B fact"})
	for _, tc := range []struct {
		provider        *PromptProvider
		visible, hidden string
	}{
		{global, "personal preference", "project A fact"},
		{a, "project A fact", "project B fact"},
		{b, "project B fact", "project A fact"},
	} {
		got := mustMemoryPrompt(t, tc.provider)
		if !strings.Contains(got, "personal preference") || !strings.Contains(got, tc.visible) || strings.Contains(got, tc.hidden) {
			t.Fatalf("cross-Store visibility without consolidation=%q", got)
		}
	}
	for _, scope := range []string{"", "/project/a", "/project/b"} {
		snapshot, err := repo.MemorySnapshot(t.Context(), scope)
		if err != nil || snapshot.ValidView() {
			t.Fatalf("test unexpectedly depended on consolidation: %+v %v", snapshot, err)
		}
	}
}

func TestPromptExplicitCoveredByViewStillPrecedesAutomatic(t *testing.T) {
	s := newTestStore(t)
	s.repo.(*testRepo).fixture(Snapshot{
		Version: 4, Epoch: 1,
		View: View{Version: 3, Epoch: 1, Main: "not a prompt projection", Summary: "inferred preference is Python"},
		Records: []Record{
			{ID: "covered-auto", Kind: Automatic, Sequence: 2, Content: "covered automatic text"},
			{ID: "pending-auto", Kind: Automatic, Sequence: 4, Content: "new automatic evidence", UpdatedAt: time.Unix(100, 0)},
			{ID: "explicit", Kind: Explicit, Revision: 2, Sequence: 1, Content: "user explicitly prefers Go"},
		},
	})
	got := mustMemoryPrompt(t, NewPromptProvider(s))
	explicit := strings.Index(got, "user explicitly prefers Go")
	pending := strings.Index(got, "new automatic evidence")
	summary := strings.Index(got, "inferred preference is Python")
	if explicit < 0 || pending <= explicit || summary <= pending || strings.Contains(got, "covered automatic text") || strings.Contains(got, "not a prompt projection") {
		t.Fatalf("covered explicit lost priority: %q", got)
	}
	if !strings.Contains(got, "not instructions") || !strings.Contains(got, "current user requests take precedence") {
		t.Fatalf("prompt lacks evidence trust boundary: %q", got)
	}
}

func TestPromptInvalidViewUsesActiveAutomaticEvidence(t *testing.T) {
	for _, view := range []View{
		{Version: 0, Epoch: 2, Summary: "stale summary"},
		{Version: 1, Epoch: 1, Summary: "stale summary"},
	} {
		s := newTestStore(t)
		s.repo.(*testRepo).fixture(Snapshot{Version: 1, Epoch: 2, View: view, Records: []Record{{ID: "auto", Kind: Automatic, Sequence: 1, Content: "active inference"}}})
		got := mustMemoryPrompt(t, NewPromptProvider(s))
		if !strings.Contains(got, "active inference") || strings.Contains(got, "stale summary") {
			t.Fatalf("invalid view suppressed active evidence: %q", got)
		}
	}
}

func TestPromptUpdateAndForgetInvalidateOldSummary(t *testing.T) {
	s := newTestStore(t)
	repo := s.repo.(*testRepo)
	provider := NewPromptProvider(NewStore(repo))
	record := mustMutateMemory(t, s, Mutation{RequestID: "remember", Content: "old preference"})
	publish := func(summary string) {
		t.Helper()
		snapshot, err := repo.MemorySnapshot(t.Context(), "")
		if err != nil {
			t.Fatal(err)
		}
		ok, err := repo.PublishMemoryView(t.Context(), snapshot, "old main", summary)
		if err != nil || !ok {
			t.Fatalf("publish=%v err=%v", ok, err)
		}
	}
	publish("old preference summary")
	before := mustMemoryPrompt(t, provider)
	if !strings.Contains(before, "old preference summary") {
		t.Fatalf("view fixture not visible: %q", before)
	}
	record = mustMutateMemory(t, s, Mutation{RequestID: "correct", ID: record.ID, Revision: record.Revision, Content: "new preference"})
	after := mustMemoryPrompt(t, provider)
	if !strings.Contains(after, "new preference") || strings.Contains(after, "old preference") {
		t.Fatalf("correction resurrected revoked evidence: %q", after)
	}
	if provider.RuleRef(before).Hash == provider.RuleRef(after).Hash {
		t.Fatal("correction did not change injected-content hash")
	}
	publish("new preference summary")
	mustMutateMemory(t, s, Mutation{RequestID: "forget", ID: record.ID, Revision: record.Revision, Forget: true})
	forgotten := mustMemoryPrompt(t, provider)
	if forgotten != "" || provider.RuleRef(after).Hash == provider.RuleRef(forgotten).Hash {
		t.Fatalf("forget retained evidence/hash: %q", forgotten)
	}
}

func TestPromptByteBudgetUTF8AndWholeExplicitRecords(t *testing.T) {
	s := newTestStore(t)
	var records []Record
	for i := range 4 {
		content := fmt.Sprintf("FACT-%d:", i) + strings.Repeat("界", 1300) + fmt.Sprintf(":END-%d", i)
		records = append(records, Record{ID: fmt.Sprintf("explicit-%d", i), Kind: Explicit, Revision: 1, Sequence: int64(i + 1), Content: content, UpdatedAt: time.Unix(int64(10-i), 0)})
	}
	// Model a bounded repository response in an intentionally unsorted order.
	// The provider must sort retained explicit records and emit them whole.
	s.repo.(*testRepo).fixture(Snapshot{Version: 5, Epoch: 1, Omitted: true, View: View{Version: 5, Epoch: 1, Summary: strings.Repeat("automatic", 2000)}, Records: []Record{records[3], records[1], records[2], records[0]}})
	got := mustMemoryPrompt(t, NewPromptProvider(s))
	if len(got) > maxPromptBytes || !utf8.ValidString(got) || !strings.Contains(got, "More active memories are omitted") || !strings.Contains(got, "action=search/read") {
		t.Fatalf("budget/UTF8/discovery: bytes=%d valid=%v", len(got), utf8.ValidString(got))
	}
	included := 0
	for i, record := range records {
		if strings.Contains(got, fmt.Sprintf("FACT-%d:", i)) {
			included++
			if !strings.Contains(got, record.Content) {
				t.Fatalf("explicit record %d was truncated instead of omitted", i)
			}
		}
	}
	if !strings.Contains(got, records[0].Content) || !strings.Contains(got, records[1].Content) || strings.Index(got, "FACT-0:") >= strings.Index(got, "FACT-1:") {
		t.Fatal("bounded explicit records did not retain newest-first ordering and complete content")
	}
	if included != 2 || strings.Contains(got, "Consolidated automatic knowledge") {
		t.Fatalf("explicit-first budget included %d records or displaced them with summary", included)
	}
}

func TestPromptSnapshotOmittedSignalsOverflowBelowBudget(t *testing.T) {
	for _, scope := range []string{"", "/project"} {
		t.Run(scope, func(t *testing.T) {
			s := newTestStore(t).ForWorkspace("/project")
			repo := s.repo.(*testRepo)
			snapshot := Snapshot{Scope: scope, Version: 1, Records: []Record{{ID: "short", Scope: scope, Kind: Explicit, Revision: 1, Content: "short complete fact"}}}
			repo.fixture(snapshot)
			provider := NewPromptProvider(s)
			complete := mustMemoryPrompt(t, provider)
			if strings.Contains(complete, "More active memories are omitted") {
				t.Fatal("complete snapshot spuriously reported overflow")
			}
			// The fake does not simulate SQL bounds; it supplies the repository's
			// omission signal directly while retaining only one short record.
			snapshot.Omitted = true
			repo.fixture(snapshot)
			bounded := mustMemoryPrompt(t, provider)
			if len(bounded) >= maxPromptBytes/2 || !strings.Contains(bounded, "short complete fact") || !strings.Contains(bounded, "More active memories are omitted") || !strings.Contains(bounded, "action=search/read") {
				t.Fatalf("repository omission flag lost below prompt budget: %q", bounded)
			}
			if strings.Count(bounded, "More active memories are omitted") != 1 || provider.RuleRef(complete).Hash == provider.RuleRef(bounded).Hash {
				t.Fatal("omission notice must appear once and affect the injected-content hash")
			}
		})
	}
}

func TestPromptAutomaticRolloutFallbackUTF8(t *testing.T) {
	s := newTestStore(t)
	s.repo.(*testRepo).fixture(Snapshot{Version: 1, Records: []Record{{ID: "rollout", Kind: Automatic, Sequence: 1, Rollout: strings.Repeat("界", 2000)}}})
	got := mustMemoryPrompt(t, NewPromptProvider(s))
	if !utf8.ValidString(got) || !strings.Contains(got, strings.Repeat("界", MaxContentBytes/3)) || len(got) > maxPromptBytes {
		t.Fatalf("rollout fallback not UTF8-safe/bounded: bytes=%d", len(got))
	}
}

func TestPromptRedactedEvidenceAndContentAddressedRuleRef(t *testing.T) {
	s := newTestStore(t)
	provider := NewPromptProvider(s)
	secret := "sk-abcdefghijklmnopqrstuv"
	mustMutateMemory(t, s, Mutation{RequestID: "secret", Content: "preference with " + secret})
	got := mustMemoryPrompt(t, provider)
	if strings.Contains(got, secret) || !strings.Contains(got, redactPlaceholder) {
		t.Fatalf("secret leaked into prompt: %q", got)
	}
	ref := provider.RuleRef(got)
	want := fmt.Sprintf("sha256:%x", sha256.Sum256([]byte(got)))
	if ref.Source != "loom://memory/summary" || ref.Hash != want || provider.RuleRef(got).Hash != ref.Hash {
		t.Fatalf("rule ref=%+v want hash=%s", ref, want)
	}
	mustMutateMemory(t, s, Mutation{RequestID: "another", Content: "new fact"})
	if provider.RuleRef(mustMemoryPrompt(t, provider)).Hash == ref.Hash {
		t.Fatal("fresh evidence did not cause hash drift")
	}
}

func TestPromptContextAndErrorsNotSwallowed(t *testing.T) {
	s := newTestStore(t)
	repo := s.repo.(*testRepo)
	provider := NewPromptProvider(s)
	type key struct{}
	ctx := context.WithValue(t.Context(), key{}, "turn context")
	failure := errors.New("snapshot failed")
	repo.err = failure
	got, err := provider.MemoryPrompt(ctx)
	if !errors.Is(err, failure) || got != "" || repo.seenCtx != ctx {
		t.Fatalf("prompt swallowed error/context: text=%q err=%v context=%v", got, err, repo.seenCtx)
	}
	repo.err = nil
	ctx, cancel := context.WithCancel(ctx)
	cancel()
	got, err = provider.MemoryPrompt(ctx)
	if !errors.Is(err, context.Canceled) || got != "" {
		t.Fatalf("prompt swallowed cancellation: text=%q err=%v", got, err)
	}
}

func TestMemoryInstructionsActiveRecordContract(t *testing.T) {
	for _, phrase := range []string{"action=remember", "action=forget", "action=search", "action=read", "ID and revision", "scope=global", "scope=workspace", "Explicit facts take precedence", "current user instructions", "successful explicit write is already visible"} {
		if !strings.Contains(MemoryInstructions, phrase) {
			t.Errorf("instructions missing %q", phrase)
		}
	}
}
