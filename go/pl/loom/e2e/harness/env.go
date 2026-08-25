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

// Package harness is the shared process-level assembly for loom e2e
// tests (docs/REPLAY_TESTING_DESIGN.md §5.4): one Env brings up the full
// serve path — process runtime, workspace bootstrap, event broker,
// session service — on an isolated loom home, and optionally wires the
// record/replay model layer:
//
//   - real mode (default): the user's own ~/.loom/config.yaml copied
//     into a temp home; gated on LOOM_E2E_LLM=1.
//   - record mode (WithRecording): real mode plus a RecordingModel
//     wrapping every provider, persisting calls.*.jsonl fixtures.
//   - replay mode (WithReplay): keyless; a synthetic config whose every
//     provider model is the ReplayModel loaded from a scenario dir.
package harness

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"gopkg.in/yaml.v3"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/client"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/model/replay"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// Env is one assembled serve-path stack on an isolated loom home.
type Env struct {
	Ctx         context.Context
	Home        string // temp loom home; all writable state derives from it
	Workspace   string
	ArtifactDir string
	Resolved    *config.ResolvedConfig
	Proc        *app.ProcessRuntime
	Broker      *runtimeevent.Broker
	Svc         *app.SessionService
	Logger      *slog.Logger

	recorder    *replay.Recorder
	replayModel *replay.ReplayModel
	tap         *Tap
	closeProc   func()
	rawConfig   []byte // the config bytes this env loaded (real/record mode)
}

type envConfig struct {
	configRaw []byte
	adjust    func(*config.ResolvedConfig)
	recordDir string
	replayDir string
}

// Option customizes Env assembly.
type Option func(*envConfig)

// WithConfigRaw replaces the user-config bytes loaded in real/record
// mode (e.g. a modality-patched copy).
func WithConfigRaw(raw []byte) Option {
	return func(c *envConfig) { c.configRaw = raw }
}

// WithAdjust mutates the resolved config before the stack boots (e.g.
// neutralizing the memory pipeline's timers).
func WithAdjust(f func(*config.ResolvedConfig)) Option {
	return func(c *envConfig) { c.adjust = f }
}

// WithRecording selects record mode: real provider calls, every model
// call persisted under dir (REPLAY_TESTING_DESIGN §3). Still gated on
// LOOM_E2E_LLM=1 — recording IS the real-model acceptance run.
func WithRecording(dir string) Option {
	return func(c *envConfig) { c.recordDir = dir }
}

// WithReplay selects replay mode: keyless, the model calls replay from
// the recorded fixtures under dir (REPLAY_TESTING_DESIGN §4).
func WithReplay(dir string) Option {
	return func(c *envConfig) { c.replayDir = dir }
}

// NewEnv assembles the stack and registers the full teardown on
// t.Cleanup (service shutdown, broker, bootstrap, process runtime).
func NewEnv(t *testing.T, opts ...Option) *Env {
	t.Helper()
	var cfg envConfig
	for _, opt := range opts {
		opt(&cfg)
	}
	if cfg.recordDir != "" && cfg.replayDir != "" {
		t.Fatal("harness: WithRecording and WithReplay are mutually exclusive")
	}

	env := &Env{
		Ctx:    context.Background(),
		Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	if cfg.replayDir != "" || cfg.recordDir != "" {
		// Record/replay runs use a STABLE scenario root: the recorded
		// stream embeds absolute tool-call paths as arbitrarily fragmented
		// argument deltas, which no tokenization can rewrite — replay must
		// run at the same path the record run used (the dsh fixed-length
		// spill-root idea, taken one step further).
		dir := cfg.replayDir
		if dir == "" {
			dir = cfg.recordDir
		}
		env.Home = stableScenarioHome(t, dir)
	}
	if cfg.replayDir != "" {
		env.setupReplayConfig(t, cfg.replayDir)
	} else {
		env.setupRealConfig(t, &cfg)
	}
	env.ArtifactDir = filepath.Join(env.Home, "artifacts")
	env.Workspace = filepath.Join(env.Home, "ws")
	if err := os.MkdirAll(env.Resolved.Storage.SessionsDir(), 0o700); err != nil {
		t.Fatalf("mkdir sessions: %v", err)
	}
	if err := os.MkdirAll(env.Workspace, 0o755); err != nil {
		t.Fatalf("mkdir workspace: %v", err)
	}

	scrubPaths := []replay.ScrubPath{
		{Prefix: env.ArtifactDir, Token: "{{artifacts}}"},
		{Prefix: env.Workspace, Token: "{{cwd}}"},
		{Prefix: env.Home, Token: "{{home}}"},
	}
	if cfg.recordDir != "" {
		env.installRecorder(t, cfg.recordDir, scrubPaths)
	}
	if cfg.replayDir != "" {
		env.installReplay(t, cfg.replayDir, scrubPaths)
	}
	env.StartStack(t)
	return env
}

// StartStack brings up the serve-path stack (process runtime, workspace
// bootstrap, broker, tap, session service) on the env's home and
// registers its teardown. NewEnv calls it once; tests that exercise
// crash/restart recovery shut the service down and call StartStack again
// to reboot the stack on the SAME home.
func (e *Env) StartStack(t *testing.T) {
	t.Helper()
	proc, err := app.NewProcessRuntime(e.Ctx, e.Resolved, app.ProcessRuntimeConfig{
		ArtifactDir: e.ArtifactDir,
		Version:     "e2e",
		Logger:      e.Logger,
	})
	if err != nil {
		t.Fatalf("NewProcessRuntime: %v", err)
	}
	e.Proc = proc
	// Close is observable by some suites (the memory pipeline asserts the
	// exit path is fast), so it must be idempotent: tests call CloseProc
	// themselves and the cleanup re-entry is a no-op.
	closeProc := sync.OnceFunc(proc.Close)
	e.closeProc = closeProc
	t.Cleanup(closeProc)

	bootstrap, err := app.NewWorkspaceBootstrap(e.Ctx, proc, app.BootstrapConfig{
		WorkspaceRoot: e.Workspace,
		// Pin the prompt environment: the byte/4 occupancy estimate counts
		// the system prompt, so host-derived Platform/Shell (darwin/arm64 +
		// /bin/zsh vs linux/amd64 + unset SHELL) would flip golden numbers
		// across platforms.
		PromptEnv: prompt.NewFixedEnvProvider(e.Workspace, "e2e/e2e", "e2e"),
	})
	if err != nil {
		t.Fatalf("NewWorkspaceBootstrap: %v", err)
	}
	t.Cleanup(func() { bootstrap.Close() })

	e.Broker = runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(e.Broker.Close)
	e.tap = NewTap(e.Broker)
	t.Cleanup(e.tap.Close)

	e.Svc = app.NewSingletonWorkspaceService(bootstrap, e.Broker, app.SessionServiceConfig{Logger: e.Logger})
	svc := e.Svc
	t.Cleanup(func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = svc.Shutdown(shutdownCtx)
	})
}

// CloseProc idempotently closes the current process runtime.
func (e *Env) CloseProc() { e.closeProc() }

// NewClient opens an in-process client on a fresh session.
func (e *Env) NewClient(t *testing.T) client.Client {
	t.Helper()
	c := client.NewInProc(e.Svc)
	if err := c.NewSession(e.Ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	return c
}

// Subscribe is the per-session event subscription used by collectors.
func (e *Env) Subscribe(t *testing.T, c client.Client) <-chan runtimeevent.RuntimeEvent {
	t.Helper()
	ch, err := c.SubscribeEvents(e.Ctx, 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	return ch
}

// OpenStoreReadOnly opens the isolated session store for post-turn
// inspection of the durable event log.
func (e *Env) OpenStoreReadOnly(t *testing.T) *session.SQLiteStore {
	t.Helper()
	store, err := session.OpenSQLiteStoreReadOnly(e.Ctx, e.Resolved.Storage.SessionDBPath())
	if err != nil {
		t.Fatalf("open session store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	return store
}

// ReplayModel returns the replay-mode model (nil in other modes).
func (e *Env) ReplayModel() *replay.ReplayModel { return e.replayModel }

// TapEvents returns every runtime event published so far, in publish
// order — the raw material of the snapshot golden (§5.1).
func (e *Env) TapEvents() []runtimeevent.RuntimeEvent { return e.tap.Events() }

// --- config setup ---

// setupRealConfig loads the user's own config (or the WithConfigRaw
// override) from a temp loom home. Real-model suites are gated on
// LOOM_E2E_LLM=1 so CI never pays for or depends on a live model.
func (e *Env) setupRealConfig(t *testing.T, cfg *envConfig) {
	t.Helper()
	if os.Getenv("LOOM_E2E_LLM") != "1" {
		t.Skip("set LOOM_E2E_LLM=1 to run the real-model acceptance suite")
	}
	raw := cfg.configRaw
	if raw == nil {
		raw = ReadRealUserConfig(t)
	}
	e.rawConfig = raw
	home, resolved := LoadIsolatedConfigAt(t, e.Home, raw)
	if cfg.adjust != nil {
		cfg.adjust(resolved)
	}
	e.Home = home
	e.Resolved = resolved
}

// recordedConfigFile is the sanitized copy of the record run's config.
// Replay loads it verbatim (only the model instances are swapped for the
// ReplayModel), so tool sets, limits, prompt composition, and context
// windows match the recording exactly — a window difference alone would
// change when compaction fires and diverge the golden for reasons that
// have nothing to do with the code.
const recordedConfigFile = "config.recorded.yaml"

// setupReplayConfig loads the record run's sanitized config; without it
// (a hand-authored fixture) it falls back to a minimal keyless provider.
func (e *Env) setupReplayConfig(t *testing.T, dir string) {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(dir, recordedConfigFile))
	if err != nil {
		raw = []byte(`default: replay/replay-model
providers:
  - name: replay
    type: openai
    base_url: http://127.0.0.1:9
    api_key: replay-keyless
    models:
      - name: replay-model
        context_window: 1048576
`)
	}
	home, resolved := LoadIsolatedConfigAt(t, e.Home, raw)
	e.Home = home
	e.Resolved = resolved
}

// installRecorder wraps every provider model with the RecordingModel and
// persists the sanitized config sidecar for replay-time reconstruction.
func (e *Env) installRecorder(t *testing.T, dir string, scrubPaths []replay.ScrubPath) {
	t.Helper()
	recorder, err := replay.NewRecorder(dir, replay.WithScrubPaths(scrubPaths...))
	if err != nil {
		t.Fatalf("NewRecorder: %v", err)
	}
	e.recorder = recorder
	for i := range e.Resolved.Providers {
		p := &e.Resolved.Providers[i]
		p.Model = recorder.Wrap(p.Model)
		for wire, inst := range p.WireModels {
			p.WireModels[wire] = recorder.Wrap(inst)
		}
	}
	t.Cleanup(func() {
		if err := recorder.Close(); err != nil {
			t.Errorf("recorder close: %v", err)
		}
	})

	if err := os.WriteFile(filepath.Join(dir, recordedConfigFile), sanitizeConfig(t, e.rawConfig), 0o600); err != nil {
		t.Fatalf("write %s: %v", recordedConfigFile, err)
	}
}

// sanitizeConfig strips every provider credential from a config copy so
// the record run's config can be committed as a fixture: api_key becomes
// a placeholder and api_key_env references are dropped. Every other
// section (tools, limits, prompt, memory, MCP) is preserved verbatim —
// replay must compose the same request header the record run did.
func sanitizeConfig(t *testing.T, raw []byte) []byte {
	t.Helper()
	dec := yaml.NewDecoder(strings.NewReader(string(raw)))
	var docs []any
	for {
		var doc any
		err := dec.Decode(&doc)
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			t.Fatalf("parse config for sanitizing: %v", err)
		}
		docs = append(docs, doc)
	}
	for _, doc := range docs {
		m, _ := doc.(map[string]any)
		providers, _ := m["providers"].([]any)
		for _, p := range providers {
			pm, _ := p.(map[string]any)
			if pm == nil {
				continue
			}
			delete(pm, "api_key_env")
			pm["api_key"] = "replay-keyless"
			// The gateway address is recording-machine infrastructure:
			// replay never contacts it (every model instance is the
			// ReplayModel), and a committed internal hostname is a leak.
			pm["base_url"] = "https://replay.invalid"
		}
		// Tracing credentials are secrets even though the replay host is
		// inert; the machine-local skill roots are unportable by
		// definition (and would resurface the catalog the fixtures
		// scrub).
		if tr, _ := m["tracing"].(map[string]any); tr != nil {
			tr["public_key"] = "replay-redacted"
			tr["secret_key"] = "replay-redacted"
		}
		if sk, _ := m["skills"].(map[string]any); sk != nil {
			// disabled lists installed skill NAMES — the same
			// machine-local (and potentially private) catalog the
			// fixtures scrub, and the catalog's removal already makes
			// the list irrelevant to replay.
			delete(sk, "extra_roots")
			delete(sk, "disabled")
		}
	}
	var sb strings.Builder
	enc := yaml.NewEncoder(&sb)
	for _, doc := range docs {
		// yaml.Encoder emits the multi-document separators itself.
		if err := enc.Encode(doc); err != nil {
			t.Fatalf("re-marshal sanitized config: %v", err)
		}
	}
	if err := enc.Close(); err != nil {
		t.Fatalf("re-marshal sanitized config: %v", err)
	}
	return []byte(sb.String())
}

// installReplay replaces every provider model with the ReplayModel, so
// any model selection resolves to the recorded fixtures.
func (e *Env) installReplay(t *testing.T, dir string, scrubPaths []replay.ScrubPath) {
	t.Helper()
	replayModel, err := replay.NewReplayModel(
		dir,
		replay.WithScrubPaths(scrubPaths...),
		replay.WithStrict(os.Getenv("LOOM_REPLAY_STRICT") == "1"),
		replay.WithWarnFunc(func(format string, args ...any) { t.Logf(format, args...) }),
	)
	// (scrub paths stay as a fingerprint defense-in-depth even though the
	// stable scenario home makes record and replay paths identical.)
	if err != nil {
		t.Fatalf("NewReplayModel: %v", err)
	}
	e.replayModel = replayModel
	for i := range e.Resolved.Providers {
		p := &e.Resolved.Providers[i]
		p.Model = replayModel
		for wire := range p.WireModels {
			p.WireModels[wire] = replayModel
		}
	}
}

// ReadRealUserConfig reads the user's own loom config for the
// real-model suites.
func ReadRealUserConfig(t *testing.T) []byte {
	t.Helper()
	home, err := config.HomeDir(os.LookupEnv)
	if err != nil {
		t.Skipf("no home dir: %v", err)
	}
	raw, err := os.ReadFile(config.ConfigPathForHome(home))
	if err != nil {
		t.Skipf("loom config not found at %s", config.ConfigPathForHome(home))
	}
	return raw
}

// LoadIsolatedConfigAt copies raw into a fresh temp loom home (or the
// explicit home, wiped first so no state leaks between runs) and loads
// it from there, so every writable location derives from the temp home
// and the user's stores stay untouched. Returns the temp home and the
// resolved config.
func LoadIsolatedConfigAt(t *testing.T, home string, raw []byte) (string, *config.ResolvedConfig) {
	t.Helper()
	if home == "" {
		home = t.TempDir()
	} else {
		if err := os.RemoveAll(home); err != nil {
			t.Fatalf("wipe loom home %s: %v", home, err)
		}
		if err := os.MkdirAll(home, 0o700); err != nil {
			t.Fatalf("mkdir loom home %s: %v", home, err)
		}
	}
	if err := os.WriteFile(config.ConfigPathForHome(home), raw, 0o600); err != nil {
		t.Fatalf("write isolated config: %v", err)
	}
	resolved, err := config.Load(home, config.LoadOptions{RequireProviders: true, Logger: slog.Default()}, os.LookupEnv)
	if err != nil {
		t.Skipf("load loom config: %v", err)
	}
	return home, resolved
}

// stableScenarioHome returns the fixed loom home for a snapshot scenario
// (/tmp/loom-snapshot/<scenario>/home) and registers its cleanup. Fixed
// across record and replay runs AND across test runners (go test vs the
// bazel sandbox assign different $TMPDIRs; /tmp is the one root both
// share) — see NewEnv. Concurrent runs of the SAME scenario in two
// processes would collide; go test runs package scenarios serially.
func stableScenarioHome(t *testing.T, scenarioDir string) string {
	t.Helper()
	scenario := filepath.Base(scenarioDir)
	root := filepath.Join(string(os.PathSeparator)+"tmp", "loom-snapshot", scenario)
	home := filepath.Join(root, "home")
	t.Cleanup(func() { _ = os.RemoveAll(root) })
	return home
}

// Tap collects every event published to the broker, in publish order.
type Tap struct {
	mu     sync.Mutex
	events []runtimeevent.RuntimeEvent
	done   func()
}

// NewTap subscribes to the broker and drains the subscription in the
// background until Close.
func NewTap(broker *runtimeevent.Broker) *Tap {
	ch, unsub := broker.Subscribe()
	tap := &Tap{done: unsub}
	go func() {
		for evt := range ch {
			tap.mu.Lock()
			tap.events = append(tap.events, evt)
			tap.mu.Unlock()
		}
	}()
	return tap
}

// Events returns a snapshot of every event collected so far.
func (t *Tap) Events() []runtimeevent.RuntimeEvent {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]runtimeevent.RuntimeEvent, len(t.events))
	copy(out, t.events)
	return out
}

// Close unsubscribes from the broker.
func (t *Tap) Close() { t.done() }
