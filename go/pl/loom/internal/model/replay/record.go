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

package replay

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// options carries the shared knobs for Recorder and ReplayModel.
type options struct {
	scrubPaths []ScrubPath
	strict     bool
	warnf      func(format string, args ...any)
}

// Option configures a Recorder or ReplayModel.
type Option func(*options)

// WithScrubPaths registers the environment-dependent path roots
// (workspace, loom home, artifact dir) and their stable tokens. The
// record side tokenizes fixtures with them, the replay side detokenizes
// and fingerprints with them — the harness passes the same tokens on
// both sides.
func WithScrubPaths(paths ...ScrubPath) Option {
	return func(o *options) { o.scrubPaths = append(o.scrubPaths, paths...) }
}

// WithStrict escalates a replay-time fingerprint mismatch from a warning
// to a call failure (LOOM_REPLAY_STRICT=1 semantics).
func WithStrict(strict bool) Option {
	return func(o *options) { o.strict = strict }
}

// WithWarnFunc sets the sink for non-fatal replay warnings (fingerprint
// drift). The harness wires it to t.Logf so the drift shows up in test
// output and review.
func WithWarnFunc(f func(format string, args ...any)) Option {
	return func(o *options) { o.warnf = f }
}

func resolveOptions(opts []Option) options {
	var out options
	out.warnf = func(string, ...any) {}
	for _, opt := range opts {
		opt(&out)
	}
	return out
}

// Recorder persists every call made through the models it wraps,
// sharded by session binding key: the root session appends to
// calls.jsonl, each delegated session to calls.<parent call ID>.jsonl
// (REPLAY_TESTING_DESIGN §3.3). Sharding by binding key — rather than
// one global call sequence — is what keeps parallel delegations
// replayable: the interleaving of concurrent sub-agent calls is not
// stable across runs, but each session's own call order is.
type Recorder struct {
	dir  string
	opts options

	mu      sync.Mutex
	writers map[string]*scriptWriter
	closed  bool
}

// NewRecorder creates a recorder persisting under dir (created if
// needed). Close must be called to flush and validate the recordings.
func NewRecorder(dir string, opts ...Option) (*Recorder, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("replay: create scenario dir: %w", err)
	}
	return &Recorder{dir: dir, opts: resolveOptions(opts), writers: make(map[string]*scriptWriter)}, nil
}

// Wrap returns a domain.Model that records every call passing through
// inner. All wrapped models of one process share this Recorder, so the
// root loop and every sub-agent write into the same scenario directory.
func (r *Recorder) Wrap(inner domain.Model) domain.Model {
	return &RecordingModel{inner: inner, rec: r}
}

// Close flushes and closes all per-session files. A call left open —
// the process died mid-stream — is reported, not silently completed.
func (r *Recorder) Close() error {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.closed = true
	var errs []error
	for _, w := range r.writers {
		if w.open {
			errs = append(errs, fmt.Errorf("replay: %s: call #%d has no terminator (process exited mid-stream)", w.path, w.seq))
		}
		if err := w.file.Close(); err != nil {
			errs = append(errs, fmt.Errorf("replay: close %s: %w", w.path, err))
		}
	}
	return errors.Join(errs...)
}

func (r *Recorder) writerFor(key string) (*scriptWriter, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return nil, fmt.Errorf("replay: recorder is closed")
	}
	if w, ok := r.writers[key]; ok {
		return w, nil
	}
	path := filepath.Join(r.dir, CallsFileFor(key))
	f, err := os.Create(path)
	if err != nil {
		return nil, fmt.Errorf("replay: create %s: %w", path, err)
	}
	w := &scriptWriter{file: f, path: path, scrubs: r.opts.scrubPaths}
	r.writers[key] = w
	return w, nil
}

// scriptWriter appends one session's calls to its calls.*.jsonl file.
type scriptWriter struct {
	mu     sync.Mutex
	file   *os.File
	path   string
	scrubs []ScrubPath
	seq    int
	open   bool
}

// skillsCatalogJSONRe is skillsCatalogRe's counterpart for MARSHALED
// JSON text, where the catalog's newlines appear JSON-escaped as the
// two-character \n sequence (the embedded request is json.RawMessage,
// so it keeps its single-level escaping inside the marshaled line).
var skillsCatalogJSONRe = regexp.MustCompile(`(?s)# Available Skills\\n.*?best alternative\.\\n\\n`)

// writeLine marshals one line, tokenizes the environment-dependent
// paths inside it (so the committed fixture is portable), strips the
// machine-local skills catalog (so the fixture commits no machine
// specific — and potentially private — skill descriptions), and appends
// it.
func (w *scriptWriter) writeLine(v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	line := ScrubPaths(string(data), w.scrubs)
	line = skillsCatalogJSONRe.ReplaceAllString(line, "")
	data = append([]byte(line), '\n')
	_, err = w.file.Write(data)
	return err
}

// callRecorder tracks the one in-flight call of a session. Model calls
// within one session are sequential, so a single open slot suffices;
// the mutex only guards against a buggy concurrent second call.
type callRecorder struct {
	w    *scriptWriter
	done bool
}

func (r *Recorder) beginCall(ctx context.Context, req domain.ModelRequest) (*callRecorder, error) {
	w, err := r.writerFor(BindingKeyFrom(ctx))
	if err != nil {
		return nil, err
	}
	request, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("replay: marshal request: %w", err)
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.open {
		return nil, fmt.Errorf("replay: %s: a second call started while call #%d is still open", w.path, w.seq)
	}
	w.seq++
	w.open = true
	if err := w.writeLine(callStartLine{
		Type:        lineCallStart,
		Seq:         w.seq,
		Fingerprint: Fingerprint(req, r.opts.scrubPaths),
		Request:     request,
	}); err != nil {
		return nil, fmt.Errorf("replay: write call_start: %w", err)
	}
	return &callRecorder{w: w}, nil
}

func (c *callRecorder) event(evt domain.ModelEvent) {
	c.w.mu.Lock()
	defer c.w.mu.Unlock()
	if c.done {
		return
	}
	_ = c.w.writeLine(eventLine{Type: lineEvent, ModelEvent: evt})
}

// terminate closes the call with its outcome; only the first call wins.
func (c *callRecorder) terminate(outcome CallOutcome, errText string) {
	c.w.mu.Lock()
	defer c.w.mu.Unlock()
	if c.done {
		return
	}
	c.done = true
	c.w.open = false
	switch outcome {
	case OutcomeEnd:
		_ = c.w.writeLine(callEndLine{Type: lineCallEnd, Seq: c.w.seq})
	case OutcomeError:
		_ = c.w.writeLine(callErrorLine{Type: lineCallError, Seq: c.w.seq, Error: errText})
	case OutcomeHang:
		_ = c.w.writeLine(callHangLine{Type: lineCallHang, Seq: c.w.seq, Until: hangUntilCancelled})
	}
}

// RecordingModel wraps a real provider model and records every call
// (REPLAY_TESTING_DESIGN §3.1).
type RecordingModel struct {
	inner domain.Model
	rec   *Recorder
}

func (m *RecordingModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	stream, err := m.inner.Stream(ctx, req)
	cr, recErr := m.rec.beginCall(ctx, req)
	if recErr != nil {
		// A recording failure must not mask or alter the live call's
		// outcome — but it must not pass silently either, or the fixture
		// would diverge from what actually happened.
		if err == nil {
			_ = stream.Close()
		}
		return nil, recErr
	}
	if err != nil {
		cr.terminate(OutcomeError, err.Error())
		return nil, err
	}
	return &recordingStream{inner: stream, ctx: ctx, cr: cr}, nil
}

type recordingStream struct {
	inner domain.ModelStream
	ctx   context.Context
	cr    *callRecorder
}

func (s *recordingStream) Recv() (domain.ModelEvent, error) {
	evt, err := s.inner.Recv()
	switch {
	case err == nil:
		s.cr.event(evt)
	case errors.Is(err, io.EOF):
		s.cr.terminate(OutcomeEnd, "")
	case s.ctx.Err() != nil:
		// The stream died because its context was cancelled: record the
		// hang so replay blocks at exactly this position (§3.2).
		s.cr.terminate(OutcomeHang, "")
	default:
		s.cr.terminate(OutcomeError, err.Error())
	}
	return evt, err
}

func (s *recordingStream) Close() error {
	// A stream closed without a terminal Recv outcome never reached the
	// loop's drain loop — treat it as an error-shaped tail so the
	// recording always carries a terminator.
	s.cr.terminate(OutcomeError, "stream closed before reaching a terminal event")
	return s.inner.Close()
}
