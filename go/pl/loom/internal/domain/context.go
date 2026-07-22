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
// Created: 2026/07/22 21:10

package domain

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
)

const contextManifestIDPrefix = "ctxm_"

// ContextRuleRef records a rule source included in a model request.
type ContextRuleRef struct {
	Source string `json:"source"`
	Hash   string `json:"hash"`
}

// ContextMessageRange records which transcript message/part range was selected.
type ContextMessageRange struct {
	MessageID MessageID `json:"message_id,omitempty"`
	Sequence  int64     `json:"sequence,omitempty"`
	StartPart int       `json:"start_part,omitempty"`
	EndPart   int       `json:"end_part,omitempty"`
}

// ContextCodeRef records a code excerpt included in a model request.
type ContextCodeRef struct {
	Path        string `json:"path"`
	PathHash    string `json:"path_hash,omitempty"`
	ContentHash string `json:"content_hash,omitempty"`
	StartLine   int    `json:"start_line,omitempty"`
	EndLine     int    `json:"end_line,omitempty"`
}

// ContextArtifactRef records a summarized artifact selected for the request.
type ContextArtifactRef struct {
	ArtifactID     ArtifactID `json:"artifact_id,omitempty"`
	SummaryHash    string     `json:"summary_hash"`
	SummaryVersion string     `json:"summary_version,omitempty"`
}

// ContextSummaryRef records a compacted summary and its lineage.
type ContextSummaryRef struct {
	ID      string   `json:"id"`
	Hash    string   `json:"hash"`
	Lineage []string `json:"lineage,omitempty"`
}

// TokenizerRef identifies the tokenizer used for context budgeting.
type TokenizerRef struct {
	Name    string `json:"name"`
	Version string `json:"version,omitempty"`
}

// BudgetBucket records one logical share of the prompt budget.
type BudgetBucket struct {
	Name   string `json:"name"`
	Tokens int64  `json:"tokens,omitempty"`
}

// ContextTruncation records a clipping decision made while building context.
type ContextTruncation struct {
	Target string `json:"target,omitempty"`
	Reason string `json:"reason"`
}

// ContextManifest is a lightweight, reproducible description of a model request.
type ContextManifest struct {
	ID            string                `json:"id"`
	Hash          string                `json:"hash"`
	Rules         []ContextRuleRef      `json:"rules,omitempty"`
	MessageRanges []ContextMessageRange `json:"message_ranges,omitempty"`
	CodeRefs      []ContextCodeRef      `json:"code_refs,omitempty"`
	ArtifactRefs  []ContextArtifactRef  `json:"artifact_refs,omitempty"`
	SummaryRefs   []ContextSummaryRef   `json:"summary_refs,omitempty"`
	Tokenizer     TokenizerRef          `json:"tokenizer"`
	BudgetBuckets []BudgetBucket        `json:"budget_buckets,omitempty"`
	Truncations   []ContextTruncation   `json:"truncations,omitempty"`
	PromptHash    string                `json:"prompt_hash"`
}

type contextManifestBody struct {
	Rules         []ContextRuleRef      `json:"rules,omitempty"`
	MessageRanges []ContextMessageRange `json:"message_ranges,omitempty"`
	CodeRefs      []ContextCodeRef      `json:"code_refs,omitempty"`
	ArtifactRefs  []ContextArtifactRef  `json:"artifact_refs,omitempty"`
	SummaryRefs   []ContextSummaryRef   `json:"summary_refs,omitempty"`
	Tokenizer     TokenizerRef          `json:"tokenizer"`
	BudgetBuckets []BudgetBucket        `json:"budget_buckets,omitempty"`
	Truncations   []ContextTruncation   `json:"truncations,omitempty"`
	PromptHash    string                `json:"prompt_hash"`
}

// Canonicalize derives the stable manifest hash and identifier.
func (m ContextManifest) Canonicalize() (ContextManifest, error) {
	bodyJSON, body, err := m.canonicalBodyJSON()
	if err != nil {
		return ContextManifest{}, err
	}

	hash := sha256.Sum256(bodyJSON)
	canonical := ContextManifest{
		ID:            contextManifestIDPrefix + hex.EncodeToString(hash[:12]),
		Hash:          hex.EncodeToString(hash[:]),
		Rules:         body.Rules,
		MessageRanges: body.MessageRanges,
		CodeRefs:      body.CodeRefs,
		ArtifactRefs:  body.ArtifactRefs,
		SummaryRefs:   body.SummaryRefs,
		Tokenizer:     body.Tokenizer,
		BudgetBuckets: body.BudgetBuckets,
		Truncations:   body.Truncations,
		PromptHash:    body.PromptHash,
	}
	return canonical, nil
}

// NewContextManifest canonicalizes a draft manifest and derives stable ID/hash.
func NewContextManifest(draft ContextManifest) (ContextManifest, error) {
	return draft.Canonicalize()
}

// CanonicalJSON renders the manifest with deterministic JSON.
func (m ContextManifest) CanonicalJSON() ([]byte, error) {
	canonical, err := m.Canonicalize()
	if err != nil {
		return nil, err
	}
	return json.Marshal(canonical)
}

// Validate checks the manifest is well-formed and consistent with its derived hash.
func (m ContextManifest) Validate() error {
	if m.ID == "" {
		return fmt.Errorf("context manifest id required")
	}
	if m.Hash == "" {
		return fmt.Errorf("context manifest hash required")
	}
	canonical, err := m.Canonicalize()
	if err != nil {
		return err
	}
	if m.ID != canonical.ID {
		return fmt.Errorf("context manifest id mismatch")
	}
	if m.Hash != canonical.Hash {
		return fmt.Errorf("context manifest hash mismatch")
	}
	return nil
}

func (m ContextManifest) canonicalBodyJSON() ([]byte, contextManifestBody, error) {
	body := contextManifestBody{
		Rules:         append([]ContextRuleRef(nil), m.Rules...),
		MessageRanges: append([]ContextMessageRange(nil), m.MessageRanges...),
		CodeRefs:      append([]ContextCodeRef(nil), m.CodeRefs...),
		ArtifactRefs:  append([]ContextArtifactRef(nil), m.ArtifactRefs...),
		SummaryRefs:   append([]ContextSummaryRef(nil), m.SummaryRefs...),
		Tokenizer:     m.Tokenizer,
		BudgetBuckets: append([]BudgetBucket(nil), m.BudgetBuckets...),
		Truncations:   append([]ContextTruncation(nil), m.Truncations...),
		PromptHash:    m.PromptHash,
	}
	if err := validateContextManifestBody(body); err != nil {
		return nil, contextManifestBody{}, err
	}
	data, err := json.Marshal(body)
	if err != nil {
		return nil, contextManifestBody{}, err
	}
	return data, body, nil
}

func validateContextManifestBody(body contextManifestBody) error {
	for i, rule := range body.Rules {
		if rule.Source == "" {
			return fmt.Errorf("rules[%d]: source required", i)
		}
		if rule.Hash == "" {
			return fmt.Errorf("rules[%d]: hash required", i)
		}
	}
	for i, rng := range body.MessageRanges {
		if rng.MessageID.IsZero() && rng.Sequence < 0 {
			return fmt.Errorf("message_ranges[%d]: sequence must be non-negative", i)
		}
		if rng.StartPart < 0 {
			return fmt.Errorf("message_ranges[%d]: start_part must be non-negative", i)
		}
		if rng.EndPart < 0 {
			return fmt.Errorf("message_ranges[%d]: end_part must be non-negative", i)
		}
		if rng.EndPart != 0 && rng.EndPart < rng.StartPart {
			return fmt.Errorf("message_ranges[%d]: end_part must be >= start_part", i)
		}
	}
	for i, ref := range body.CodeRefs {
		if ref.Path == "" {
			return fmt.Errorf("code_refs[%d]: path required", i)
		}
		if ref.StartLine < 0 {
			return fmt.Errorf("code_refs[%d]: start_line must be non-negative", i)
		}
		if ref.EndLine < 0 {
			return fmt.Errorf("code_refs[%d]: end_line must be non-negative", i)
		}
		if ref.EndLine != 0 && ref.EndLine < ref.StartLine {
			return fmt.Errorf("code_refs[%d]: end_line must be >= start_line", i)
		}
	}
	for i, ref := range body.ArtifactRefs {
		if ref.ArtifactID.IsZero() && ref.SummaryHash == "" {
			return fmt.Errorf("artifact_refs[%d]: artifact_id or summary_hash required", i)
		}
	}
	for i, ref := range body.SummaryRefs {
		if ref.ID == "" {
			return fmt.Errorf("summary_refs[%d]: id required", i)
		}
		if ref.Hash == "" {
			return fmt.Errorf("summary_refs[%d]: hash required", i)
		}
		for j, parent := range ref.Lineage {
			if parent == "" {
				return fmt.Errorf("summary_refs[%d]: lineage[%d] required", i, j)
			}
		}
	}
	if body.Tokenizer.Name == "" {
		return fmt.Errorf("tokenizer name required")
	}
	for i, bucket := range body.BudgetBuckets {
		if bucket.Name == "" {
			return fmt.Errorf("budget_buckets[%d]: name required", i)
		}
		if bucket.Tokens < 0 {
			return fmt.Errorf("budget_buckets[%d]: tokens must be non-negative", i)
		}
	}
	for i, truncation := range body.Truncations {
		if truncation.Reason == "" {
			return fmt.Errorf("truncations[%d]: reason required", i)
		}
	}
	if body.PromptHash == "" {
		return fmt.Errorf("prompt hash required")
	}
	return nil
}
