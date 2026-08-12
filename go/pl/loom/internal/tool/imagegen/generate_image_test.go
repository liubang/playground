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
// Created: 2026/08/06

package imagegen

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/media"
	"github.com/liubang/playground/go/pl/loom/internal/model/images"
)

var testPNG = []byte{
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
	0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
}

// fakeGenerator records the request and returns a canned result.
type fakeGenerator struct {
	gotModel   string
	gotPrompt  string
	gotSize    string
	gotQuality string
	result     images.GenerateResult
	err        error
}

func (f *fakeGenerator) Generate(_ context.Context, req images.GenerateRequest) (images.GenerateResult, error) {
	f.gotModel, f.gotPrompt, f.gotSize, f.gotQuality = req.Model, req.Prompt, req.Size, req.Quality
	return f.result, f.err
}

func newTestTool(t *testing.T, gen images.Generator, opts Options) *GenerateImageTool {
	t.Helper()
	store, err := artifact.Open(t.TempDir(), 8<<20)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	tool, err := NewGenerateImageTool(gen, store, opts)
	if err != nil {
		t.Fatalf("NewGenerateImageTool: %v", err)
	}
	return tool
}

func prepareCall(t *testing.T, tool *GenerateImageTool, args string) domain.PreparedCall {
	t.Helper()
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "generate_image",
		Arguments: json.RawMessage(args),
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	return prepared
}

func TestNewGenerateImageToolValidation(t *testing.T) {
	t.Parallel()
	gen := &fakeGenerator{}
	store, err := artifact.Open(t.TempDir(), 8<<20)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	if _, err := NewGenerateImageTool(nil, store, Options{Model: "m"}); err == nil {
		t.Fatal("expected error for nil generator")
	}
	if _, err := NewGenerateImageTool(gen, store, Options{}); err == nil {
		t.Fatal("expected error for empty model")
	}
	if _, err := NewGenerateImageTool(gen, store, Options{Model: "m", Size: "800x600"}); err == nil {
		t.Fatal("expected error for invalid default size")
	}
	if _, err := NewGenerateImageTool(gen, store, Options{Model: "m", Quality: "ultra"}); err == nil {
		t.Fatal("expected error for invalid default quality")
	}
	if _, err := NewGenerateImageTool(gen, store, Options{Model: "m", Size: "auto", Quality: "high"}); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if _, err := NewGenerateImageTool(gen, nil, Options{Model: "m"}); err == nil {
		t.Fatal("expected error for nil store")
	}
}

func TestPrepareValidatesArgs(t *testing.T) {
	t.Parallel()
	tool := newTestTool(t, &fakeGenerator{}, Options{Model: "gpt-image-2"})
	cases := []struct {
		name string
		args string
	}{
		{"empty prompt", `{"prompt":""}`},
		{"blank prompt", `{"prompt":"   "}`},
		{"oversized prompt", `{"prompt":"` + strings.Repeat("a", maxPromptRunes+1) + `"}`},
		{"invalid size", `{"prompt":"p","size":"800x600"}`},
		{"invalid quality", `{"prompt":"p","quality":"ultra"}`},
		{"unknown field", `{"prompt":"p","model":"gpt-image-9"}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := tool.Prepare(context.Background(), domain.ToolCall{
				ID:        domain.NewToolCallID(),
				Name:      "generate_image",
				Arguments: json.RawMessage(tc.args),
			})
			if err == nil {
				t.Fatal("expected Prepare to reject the arguments")
			}
		})
	}
}

func TestPrepareApprovalDescContainsPrompt(t *testing.T) {
	t.Parallel()
	tool := newTestTool(t, &fakeGenerator{}, Options{Model: "gpt-image-2"})
	prepared := prepareCall(t, tool, `{"prompt":"a red fox in a field"}`)
	if !strings.Contains(prepared.ApprovalDesc, "gpt-image-2") || !strings.Contains(prepared.ApprovalDesc, "a red fox") {
		t.Fatalf("unexpected approval description: %q", prepared.ApprovalDesc)
	}
	if prepared.Risk != domain.R3 {
		t.Fatalf("network capability should map to R3, got %v", prepared.Risk)
	}
}

func TestExecuteSuccessStoresArtifact(t *testing.T) {
	t.Parallel()
	gen := &fakeGenerator{result: images.GenerateResult{
		Data:          testPNG,
		MediaType:     "image/png",
		RevisedPrompt: "revised",
	}}
	tool := newTestTool(t, gen, Options{Model: "gpt-image-2", Size: "1024x1024"})

	prepared := prepareCall(t, tool, `{"prompt":"  a red fox  ","quality":"high"}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %s, err = %+v", result.Status, result.Error)
	}

	// The pinned model/default size come from Options; the prompt is
	// trimmed; the explicit quality override wins.
	if gen.gotModel != "gpt-image-2" || gen.gotPrompt != "a red fox" ||
		gen.gotSize != "1024x1024" || gen.gotQuality != "high" {
		t.Fatalf("unexpected generator request: %+v", gen)
	}

	var textPart, artifactPart *domain.ContentPart
	for i := range result.Content {
		switch result.Content[i].Kind {
		case domain.PartText:
			textPart = &result.Content[i]
		case domain.PartArtifact:
			artifactPart = &result.Content[i]
		}
	}
	if textPart == nil || artifactPart == nil {
		t.Fatalf("expected text+artifact parts, got %+v", result.Content)
	}

	var out generateImageOutput
	if err := json.Unmarshal([]byte(textPart.Text), &out); err != nil {
		t.Fatalf("output header is not JSON: %v", err)
	}
	if out.MediaType != "image/png" || out.Bytes != len(testPNG) || out.RevisedPrompt != "revised" {
		t.Fatalf("unexpected output header: %+v", out)
	}
	if out.Artifact == nil || out.Artifact.ID != artifactPart.Artifact.ID {
		t.Fatal("header artifact does not match the artifact part")
	}
	if artifactPart.Artifact.Size != int64(len(testPNG)) {
		t.Fatalf("artifact size = %d", artifactPart.Artifact.Size)
	}
	// The declared media type lets renderers dispatch without fetching the
	// blob (and keeps text artifacts from being rendered as images).
	if artifactPart.Artifact.MediaType != "image/png" {
		t.Fatalf("artifact media type = %q, want image/png", artifactPart.Artifact.MediaType)
	}
}

func TestExecuteWithoutArtifactStore(t *testing.T) {
	t.Parallel()
	// generate_image now requires an artifact store at construction; a
	// nil store is rejected by NewGenerateImageTool, so there is no
	// "without store" execute path to test anymore.
}

func TestExecuteGeneratorError(t *testing.T) {
	t.Parallel()
	gen := &fakeGenerator{err: domain.NewError(domain.ErrRateLimited, "slow down", domain.WithRetryable(true))}
	tool := newTestTool(t, gen, Options{Model: "gpt-image-2"})

	result := tool.Execute(context.Background(), prepareCall(t, tool, `{"prompt":"p"}`))
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s", result.Status)
	}
	if result.Error == nil || result.Error.Code != string(domain.ErrRateLimited) || !result.Error.Retryable {
		t.Fatalf("unexpected tool error: %+v", result.Error)
	}
}

func TestExecuteLargeImageSkipsInline(t *testing.T) {
	t.Parallel()
	// generate_image no longer inlines anything — images live only as
	// artifacts, and oversized blobs are rejected by media.StoreImage
	// (MaxImageBytes) before they reach the store. The previous "inline
	// cap" concept is gone, so this test is retained as a guard that an
	// oversized image is rejected, not silently dropped.
	big := make([]byte, media.MaxImageBytes+1)
	gen := &fakeGenerator{result: images.GenerateResult{Data: big, MediaType: "image/png"}}
	tool := newTestTool(t, gen, Options{Model: "gpt-image-2"})

	result := tool.Execute(context.Background(), prepareCall(t, tool, `{"prompt":"p"}`))
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s, want error for oversized image", result.Status)
	}
	if result.Error == nil || result.Error.Code != string(domain.ErrInvalidInput) {
		t.Fatalf("result = %+v, want invalid input", result)
	}
}

func TestExecuteRejectsTamperedPreparedCall(t *testing.T) {
	t.Parallel()
	gen := &fakeGenerator{result: images.GenerateResult{Data: testPNG, MediaType: "image/png"}}
	tool := newTestTool(t, gen, Options{Model: "gpt-image-2"})

	prepared := prepareCall(t, tool, `{"prompt":"p"}`)
	prepared.Call.Arguments = json.RawMessage(`{"prompt":"tampered"}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s", result.Status)
	}
	if result.Error == nil || result.Error.Code != string(domain.ErrSecurity) {
		t.Fatalf("expected security error, got %+v", result.Error)
	}
	if gen.gotPrompt == "tampered" {
		t.Fatal("generator must not run for a tampered call")
	}
}
