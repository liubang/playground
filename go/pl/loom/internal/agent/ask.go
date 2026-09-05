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
// Created: 2026/07/27

package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

type askUserArgsOption struct {
	Label       string `json:"label"`
	Description string `json:"description"`
}

type askUserArgs struct {
	Question      string              `json:"question"`
	Options       []askUserArgsOption `json:"options"`
	AllowMultiple bool                `json:"allow_multiple"`
}

// AskUserTool lets the model ask the user a multiple-choice question when
// it is uncertain between reasonable directions, instead of guessing. The
// tool blocks on the injected Questioner; the answer (or a skip) comes back
// as the tool result.
type AskUserTool struct {
	def        domain.ToolDefinition
	questioner domain.Questioner
}

// NewAskUserTool creates the tool bound to the given questioner.
func NewAskUserTool(questioner domain.Questioner) (*AskUserTool, error) {
	if questioner == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "questioner is required")
	}
	def := domain.ToolDefinition{
		Name: "ask_user",
		Description: "Ask the user a multiple-choice question when you are uncertain between several reasonable " +
			"directions (requirements, scope, trade-offs) and guessing wrong would be costly. " +
			"Rules: ask sparingly — at most when the answer genuinely changes what you will do; prefer 2-4 concrete, " +
			"mutually distinct options with a one-line description each; never ask about things you can determine " +
			"yourself by reading code or running commands. " +
			"The user may pick from your options, answer in free text, or skip the question entirely — a skip means " +
			"'proceed with your best judgment', never treat it as an error or ask the same question again.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"question":{"type":"string","minLength":1,"maxLength":1024},"options":{"type":"array","minItems":1,"maxItems":8,"items":{"type":"object","additionalProperties":false,"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":512}},"required":["label"]}},"allow_multiple":{"type":"boolean"}},"required":["question","options"]}`),
		Capabilities: []domain.Capability{domain.CapUserInteract},
		Source:       domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	return &AskUserTool{def: def, questioner: questioner}, nil
}

// Definition returns the tool definition.
func (t *AskUserTool) Definition() domain.ToolDefinition { return t.def }

// Prepare validates and canonicalizes the call; it is side-effect-free.
func (t *AskUserTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	question, canonical, err := decodeAskUserArgs(call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	call.Arguments = canonical
	return domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         domain.R0,
		ApprovalDesc: fmt.Sprintf("Ask user: %s", toolkit.Ellipsize(question.Text, 60)),
		ArgsHash:     toolkit.ArgsFingerprint(canonical),
	}, nil
}

// Execute asks the question and blocks for the answer.
func (t *AskUserTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := domain.RealClock{}.Now()
	question, _, err := decodeAskUserArgs(prepared.Call.Arguments)
	if err != nil {
		return toolErrorResult(prepared.Call.ID, startedAt, err)
	}
	question.ID = domain.NewEventID()

	answer, err := t.questioner.Ask(ctx, question)
	if err != nil {
		return toolErrorResult(prepared.Call.ID, startedAt, err)
	}

	payload := map[string]any{"answered": !answer.Skipped}
	switch {
	case answer.Skipped:
		payload["note"] = "the user skipped this question (or no interactive frontend is attached); proceed with your best judgment and do not re-ask"
	default:
		payload["note"] = "the user answered"
		if len(answer.Selected) > 0 {
			payload["selected"] = answer.Selected
		}
		if answer.CustomText != "" {
			payload["custom_text"] = answer.CustomText
		}
	}
	raw, err := json.Marshal(payload)
	if err != nil {
		return toolErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode result", domain.WithCause(err)))
	}
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: domain.RealClock{}.Now(),
	}
}

// decodeAskUserArgs parses, normalizes, and validates the call arguments
// into a domain question; unknown fields are rejected. The question ID is
// assigned by the caller at ask time.
func decodeAskUserArgs(raw json.RawMessage) (domain.Question, json.RawMessage, error) {
	var args askUserArgs
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&args); err != nil {
		return domain.Question{}, nil, domain.NewError(domain.ErrInvalidInput, "invalid ask_user arguments", domain.WithCause(err))
	}
	question := domain.Question{
		Text:          args.Question,
		AllowMultiple: args.AllowMultiple,
	}
	for _, opt := range args.Options {
		question.Options = append(question.Options, domain.QuestionOption{
			Label:       opt.Label,
			Description: opt.Description,
		})
	}
	if err := question.Validate(); err != nil {
		return domain.Question{}, nil, domain.NewError(domain.ErrInvalidInput, err.Error())
	}
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.Question{}, nil, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	return question, canonical, nil
}
