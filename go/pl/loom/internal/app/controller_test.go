// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

package app

import (
	"context"
	"path/filepath"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

func TestControllerContinuesSessionForFollowUpPrompt(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first answer", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "follow-up answer", StopReason: domain.StopEndTurn},
	)
	controller := NewController(ControllerConfig{
		Bootstrap: &Bootstrap{
			Config:    BootstrapConfig{Limits: domain.DefaultLimits()},
			Store:     store,
			Model:     model,
			ModelName: "test-model",
			Registry:  agent.NewToolRegistry(),
		},
		Broker:   runtimeevent.NewBroker(),
		Approver: NewChannelApprover(),
		Clock:    domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if err := controller.SubmitPrompt(ctx, "first question"); err != nil {
		t.Fatalf("SubmitPrompt(first): %v", err)
	}
	waitForIdle(t, controller)

	if err := controller.SubmitPrompt(ctx, "follow-up question"); err != nil {
		t.Fatalf("SubmitPrompt(follow-up): %v", err)
	}
	waitForIdle(t, controller)

	calls := model.Calls()
	if len(calls) != 2 {
		t.Fatalf("model calls = %d, want 2", len(calls))
	}
	if got := messageText(calls[1].Messages[0]); got != "first question" {
		t.Fatalf("first message in follow-up context = %q, want first question", got)
	}
	if got := messageText(calls[1].Messages[2]); got != "follow-up question" {
		t.Fatalf("follow-up message in context = %q, want follow-up question", got)
	}

	snapshot, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if len(snapshot.Messages) != 4 {
		t.Fatalf("message count = %d, want 4", len(snapshot.Messages))
	}
}

func waitForIdle(t *testing.T, controller *Controller) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		if controller.State() == ControllerStateIdle {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("controller state = %q, want idle", controller.State())
}

func messageText(message domain.Message) string {
	for _, part := range message.Parts {
		if part.Kind == domain.PartText {
			return part.Text
		}
	}
	return ""
}

func TestToolCallTargetPrefersWritePaths(t *testing.T) {
	if got := toolCallTarget(toolCallAuditDTO{WritePaths: []string{"w.go"}, ReadPaths: []string{"r.go"}}); got != "w.go" {
		t.Fatalf("target = %q, want write path", got)
	}
	if got := toolCallTarget(toolCallAuditDTO{ReadPaths: []string{"r.go"}}); got != "r.go" {
		t.Fatalf("target = %q, want read path", got)
	}
	if got := toolCallTarget(toolCallAuditDTO{ApprovalDesc: "run: go test ./..."}); got != "run: go test ./..." {
		t.Fatalf("target = %q, want approval description", got)
	}
	if got := toolCallTarget(toolCallAuditDTO{}); got != "" {
		t.Fatalf("target = %q, want empty", got)
	}
}

func TestToolResultPreviewExtractsAndBounds(t *testing.T) {
	callID := domain.NewToolCallID()
	msg := domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{
			Kind: domain.PartToolResult,
			ToolResult: &domain.ToolResult{
				CallID:  callID,
				Status:  domain.ToolStatusSuccess,
				Content: []domain.ContentPart{{Kind: domain.PartText, Text: "line1\nline2"}},
			},
		}},
	}
	gotID, preview := toolResultPreview(msg)
	if gotID != callID {
		t.Fatalf("call ID = %v, want %v", gotID, callID)
	}
	if preview != "line1\nline2" {
		t.Fatalf("preview = %q, want joined text", preview)
	}

	errMsg := domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{
			Kind: domain.PartToolResult,
			ToolResult: &domain.ToolResult{
				CallID: callID,
				Status: domain.ToolStatusError,
				Error:  &domain.ToolError{Code: "x", Message: "boom"},
			},
		}},
	}
	if _, preview := toolResultPreview(errMsg); preview != "boom" {
		t.Fatalf("error preview = %q, want error message", preview)
	}
}

func TestBoundPreviewLinesTruncates(t *testing.T) {
	if got := boundPreviewLines("a\nb\nc\nd", 2, 100); got != "a\nb\n…" {
		t.Fatalf("line-bounded preview = %q", got)
	}
	if got := boundPreviewLines("abcdefgh", 10, 3); got != "abc\n…" {
		t.Fatalf("byte-bounded preview = %q", got)
	}
	if got := boundPreviewLines("  \n", 10, 10); got != "" {
		t.Fatalf("blank preview = %q, want empty", got)
	}
}
