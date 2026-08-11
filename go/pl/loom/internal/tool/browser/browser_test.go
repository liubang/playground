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
// Created: 2026/08/12

package browser

import (
	"context"
	"encoding/json"
	"fmt"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/stretchr/testify/assert"
)

// --- helpers ---

func newToolCall(t *testing.T, name string, args any) domain.ToolCall {
	t.Helper()
	raw, err := json.Marshal(args)
	if err != nil {
		t.Fatalf("marshal args: %v", err)
	}
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: name, Arguments: raw}
}

// newTestManager creates a Manager without probing for Chrome by passing
// a dummy chromePath. The Manager is usable for Prepare/verify tests since
// those don't touch the browser instance — only Execute does.
func newTestManager(t *testing.T) *Manager {
	t.Helper()
	// Use /bin/true as a stand-in: NewManager only checks the path exists,
	// it doesn't verify it's Chrome until Acquire creates a context.
	m, err := NewManager("/bin/true", 0, 0, 0)
	if err != nil {
		t.Fatalf("NewManager() error = %v", err)
	}
	t.Cleanup(m.Close)
	return m
}

// --- validateBrowserArgs tests ---

func TestValidateBrowserArgs_Navigate(t *testing.T) {
	tests := []struct {
		name    string
		args    browserArgs
		wantErr bool
		errCode domain.ErrorCode
	}{
		{
			name:    "valid navigate",
			args:    browserArgs{Action: "navigate", URL: "https://example.com"},
			wantErr: false,
		},
		{
			name:    "navigate missing url",
			args:    browserArgs{Action: "navigate"},
			wantErr: true,
			errCode: domain.ErrInvalidInput,
		},
		{
			name:    "navigate empty url",
			args:    browserArgs{Action: "navigate", URL: ""},
			wantErr: true,
			errCode: domain.ErrInvalidInput,
		},
		{
			name:    "navigate whitespace url",
			args:    browserArgs{Action: "navigate", URL: "   "},
			wantErr: true,
			errCode: domain.ErrInvalidInput,
		},
		{
			name:    "navigate invalid scheme",
			args:    browserArgs{Action: "navigate", URL: "ftp://example.com"},
			wantErr: true,
			errCode: domain.ErrInvalidInput,
		},
		{
			name:    "navigate file scheme",
			args:    browserArgs{Action: "navigate", URL: "file:///etc/passwd"},
			wantErr: true,
			errCode: domain.ErrInvalidInput,
		},
		{
			name:    "navigate missing host",
			args:    browserArgs{Action: "navigate", URL: "https:///path"},
			wantErr: true,
			errCode: domain.ErrInvalidInput,
		},
		{
			name:    "navigate http scheme valid",
			args:    browserArgs{Action: "navigate", URL: "http://localhost:8080"},
			wantErr: false,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := validateBrowserArgs(tt.args)
			if tt.wantErr {
				assert.Error(t, err)
				var agentErr *domain.AgentError
				if assert.ErrorAs(t, err, &agentErr) {
					assert.Equal(t, tt.errCode, agentErr.Code)
				}
			} else {
				assert.NoError(t, err)
			}
		})
	}
}

func TestValidateBrowserArgs_Actions(t *testing.T) {
	tests := []struct {
		name    string
		args    browserArgs
		wantErr bool
	}{
		{"screenshot valid", browserArgs{Action: "screenshot"}, false},
		{"snapshot valid", browserArgs{Action: "snapshot"}, false},
		{"scroll valid", browserArgs{Action: "scroll"}, false},
		{"scroll with coords", browserArgs{Action: "scroll", ScrollX: 100, ScrollY: 200}, false},
		{"scroll with selector", browserArgs{Action: "scroll", Selector: "#content"}, false},
		{"close valid", browserArgs{Action: "close"}, false},
		{"unknown action", browserArgs{Action: "foo"}, true},
		{"click without ref", browserArgs{Action: "click"}, true},
		{"click with ref", browserArgs{Action: "click", Ref: "[1]"}, false},
		{"type without ref", browserArgs{Action: "type", Text: "hello"}, true},
		{"type without text", browserArgs{Action: "type", Ref: "[1]"}, true},
		{"type valid", browserArgs{Action: "type", Ref: "[1]", Text: "hello"}, false},
		{"empty action", browserArgs{Action: ""}, true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := validateBrowserArgs(tt.args)
			if tt.wantErr {
				assert.Error(t, err)
			} else {
				assert.NoError(t, err)
			}
		})
	}
}

func TestValidateBrowserArgs_Format(t *testing.T) {
	tests := []struct {
		name    string
		format  string
		wantErr bool
	}{
		{"png valid", "png", false},
		{"jpeg valid", "jpeg", false},
		{"empty defaults", "", false},
		{"webp invalid", "webp", true},
		{"gif invalid", "gif", true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := validateBrowserArgs(browserArgs{Action: "screenshot", Format: tt.format})
			if tt.wantErr {
				assert.Error(t, err)
			} else {
				assert.NoError(t, err)
			}
		})
	}
}

func TestValidateBrowserArgs_Timeout(t *testing.T) {
	tests := []struct {
		name      string
		timeoutMs int
		wantErr   bool
	}{
		{"zero ok (default)", 0, false},
		{"min boundary", 5000, false},
		{"below min", 4999, true},
		{"max boundary", 120000, false},
		{"above max", 120001, true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := validateBrowserArgs(browserArgs{Action: "screenshot", TimeoutMs: tt.timeoutMs})
			if tt.wantErr {
				assert.Error(t, err)
			} else {
				assert.NoError(t, err)
			}
		})
	}
}

func TestValidateBrowserArgs_Quality(t *testing.T) {
	tests := []struct {
		name    string
		quality int
		wantErr bool
	}{
		{"zero ok (default)", 0, false},
		{"min boundary", 10, false},
		{"below min", 9, true},
		{"max boundary", 100, false},
		{"above max", 101, true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := validateBrowserArgs(browserArgs{Action: "screenshot", Quality: tt.quality})
			if tt.wantErr {
				assert.Error(t, err)
			} else {
				assert.NoError(t, err)
			}
		})
	}
}

// --- extractURLRequest tests ---

func TestExtractURLRequest(t *testing.T) {
	tests := []struct {
		name    string
		rawURL  string
		want    *domain.URLRequest
		wantNil bool
	}{
		{"https", "https://example.com", &domain.URLRequest{Host: "example.com"}, false},
		{"http with port", "http://localhost:8080", &domain.URLRequest{Host: "localhost"}, false},
		{"uppercase host", "https://Example.COM/Path", &domain.URLRequest{Host: "example.com"}, false},
		{"ftp scheme", "ftp://example.com", nil, true},
		{"file scheme", "file:///path", nil, true},
		{"missing host", "https:///path", nil, true},
		{"invalid url", "://not-a-url", nil, true},
		{"empty string", "", nil, true},
		{"with whitespace", "  https://example.com  ", &domain.URLRequest{Host: "example.com"}, false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := extractURLRequest(tt.rawURL)
			if tt.wantNil {
				assert.Nil(t, got)
			} else {
				assert.Equal(t, tt.want, got)
			}
		})
	}
}

// --- NewBrowserTool tests ---

func TestNewBrowserTool_NilManager(t *testing.T) {
	_, err := NewBrowserTool(nil, nil, 0, 0)
	assert.Error(t, err)
	var agentErr *domain.AgentError
	if assert.ErrorAs(t, err, &agentErr) {
		assert.Equal(t, domain.ErrInvalidInput, agentErr.Code)
	}
}

func TestNewBrowserTool_Defaults(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)
	assert.Equal(t, defaultNavTimeoutMs*time.Millisecond, tool.navTimeout)
	assert.Equal(t, defaultScreenshotQuality, tool.screenshotQual)
}

func TestNewBrowserTool_CustomValues(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 60*time.Second, 90)
	assert.NoError(t, err)
	assert.Equal(t, 60*time.Second, tool.navTimeout)
	assert.Equal(t, 90, tool.screenshotQual)
}

// --- Prepare / verify tests ---

func TestBrowserTool_Prepare_Navigate(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "navigate", URL: "https://example.com"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)
	assert.Contains(t, prepared.ApprovalDesc, "navigate")
	assert.Contains(t, prepared.ApprovalDesc, "https://example.com")
	assert.NotNil(t, prepared.URLRequest)
	assert.Equal(t, "example.com", prepared.URLRequest.Host)
	assert.NotEmpty(t, prepared.ArgsHash)
}

func TestBrowserTool_Prepare_Screenshot_NoURL(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "screenshot"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)
	assert.Nil(t, prepared.URLRequest)
}

func TestBrowserTool_Prepare_InvalidAction(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "invalid"})
	_, err = tool.Prepare(context.Background(), call)
	assert.Error(t, err)
}

func TestBrowserTool_Prepare_NameMismatch(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "wrong_name", browserArgs{Action: "screenshot"})
	_, err = tool.Prepare(context.Background(), call)
	assert.Error(t, err)
}

func TestBrowserTool_VerifyPreparedCall(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "navigate", URL: "https://example.com"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)

	// Verify should pass for a properly prepared call.
	err = tool.base.verifyPreparedCall(prepared)
	assert.NoError(t, err)

	// Tamper with the args hash — verification must fail.
	prepared.ArgsHash = "tampered"
	err = tool.base.verifyPreparedCall(prepared)
	assert.Error(t, err)
}

func TestBrowserTool_Definition(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	def := tool.Definition()
	assert.Equal(t, "browser", def.Name)
	assert.Equal(t, domain.ToolSourceBuiltin, def.Source)
	// No static capabilities: risk is graded per action in Prepare (a
	// static network.connect would floor the tier at R3, and the loop's
	// drift guard rejects prepared risks below the definition default).
	assert.Empty(t, def.Capabilities)
}

// TestBrowserTool_Prepare_RiskTiers pins the per-action risk grading
// (docs/BROWSER_DESIGN.md §5.2): read/shape actions on the approved page
// are R2 (no approval prompt in any mode); navigate/type are R3.
func TestBrowserTool_Prepare_RiskTiers(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	cases := []struct {
		args     browserArgs
		wantRisk domain.RiskLevel
	}{
		{browserArgs{Action: "navigate", URL: "https://example.com"}, domain.R3},
		{browserArgs{Action: "type", Ref: "1", Text: "hi"}, domain.R3},
		{browserArgs{Action: "snapshot"}, domain.R2},
		{browserArgs{Action: "screenshot"}, domain.R2},
		{browserArgs{Action: "scroll", ScrollY: 400}, domain.R2},
		{browserArgs{Action: "click", Ref: "1"}, domain.R2},
		{browserArgs{Action: "close"}, domain.R2},
	}
	for _, tc := range cases {
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, "browser", tc.args))
		assert.NoError(t, err, tc.args.Action)
		assert.Equal(t, tc.wantRisk, prepared.Risk, tc.args.Action)
		// The per-action risk must round-trip through verification.
		assert.NoError(t, tool.base.verifyPreparedCall(prepared), tc.args.Action)
	}

	// A prepared call whose risk was tampered with must fail verification.
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "browser", browserArgs{Action: "snapshot"}))
	assert.NoError(t, err)
	prepared.Risk = domain.R3
	assert.Error(t, tool.base.verifyPreparedCall(prepared))
}

func TestBrowserTool_ConcurrentSafe(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	// Browser tool is NOT concurrent-safe: operations are serialized
	// through the Manager mutex.
	assert.False(t, tool.ConcurrentSafe())
}

// --- mapBrowserError tests ---

type errFake string

func (e errFake) Error() string { return string(e) }

func TestMapBrowserError(t *testing.T) {
	tests := []struct {
		name     string
		err      error
		wantCode domain.ErrorCode
	}{
		{"nil", nil, ""},
		{"timeout", context.DeadlineExceeded, domain.ErrTimeout},
		{"cancelled", context.Canceled, domain.ErrCancelled},
		{"connection refused", errFake("connection refused"), domain.ErrUnavailable},
		{"no such host", errFake("no such host"), domain.ErrUnavailable},
		{"navigation error", errFake("navigation failed"), domain.ErrUnavailable},
		{"generic", errFake("something else"), domain.ErrInternal},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := mapBrowserError(tt.err)
			if tt.err == nil {
				assert.NoError(t, got)
				return
			}
			var agentErr *domain.AgentError
			if assert.ErrorAs(t, got, &agentErr) {
				assert.Equal(t, tt.wantCode, agentErr.Code)
			}
		})
	}
}

// --- decodeStrict tests ---

func TestDecodeStrict(t *testing.T) {
	t.Run("valid json", func(t *testing.T) {
		raw := json.RawMessage(`{"action":"navigate","url":"https://example.com"}`)
		args, err := decodeStrict[browserArgs](raw)
		assert.NoError(t, err)
		assert.Equal(t, "navigate", args.Action)
		assert.Equal(t, "https://example.com", args.URL)
	})

	t.Run("unknown field", func(t *testing.T) {
		raw := json.RawMessage(`{"action":"navigate","unknown":"foo"}`)
		_, err := decodeStrict[browserArgs](raw)
		assert.Error(t, err)
	})

	t.Run("trailing data", func(t *testing.T) {
		raw := json.RawMessage(`{"action":"navigate"}{"extra":"data"}`)
		_, err := decodeStrict[browserArgs](raw)
		assert.Error(t, err)
	})

	t.Run("invalid json", func(t *testing.T) {
		raw := json.RawMessage(`{not json}`)
		_, err := decodeStrict[browserArgs](raw)
		assert.Error(t, err)
	})
}

// --- successResult / errorResult tests ---

func TestSuccessResult(t *testing.T) {
	callID := domain.NewToolCallID()
	result := successResult(callID, time.Now(), browserOutput{
		Action: "navigate",
		Status: "ok",
	})
	assert.Equal(t, callID, result.CallID)
	assert.Equal(t, domain.ToolStatusSuccess, result.Status)
	assert.Len(t, result.Content, 1)
	assert.Equal(t, domain.PartText, result.Content[0].Kind)
}

func TestErrorResult(t *testing.T) {
	callID := domain.NewToolCallID()
	err := domain.NewError(domain.ErrInvalidInput, "test error")
	result := errorResult(callID, time.Now(), err)
	assert.Equal(t, callID, result.CallID)
	assert.Equal(t, domain.ToolStatusError, result.Status)
	assert.NotNil(t, result.Error)
	assert.Equal(t, "invalid_input", result.Error.Code)
}

func TestErrorResult_Timeout(t *testing.T) {
	callID := domain.NewToolCallID()
	result := errorResult(callID, time.Now(), context.DeadlineExceeded)
	assert.Equal(t, domain.ToolStatusTimeout, result.Status)
	assert.Equal(t, "timeout", result.Error.Code)
}

func TestErrorResult_Cancelled(t *testing.T) {
	callID := domain.NewToolCallID()
	result := errorResult(callID, time.Now(), context.Canceled)
	assert.Equal(t, domain.ToolStatusCancelled, result.Status)
	assert.Equal(t, "cancelled", result.Error.Code)
}

// --- Manager lifecycle tests ---

func TestManager_Close(t *testing.T) {
	mgr := newTestManager(t)
	mgr.Close()
	// Double close should not panic.
	mgr.Close()
}

func TestManager_AcquireAfterClose(t *testing.T) {
	mgr := newTestManager(t)
	mgr.Close()
	_, err := mgr.Acquire()
	assert.Error(t, err)
	var agentErr *domain.AgentError
	if assert.ErrorAs(t, err, &agentErr) {
		assert.Equal(t, domain.ErrUnavailable, agentErr.Code)
	}
}

func TestManager_TouchAfterClose(t *testing.T) {
	mgr := newTestManager(t)
	mgr.Close()
	// Touch after close should not panic.
	mgr.Touch()
}

func TestManager_CloseInstanceAfterClose(t *testing.T) {
	mgr := newTestManager(t)
	mgr.Close()
	// CloseInstance after close should not panic.
	mgr.CloseInstance()
}

func TestManager_String(t *testing.T) {
	mgr := newTestManager(t)
	s := mgr.String()
	assert.Contains(t, s, "browser.Manager")
	assert.Contains(t, s, "idleTTL")
}

func TestFindChrome(t *testing.T) {
	// findChrome should return a non-empty string on most systems with
	// Chrome installed, or empty when none found. We only test that it
	// doesn't panic.
	result := findChrome()
	_ = result
}

// --- Execute tests (actions that don't require a live browser) ---

func TestBrowserTool_Execute_Close(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "close"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)

	result := tool.Execute(context.Background(), prepared)
	assert.Equal(t, domain.ToolStatusSuccess, result.Status)
	assert.Len(t, result.Content, 1)
	assert.Equal(t, domain.PartText, result.Content[0].Kind)
}

func TestBrowserTool_Execute_TamperedHash(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "close"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)

	// Tamper with the args hash — Execute must reject it.
	prepared.ArgsHash = "tampered"
	result := tool.Execute(context.Background(), prepared)
	assert.Equal(t, domain.ToolStatusError, result.Status)
	assert.NotNil(t, result.Error)
	assert.Equal(t, "security", result.Error.Code)
}

func TestBrowserTool_Execute_InvalidArgs(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	// Build a prepared call with invalid JSON arguments.
	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "browser",
		Arguments: json.RawMessage(`{not valid json}`),
	}
	prepared := domain.PreparedCall{
		Call:       call,
		Definition: tool.Definition(),
		Risk:       tool.Definition().Risk(),
		// ArgsHash will be wrong, but decodeStrict fails first in Execute.
	}
	// Bypass Prepare: manually set a hash so verifyPreparedCall passes,
	// then Execute's decodeStrict will fail.
	// Actually, verifyPreparedCall will fail because ArgsHash is empty.
	result := tool.Execute(context.Background(), prepared)
	assert.Equal(t, domain.ToolStatusError, result.Status)
}

// --- storeScreenshot tests ---

// mockArtifactStore is a minimal ArtifactStore for testing overflow.
type mockArtifactStore struct {
	data   []byte
	failOn bool // when true, Begin returns an error
}

type mockStagedArtifact struct {
	store      *mockArtifactStore
	abort      bool
	totalBytes int64
}

func (s *mockArtifactStore) Begin(ctx context.Context) (domain.StagedArtifact, error) {
	if s.failOn {
		return nil, fmt.Errorf("artifact store unavailable")
	}
	return &mockStagedArtifact{store: s}, nil
}

func (s *mockArtifactStore) Read(ctx context.Context, ref domain.ArtifactRef) ([]byte, error) {
	return s.data, nil
}

func (a *mockStagedArtifact) Write(p []byte) (int, error) {
	a.store.data = append(a.store.data, p...)
	a.totalBytes += int64(len(p))
	return len(p), nil
}

func (a *mockStagedArtifact) TotalBytes() int64 { return a.totalBytes }

func (a *mockStagedArtifact) StoredBytes() int64 { return int64(len(a.store.data)) }

func (a *mockStagedArtifact) Truncated() bool { return false }

func (a *mockStagedArtifact) Abort() error { a.abort = true; return nil }

func (a *mockStagedArtifact) Commit(ctx context.Context) (domain.ArtifactRef, error) {
	return domain.ArtifactRef{ID: domain.NewArtifactID(), Size: int64(len(a.store.data))}, nil
}

func TestStoreScreenshot_NilArtifacts(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	// When artifacts store is nil, storeScreenshot reports no artifact.
	_, ok := tool.storeScreenshot(context.Background(), []byte("png-bytes"), "image/png")
	assert.False(t, ok)
}

func TestStoreScreenshot_WithStore(t *testing.T) {
	mgr := newTestManager(t)
	store := &mockArtifactStore{}
	tool, err := NewBrowserTool(mgr, store, 0, 0)
	assert.NoError(t, err)

	// Raw bytes are persisted (not base64 text) and the media type is set
	// so the UI can render the image.
	raw := []byte("\x89PNG\r\n\x1a\n fake png bytes")
	ref, ok := tool.storeScreenshot(context.Background(), raw, "image/png")
	assert.True(t, ok)
	assert.NotZero(t, ref.ID)
	assert.Equal(t, int64(len(raw)), ref.Size)
	assert.Equal(t, "image/png", ref.MediaType)
	assert.Equal(t, raw, store.data)
}

func TestStoreScreenshot_StoreFailure(t *testing.T) {
	mgr := newTestManager(t)
	store := &mockArtifactStore{failOn: true}
	tool, err := NewBrowserTool(mgr, store, 0, 0)
	assert.NoError(t, err)

	_, ok := tool.storeScreenshot(context.Background(), []byte("data"), "image/png")
	assert.False(t, ok)
}

// --- refRegistry tests ---

func TestRefRegistry_NewIsEmpty(t *testing.T) {
	r := newRefRegistry()
	assert.Nil(t, r.lookup("[1]"))
}

func TestRefRegistry_Invalidate(t *testing.T) {
	r := newRefRegistry()
	r.replace(map[string]*axNode{
		"[1]": {backendDOMID: 42, role: "button", name: "Submit"},
	})

	got := r.lookup("[1]")
	assert.NotNil(t, got)
	assert.Equal(t, "button", got.role)
	assert.Equal(t, "Submit", got.name)

	r.invalidate()
	assert.Nil(t, r.lookup("[1]"))
}

func TestRefRegistry_Replace(t *testing.T) {
	r := newRefRegistry()
	r.replace(map[string]*axNode{
		"[1]": {backendDOMID: 1, role: "button"},
	})
	assert.NotNil(t, r.lookup("[1]"))

	// A new snapshot replaces the old refs entirely.
	r.replace(map[string]*axNode{
		"[1]": {backendDOMID: 99, role: "link"},
	})
	got := r.lookup("[1]")
	assert.NotNil(t, got)
	assert.Equal(t, "link", got.role)

	// Replace with nil clears.
	r.replace(nil)
	assert.Nil(t, r.lookup("[1]"))
}

func TestAXNode_String(t *testing.T) {
	assert.Equal(t, `button "Submit"`, (&axNode{role: "button", name: "Submit"}).String())
	assert.Equal(t, "link", (&axNode{role: "link"}).String())
}

// --- snapshot serialization tests ---

func TestSerializeAXTree_Empty(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	output := tool.serializeAXTree(nil)
	assert.Contains(t, output, "empty")
}

func TestRuneCount(t *testing.T) {
	assert.Equal(t, 5, runeCount("hello"))
	assert.Equal(t, 7, runeCount("héllo!!")) // é is one rune
}

func TestTruncateRunes(t *testing.T) {
	assert.Equal(t, "hel", truncateRunes("hello", 3))
	assert.Equal(t, "hello", truncateRunes("hello", 10))
}

func TestIsInteractive(t *testing.T) {
	assert.True(t, isInteractive("button"))
	assert.True(t, isInteractive("link"))
	assert.True(t, isInteractive("textBox"))
	assert.True(t, isInteractive("checkBox"))
	assert.False(t, isInteractive("heading"))
	assert.False(t, isInteractive("paragraph"))
	assert.False(t, isInteractive("generic"))
}

func TestIsSkippable(t *testing.T) {
	assert.True(t, isSkippable("generic", ""))
	assert.True(t, isSkippable("none", ""))
	assert.False(t, isSkippable("generic", "has name"))
	assert.False(t, isSkippable("button", ""))
}

// --- Execute snapshot/click/type tests (Prepare level) ---

func TestBrowserTool_Prepare_Snapshot(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "snapshot"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)
	assert.Contains(t, prepared.ApprovalDesc, "snapshot")
	assert.Nil(t, prepared.URLRequest) // snapshot doesn't carry URL
}

func TestBrowserTool_Prepare_Click(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "click", Ref: "[1]"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)
	assert.Contains(t, prepared.ApprovalDesc, "click")
}

func TestBrowserTool_Prepare_Click_NoRef(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "click"})
	_, err = tool.Prepare(context.Background(), call)
	assert.Error(t, err)
}

func TestBrowserTool_Prepare_Type(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "type", Ref: "[1]", Text: "hello"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)
	assert.Contains(t, prepared.ApprovalDesc, "type")
}

func TestBrowserTool_Prepare_Type_NoText(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	call := newToolCall(t, "browser", browserArgs{Action: "type", Ref: "[1]"})
	_, err = tool.Prepare(context.Background(), call)
	assert.Error(t, err)
}

func TestBrowserTool_Execute_ClickStaleRef(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	// No snapshot taken yet — ref is stale.
	call := newToolCall(t, "browser", browserArgs{Action: "click", Ref: "[1]"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)

	result := tool.Execute(context.Background(), prepared)
	assert.Equal(t, domain.ToolStatusError, result.Status)
	assert.NotNil(t, result.Error)
	assert.Equal(t, "invalid_input", result.Error.Code)
	assert.Contains(t, result.Error.Message, "stale")
}

func TestBrowserTool_Execute_TypeStaleRef(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	// No snapshot taken yet — ref is stale.
	call := newToolCall(t, "browser", browserArgs{Action: "type", Ref: "[1]", Text: "hello"})
	prepared, err := tool.Prepare(context.Background(), call)
	assert.NoError(t, err)

	result := tool.Execute(context.Background(), prepared)
	assert.Equal(t, domain.ToolStatusError, result.Status)
	assert.NotNil(t, result.Error)
	assert.Contains(t, result.Error.Message, "stale")
}

// --- Definition update test ---

func TestBrowserTool_Definition_IncludesSnapshotClickType(t *testing.T) {
	mgr := newTestManager(t)
	tool, err := NewBrowserTool(mgr, nil, 0, 0)
	assert.NoError(t, err)

	def := tool.Definition()
	// wait_ready was removed: the schema must not advertise it.
	assert.NotContains(t, string(tool.Definition().InputSchema), "wait_ready")
	assert.Contains(t, def.Description, "snapshot")
	assert.Contains(t, def.Description, "click")
	assert.Contains(t, def.Description, "type")
}
