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
// Created: 2026/08/08

package config

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"gopkg.in/yaml.v3"
)

// SecretMask replaces inline secret values in API responses (GET
// /v1/config) so plaintext keys never leave the process over the wire.
// A PUT body carrying the mask means "keep the stored value" — see
// RestoreSecretsFrom. The mask uses characters no real credential
// contains, and any mask still unresolved at save time is a hard error,
// so it can never silently land in the file as an actual key.
const SecretMask = "••••••••••"

// RevisionOf derives the optimistic-locking token for raw config bytes:
// clients echo it back on save, and a mismatch means the file changed
// underneath them (external edit or a concurrent writer).
func RevisionOf(raw []byte) string {
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:16])
}

// ParseFile decodes the YAML schema strictly: unknown keys are typos and
// are rejected, mirroring Load. An empty (or comment-only) document
// yields a zero File. Multi-document input is rejected: a second document
// would be silently dropped by every YAML consumer here, which is a data
// loss trap on the save path.
func ParseFile(raw []byte) (*File, error) {
	var f File
	dec := yaml.NewDecoder(bytes.NewReader(raw))
	dec.KnownFields(true)
	if err := dec.Decode(&f); err != nil && !errors.Is(err, io.EOF) {
		return nil, err
	}
	var extra any
	err := dec.Decode(&extra)
	switch {
	case errors.Is(err, io.EOF):
		// Single document, as expected.
	case err != nil:
		return nil, err
	default:
		return nil, fmt.Errorf("config must be a single YAML document (found content after a --- marker)")
	}
	return &f, nil
}

// DecodeFileJSON decodes a request body into the File schema. JSON is a
// YAML subset, so the same strict decoder serves both the on-disk format
// and the wire format — the API field names ARE the file key names.
func DecodeFileJSON(raw []byte) (*File, error) {
	f, err := ParseFile(raw)
	if err != nil {
		return nil, fmt.Errorf("config body: %w", err)
	}
	return f, nil
}

// ToMap renders f as a plain map for JSON responses. Routing through the
// YAML codec keeps the wire names identical to the file's key names
// (omitempty drops unset fields, so the response reflects exactly what
// the file carries — defaults stay implicit).
func (f *File) ToMap() (map[string]any, error) {
	raw, err := marshalFile(f)
	if err != nil {
		return nil, err
	}
	var m map[string]any
	if err := yaml.Unmarshal(raw, &m); err != nil {
		return nil, fmt.Errorf("render config: %w", err)
	}
	if m == nil {
		m = map[string]any{}
	}
	return m, nil
}

// Resolve validates f exactly as startup with RequireProviders would —
// at least one provider, resolvable secret references, every section
// invariant — and returns the resolved runtime configuration. home is
// the loom home f would be persisted into (the config file always lives
// at <home>/config.yaml), matching what a restart loading that home
// would see. lookup resolves api_key_env/${VAR} references against the
// server process environment — the same environment a restart would
// see. Callers that persist f afterwards use the returned config for
// hot-apply directly, so the applied configuration always matches what
// was validated (no read-back from disk, no write/apply skew).
func (f *File) Resolve(home string, lookup EnvLookup) (*ResolvedConfig, error) {
	if len(f.Providers) == 0 {
		return nil, fmt.Errorf("config: at least one provider is required")
	}
	baseDir, err := filepath.Abs(home)
	if err != nil {
		return nil, fmt.Errorf("config: resolve loom home: %w", err)
	}
	return resolve(f, baseDir, lookup)
}

// MaskSecrets replaces every inline secret with SecretMask. Environment
// references (api_key_env, header ${VAR} placeholders) are not secrets
// and pass through untouched. MCP env values have no reference syntax,
// so every non-empty one is treated as a secret.
func (f *File) MaskSecrets() {
	for i := range f.Providers {
		if f.Providers[i].APIKey != "" {
			f.Providers[i].APIKey = SecretMask
		}
	}
	if f.Tracing.PublicKey != "" {
		f.Tracing.PublicKey = SecretMask
	}
	if f.Tracing.SecretKey != "" {
		f.Tracing.SecretKey = SecretMask
	}
	if f.KnowledgeBase.APIKey != "" {
		f.KnowledgeBase.APIKey = SecretMask
	}
	for name, srv := range f.MCPServers {
		for k, v := range srv.Headers {
			if !headerEnvRef.MatchString(v) {
				srv.Headers[k] = SecretMask
			}
		}
		for k, v := range srv.Env {
			if v != "" {
				srv.Env[k] = SecretMask
			}
		}
		f.MCPServers[name] = srv
	}
}

// RestoreSecretsFrom resolves SecretMask placeholders in f back to the
// stored values from cur, matching structurally (providers by name,
// MCP headers by server+key). A placeholder without a stored counterpart
// — e.g. the provider was renamed in the same edit — is an error: the
// mask must never be written to disk as a literal value.
func (f *File) RestoreSecretsFrom(cur *File) error {
	prevProviders := make(map[string]*Provider, len(cur.Providers))
	for i := range cur.Providers {
		prevProviders[cur.Providers[i].Name] = &cur.Providers[i]
	}
	for i := range f.Providers {
		p := &f.Providers[i]
		if p.APIKey != SecretMask {
			continue
		}
		prev, ok := prevProviders[p.Name]
		if !ok || prev.APIKey == "" {
			return fmt.Errorf("config: providers[%d] (%q): api_key is masked but no stored key exists — please re-enter it", i, p.Name)
		}
		p.APIKey = prev.APIKey
	}
	if err := restoreSecret(&f.Tracing.PublicKey, cur.Tracing.PublicKey, "tracing.public_key"); err != nil {
		return err
	}
	if err := restoreSecret(&f.Tracing.SecretKey, cur.Tracing.SecretKey, "tracing.secret_key"); err != nil {
		return err
	}
	if err := restoreSecret(&f.KnowledgeBase.APIKey, cur.KnowledgeBase.APIKey, "knowledge_base.api_key"); err != nil {
		return err
	}
	for name, srv := range f.MCPServers {
		prev, ok := cur.MCPServers[name]
		for k, v := range srv.Headers {
			if v != SecretMask {
				continue
			}
			if !ok || prev.Headers[k] == "" {
				return fmt.Errorf("config: mcp_servers.%s: header %q is masked but no stored value exists — please re-enter it", name, k)
			}
			srv.Headers[k] = prev.Headers[k]
		}
		for k, v := range srv.Env {
			if v != SecretMask {
				continue
			}
			if !ok || prev.Env[k] == "" {
				return fmt.Errorf("config: mcp_servers.%s: env %q is masked but no stored value exists — please re-enter it", name, k)
			}
			srv.Env[k] = prev.Env[k]
		}
		f.MCPServers[name] = srv
	}
	return nil
}

func restoreSecret(field *string, prev, name string) error {
	if *field != SecretMask {
		return nil
	}
	if prev == "" {
		return fmt.Errorf("config: %s is masked but no stored value exists — please re-enter it", name)
	}
	*field = prev
	return nil
}

// marshalFile renders f as YAML with unset fields omitted (every schema
// tag carries omitempty; pointer fields make explicit zero values
// representable). Indentation matches the template's 2-space style.
func marshalFile(f *File) ([]byte, error) {
	var buf bytes.Buffer
	enc := yaml.NewEncoder(&buf)
	enc.SetIndent(2)
	if err := enc.Encode(f); err != nil {
		return nil, fmt.Errorf("render config: %w", err)
	}
	if err := enc.Close(); err != nil {
		return nil, fmt.Errorf("render config: %w", err)
	}
	return buf.Bytes(), nil
}

// MergeIntoYAML applies f onto the existing document raw, preserving
// comments, key order, and formatting wherever the structure survives:
// mapping values merge recursively, name-keyed sequences (providers,
// models, workspaces) match items by name, and anything else is
// replaced. Keys absent from f are dropped — the UI saves the whole
// configuration, so "unset" means "remove from the file". Empty raw
// yields a fresh document without template comments (the annotated
// template stays the hand-editing starting point written by
// `loom config init`).
func MergeIntoYAML(raw []byte, f *File) ([]byte, error) {
	var src yaml.Node
	srcRaw, err := marshalFile(f)
	if err != nil {
		return nil, err
	}
	if err := yaml.Unmarshal(srcRaw, &src); err != nil {
		return nil, fmt.Errorf("render config: %w", err)
	}
	var dst yaml.Node
	if len(bytes.TrimSpace(raw)) > 0 {
		if err := yaml.Unmarshal(raw, &dst); err != nil {
			return nil, fmt.Errorf("parse existing config: %w", err)
		}
	}
	if len(dst.Content) == 0 {
		return srcRaw, nil
	}
	dst.Content[0] = mergeYAMLNodes(dst.Content[0], src.Content[0])
	var buf bytes.Buffer
	enc := yaml.NewEncoder(&buf)
	enc.SetIndent(2)
	if err := enc.Encode(&dst); err != nil {
		return nil, fmt.Errorf("render config: %w", err)
	}
	if err := enc.Close(); err != nil {
		return nil, fmt.Errorf("render config: %w", err)
	}
	return buf.Bytes(), nil
}

// mergeYAMLNodes merges src into dst, keeping dst's shape (comments,
// key nodes) where both sides agree on the structure and taking src's
// values everywhere else.
func mergeYAMLNodes(dst, src *yaml.Node) *yaml.Node {
	if dst.Kind == yaml.MappingNode && src.Kind == yaml.MappingNode {
		return mergeYAMLMapping(dst, src)
	}
	if dst.Kind == yaml.SequenceNode && src.Kind == yaml.SequenceNode {
		if merged, ok := mergeYAMLSequence(dst, src); ok {
			return merged
		}
	}
	inheritComments(src, dst)
	return src
}

// mergeYAMLMapping merges two mappings: every src key keeps its dst key
// node (and the comments attached to it), values merge recursively;
// src-only keys append; dst-only keys drop. The output follows src's key
// order — the schema's canonical order — so the first save of a
// hand-ordered file reorders sections; comments travel with their keys.
func mergeYAMLMapping(dst, src *yaml.Node) *yaml.Node {
	index := make(map[string]int, len(dst.Content)/2)
	for i := 0; i+1 < len(dst.Content); i += 2 {
		index[dst.Content[i].Value] = i
	}
	content := make([]*yaml.Node, 0, len(src.Content))
	for i := 0; i+1 < len(src.Content); i += 2 {
		sk, sv := src.Content[i], src.Content[i+1]
		if j, ok := index[sk.Value]; ok {
			dk, dv := dst.Content[j], dst.Content[j+1]
			content = append(content, dk, mergeYAMLNodes(dv, sv))
		} else {
			content = append(content, sk, sv)
		}
	}
	dst.Content = content
	return dst
}

// mergeYAMLSequence merges name-keyed mapping sequences (providers,
// models, workspaces): items match by their "name" scalar and merge
// recursively; dst-only names drop, src-only names append. ok=false
// when either side is not uniformly name-keyed — the caller replaces
// the sequence wholesale (scalar lists, empty sides).
func mergeYAMLSequence(dst, src *yaml.Node) (*yaml.Node, bool) {
	if len(src.Content) == 0 || len(dst.Content) == 0 {
		return src, true
	}
	dstByName := make(map[string]*yaml.Node, len(dst.Content))
	for _, item := range dst.Content {
		name, ok := sequenceItemName(item)
		if !ok {
			return nil, false
		}
		dstByName[name] = item
	}
	content := make([]*yaml.Node, 0, len(src.Content))
	for _, si := range src.Content {
		name, ok := sequenceItemName(si)
		if !ok {
			return nil, false
		}
		if di, ok := dstByName[name]; ok {
			content = append(content, mergeYAMLNodes(di, si))
		} else {
			content = append(content, si)
		}
	}
	dst.Content = content
	return dst, true
}

// sequenceItemName extracts the "name" scalar of a mapping sequence
// item, if it has one.
func sequenceItemName(item *yaml.Node) (string, bool) {
	if item.Kind != yaml.MappingNode {
		return "", false
	}
	for i := 0; i+1 < len(item.Content); i += 2 {
		if item.Content[i].Value == "name" && item.Content[i+1].Kind == yaml.ScalarNode {
			return item.Content[i+1].Value, true
		}
	}
	return "", false
}

// inheritComments carries dst's comments onto src when src has none of
// its own, so replacing a value keeps the annotations a user wrote next
// to it.
func inheritComments(src, dst *yaml.Node) {
	if src.HeadComment == "" {
		src.HeadComment = dst.HeadComment
	}
	if src.LineComment == "" {
		src.LineComment = dst.LineComment
	}
	if src.FootComment == "" {
		src.FootComment = dst.FootComment
	}
}

// WriteFileAtomic persists data to path atomically (same-directory temp
// file + rename). A new file gets 0600 — configs routinely carry
// plaintext keys; an existing file keeps its permissions.
func WriteFileAtomic(path string, data []byte) error {
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return fmt.Errorf("create config directory: %w", err)
	}
	mode := os.FileMode(0o600)
	if info, err := os.Stat(path); err == nil {
		mode = info.Mode().Perm()
	}
	tmp, err := os.CreateTemp(dir, ".config-*.tmp")
	if err != nil {
		return fmt.Errorf("create temp config: %w", err)
	}
	tmpName := tmp.Name()
	cleanup := func() { _ = os.Remove(tmpName) }
	if err := tmp.Chmod(mode); err != nil {
		_ = tmp.Close()
		cleanup()
		return fmt.Errorf("chmod temp config: %w", err)
	}
	if _, err := tmp.Write(data); err != nil {
		_ = tmp.Close()
		cleanup()
		return fmt.Errorf("write temp config: %w", err)
	}
	if err := tmp.Close(); err != nil {
		cleanup()
		return fmt.Errorf("close temp config: %w", err)
	}
	if err := os.Rename(tmpName, path); err != nil {
		cleanup()
		return fmt.Errorf("replace config: %w", err)
	}
	return nil
}
