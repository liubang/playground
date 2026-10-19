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

package app

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// Session title sidecar: as a session's first turn starts, one cheap
// internal model call summarizes the opening prompt into the persisted
// picker title (sessions.title, schema v11). Like the memory pipeline, the
// call never touches the transcript or the UI event stream, and every
// failure degrades silently to the listing-time fallback (the truncated
// first prompt) — the sidecar must never affect the session itself.
const (
	// titleGenTimeout bounds the whole fire-and-forget sidecar run.
	titleGenTimeout = 45 * time.Second
	// titleGenMaxTokens caps the title response (a title is ≤ 50 runes;
	// the headroom absorbs reasoning-capable models that spend tokens
	// before the visible answer).
	titleGenMaxTokens = 1024
	// titlePromptMaxRunes bounds the opening prompt fed to the title
	// model — long pastes and logs cost input tokens without improving
	// the title.
	titlePromptMaxRunes = 2000
	// titleGenAttempts bounds the sidecar's own retries of transient
	// failures: a rate-limited gateway rejects the fire-and-forget title
	// call exactly when the main request is fighting the same window, and
	// the one-shot trigger means a dropped first attempt loses the title
	// forever. Retrying inside the (already bounded) sidecar budget keeps
	// the failure silent while giving the window time to clear.
	titleGenAttempts = 3
	// titleGenRetryWait seeds the linear retry spacing (5s, 10s).
	titleGenRetryWait = 5 * time.Second
)

const titleSystemPrompt = `You name conversations for a coding assistant's session list. Given the user's first message, reply with a concise title for the conversation. Rules:
- at most 50 characters
- use the same language as the user's message
- a single line of plain text: no quotes, no markdown, no trailing punctuation
Output the title and nothing else.`

// TitleSidecar generates and persists session titles. It is wired by
// NewProcessRuntime; bare (hand-assembled) runtimes leave it nil, which
// disables generation — the same contract as MemoryJobQueue.
type TitleSidecar struct {
	store    *session.SQLiteStore
	resolved func() *config.ResolvedConfig
	logger   *slog.Logger
	// attempts bounds the sidecar's own retries of transient failures;
	// zero selects titleGenAttempts. retryWait seeds the linear retry
	// spacing; zero selects titleGenRetryWait. Tests shrink both to keep
	// the suite fast.
	attempts  int
	retryWait time.Duration
}

// NewTitleSidecar assembles the sidecar; resolved is read per run so a
// config hot-apply (including sessions.title_model) takes effect on the
// next session.
func NewTitleSidecar(store *session.SQLiteStore, resolved func() *config.ResolvedConfig, logger *slog.Logger) *TitleSidecar {
	if store == nil || resolved == nil {
		return nil
	}
	if logger == nil {
		logger = slog.Default()
	}
	return &TitleSidecar{store: store, resolved: resolved, logger: logger}
}

// Generate launches the fire-and-forget title run for a session whose
// first turn just started (the opening prompt is all the input the title
// needs). prompt is the user's raw submission (pre @file/skill
// enrichment); ref is the session's effective model — the
// sessions.title_model pin overrides it. onDone, when non-nil, is invoked
// exactly once with the persisted title (this run won the write) or ""
// (every skip and failure path) — callers use it to clear their
// title-pending marker even when the run is a no-op. A nil sidecar, a
// missing provider, and every model or persistence failure simply skip
// the generation: untitled sessions keep the truncated-prompt fallback.
func (t *TitleSidecar) Generate(sessionID domain.SessionID, ref config.ProviderModelRef, prompt string, onDone func(string)) {
	done := func(title string) {
		if onDone != nil {
			onDone(title)
		}
	}
	if t == nil || sessionID.IsZero() || strings.TrimSpace(prompt) == "" {
		done("")
		return
	}
	resolved := t.resolved()
	if resolved == nil {
		done("")
		return
	}
	if pinned := resolved.Sessions.TitleModel; pinned != "" {
		// Validated at config load; a resolve miss just keeps the follow
		// behavior.
		if r, err := resolved.ResolveRef(pinned); err == nil {
			ref = r
		}
	}
	provider := resolved.ProviderByName(ref.Provider)
	if provider == nil {
		done("")
		return
	}
	model := provider.ModelFor(ref.Model)

	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), titleGenTimeout)
		defer cancel()
		// Two loom processes can share one database: skip the model call
		// whenever a title already exists rather than racing the
		// conditional write afterwards.
		if existing, err := t.store.SessionTitle(ctx, sessionID); err != nil || existing != "" {
			done("")
			return
		}
		title, err := runTitleCall(ctx, model, ref.Model, prompt)
		attempts := t.attempts
		if attempts <= 0 {
			attempts = titleGenAttempts
		}
		retryWait := t.retryWait
		if retryWait <= 0 {
			retryWait = titleGenRetryWait
		}
		for attempt := 1; err != nil && domain.IsRetryable(err) && attempt < attempts; attempt++ {
			if waitErr := waitTitleRetry(ctx, retryWait*time.Duration(attempt)); waitErr != nil {
				break
			}
			title, err = runTitleCall(ctx, model, ref.Model, prompt)
		}
		if err != nil {
			t.logger.Warn("session title generation failed", "session", sessionID, "error", err)
			done("")
			return
		}
		won, err := t.store.SetSessionTitle(ctx, sessionID, title)
		if err != nil {
			t.logger.Warn("session title persist failed", "session", sessionID, "error", err)
			done("")
			return
		}
		if won {
			done(title)
		} else {
			done("")
		}
	}()
}

// maybeGenerateSessionTitle hands the first turn's raw prompt to the title
// sidecar; a persisted title is announced to live clients as session.titled.
// It fires at most once per session-on-controller: the turn-start attempt
// and the turn-end recovery share the one-shot so a slow sidecar call is
// never duplicated by the recovery trigger (a failed attempt simply leaves
// the session on the truncated-prompt fallback). The gate lives here (not
// in the sidecar) because turn count and the delegation flag are controller
// state. hadHistory reports whether the transcript held messages BEFORE
// this turn's user prompt landed (captured by the caller: the projection
// already carries the new prompt by trigger time) — a resumed conversation
// must not be titled after its NEW first submission, since the write-once
// title would describe "继续" instead of the conversation; such sessions
// keep the truncated-prompt fallback, which IS their historical first
// prompt.
func (c *Controller) maybeGenerateSessionTitle(turn int, prompt string, hadHistory bool) {
	if turn != 1 {
		return
	}
	c.mu.Lock()
	// A nil sidecar (bare runtimes) skips here so titlePending never
	// flickers for listings.
	sidecar := c.bootstrap.TitleSidecar
	if c.titleTriggered || c.delegated || hadHistory || sidecar == nil {
		c.mu.Unlock()
		return
	}
	c.titleTriggered = true
	c.titlePending = true
	sessionID, runID, ref := c.sessionID, c.runID, c.currentLocked()
	c.mu.Unlock()
	sidecar.Generate(sessionID, ref, prompt, func(title string) {
		c.mu.Lock()
		c.titlePending = false
		c.mu.Unlock()
		if title != "" {
			c.publishEphemeral(sessionID, runID, turn, runtimeevent.KindSessionTitled, runtimeevent.SessionTitledPayload{Title: title})
		}
	})
}

// TitlePending reports whether this session's first-turn title sidecar is
// still in flight. SessionService suppresses the truncated-prompt fallback
// while it is, so pickers keep their placeholder instead of flashing a
// fragment the generated title replaces seconds later — without hiding the
// fallback on every busy turn of a session whose generation failed.
func (c *Controller) TitlePending() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.titlePending
}

// waitTitleRetry sleeps out one sidecar retry backoff, returning early
// with the context error when the sidecar's timeout fires mid-wait.
func waitTitleRetry(ctx context.Context, d time.Duration) error {
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}

// runTitleCall makes the one-shot title request and returns the cleaned
// title. No tools are offered and the stream is consumed off the UI event
// path — the sidecar is invisible in the transcript.
func runTitleCall(ctx context.Context, model domain.Model, modelName, prompt string) (string, error) {
	promptRunes := []rune(prompt)
	if len(promptRunes) > titlePromptMaxRunes {
		prompt = string(promptRunes[:titlePromptMaxRunes])
	}
	textPart := func(text string) []domain.ContentPart {
		return []domain.ContentPart{{Kind: domain.PartText, Text: text}}
	}
	now := time.Now()
	stream, err := model.Stream(ctx, domain.ModelRequest{
		ModelName: modelName,
		Messages: []domain.Message{
			{ID: domain.NewMessageID(), Role: domain.RoleSystem, Parts: textPart(titleSystemPrompt), CreatedAt: now},
			{ID: domain.NewMessageID(), Role: domain.RoleUser, Parts: textPart(prompt), CreatedAt: now},
		},
		MaxTokens:   titleGenMaxTokens,
		Temperature: 0,
	})
	if err != nil {
		return "", err
	}
	defer stream.Close()

	var text strings.Builder
	var streamErr string
	var streamErrCode domain.ErrorCode
	var streamRetryable bool
	for {
		event, err := stream.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			return "", err
		}
		switch event.Kind {
		case domain.ModelEventTextDelta:
			text.WriteString(event.TextDelta)
		case domain.ModelEventStreamError:
			// Providers report mid-stream failures (connection reset, server
			// error) as EVENTS — Recv still ends on io.EOF. The partial text
			// collected before the break must not become the permanent
			// write-once title.
			streamErr = event.Error
			streamErrCode = event.ErrorCode
			streamRetryable = event.Retryable
		}
	}
	if streamErr != "" {
		if streamRetryable {
			// Keep the provider's classification so the sidecar retries rate
			// limits and transient failures instead of dropping the title.
			code := streamErrCode
			if code == "" {
				code = domain.ErrUnavailable
			}
			return "", domain.NewError(code, streamErr, domain.WithRetryable(true))
		}
		return "", errors.New(streamErr)
	}
	title := cleanGeneratedTitle(text.String())
	if title == "" {
		return "", errors.New("model returned an empty title")
	}
	return title, nil
}

// cleanGeneratedTitle normalizes the model's raw answer into the picker
// title shape: first line only, decoration stripped, then whitespace
// collapsed and rune-truncated by summarizeSessionTitle — the same
// normalizer the fallback path uses, so both title sources render
// identically (its Fields join already absorbs edge whitespace, which the
// trim cutsets therefore leave out).
func cleanGeneratedTitle(raw string) string {
	text, _, _ := strings.Cut(raw, "\n")
	text = strings.TrimSpace(text)
	text = strings.Trim(text, "\"'`“”‘’*")
	text = strings.TrimRight(text, ".。!！?？:：;；,，")
	return summarizeSessionTitle(text)
}
