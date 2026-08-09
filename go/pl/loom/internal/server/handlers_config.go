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

package server

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"os"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
)

// The config endpoints expose the on-disk YAML configuration (the single
// configuration source) for UI editing. Reads mask secrets; writes are
// validated exactly as startup would, merged into the existing document
// comment-preservingly, guarded by an optimistic-locking revision, and
// hot-applied to the runtime — the response's applied report classifies
// every changed section by when it takes effect.

// configView is the GET /v1/config response model.
type configView struct {
	Path     string         `json:"path"`
	Exists   bool           `json:"exists"`
	Revision string         `json:"revision"`
	Config   map[string]any `json:"config"`
}

// configUpdate is the PUT /v1/config request model: the full replacement
// configuration plus the revision the client based its edit on.
type configUpdate struct {
	Revision string          `json:"revision"`
	Config   json.RawMessage `json:"config"`
}

var errConfigUnavailable = &statusError{
	status:  http.StatusServiceUnavailable,
	code:    "config_unavailable",
	message: "this server was started without a config file path",
}

// handleGetConfig serves the current configuration with secrets masked.
func (s *Server) handleGetConfig(w http.ResponseWriter, _ *http.Request) {
	if s.cfg.ConfigPath == "" {
		writeError(w, errConfigUnavailable)
		return
	}
	view := configView{Path: s.cfg.ConfigPath, Config: map[string]any{}}
	raw, err := os.ReadFile(s.cfg.ConfigPath)
	switch {
	case errors.Is(err, os.ErrNotExist):
		// Fresh install: the UI renders an empty form against the schema.
	case err != nil:
		writeError(w, err)
		return
	default:
		f, err := config.ParseFile(raw)
		if err != nil {
			writeError(w, &statusError{
				status:  http.StatusInternalServerError,
				code:    "config_invalid",
				message: "config file cannot be parsed (fix it by hand): " + err.Error(),
			})
			return
		}
		f.MaskSecrets()
		m, err := f.ToMap()
		if err != nil {
			writeError(w, err)
			return
		}
		view.Exists = true
		view.Revision = config.RevisionOf(raw)
		view.Config = m
	}
	writeJSON(w, http.StatusOK, view)
}

// secretReveal is the POST /v1/config/reveal request model: it names one
// stored secret by its structural location. The GET response only carries
// SecretMask placeholders; this endpoint serves the plaintext on demand
// (e.g. the settings UI's reveal button) so it never has to ride the
// whole-config response.
type secretReveal struct {
	Kind  string `json:"kind"`  // provider | tracing | mcp_header
	Name  string `json:"name"`  // provider name / MCP server name
	Field string `json:"field"` // tracing: public_key|secret_key; mcp_header: header name
}

var errSecretNotFound = &statusError{
	status:  http.StatusNotFound,
	code:    "secret_not_found",
	message: "no stored secret at this location",
}

// handleRevealSecret serves one stored secret's plaintext. The file is
// read fresh from disk (it is the single source of truth) and nothing is
// cached, so a reveal always reflects what a save would preserve.
func (s *Server) handleRevealSecret(w http.ResponseWriter, r *http.Request) {
	if s.cfg.ConfigPath == "" {
		writeError(w, errConfigUnavailable)
		return
	}
	var req secretReveal
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	raw, err := os.ReadFile(s.cfg.ConfigPath)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			writeError(w, errSecretNotFound)
			return
		}
		writeError(w, err)
		return
	}
	f, err := config.ParseFile(raw)
	if err != nil {
		writeError(w, &statusError{
			status:  http.StatusInternalServerError,
			code:    "config_invalid",
			message: "config file cannot be parsed (fix it by hand): " + err.Error(),
		})
		return
	}
	value, err := lookupSecret(f, req)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"value": value})
}

// lookupSecret extracts the plaintext addressed by req from the parsed
// configuration.
func lookupSecret(f *config.File, req secretReveal) (string, error) {
	var value string
	switch req.Kind {
	case "provider":
		for i := range f.Providers {
			if f.Providers[i].Name == req.Name {
				value = f.Providers[i].APIKey
				break
			}
		}
	case "tracing":
		switch req.Field {
		case "public_key":
			value = f.Tracing.PublicKey
		case "secret_key":
			value = f.Tracing.SecretKey
		default:
			return "", invalidInput("tracing field must be public_key or secret_key")
		}
	case "mcp_header":
		if srv, ok := f.MCPServers[req.Name]; ok {
			value = srv.Headers[req.Field]
		}
	default:
		return "", invalidInput("kind must be provider, tracing, or mcp_header")
	}
	if value == "" {
		return "", errSecretNotFound
	}
	return value, nil
}

// handlePutConfig validates and saves a replacement configuration.
func (s *Server) handlePutConfig(w http.ResponseWriter, r *http.Request) {
	if s.cfg.ConfigPath == "" {
		writeError(w, errConfigUnavailable)
		return
	}
	var upd configUpdate
	if err := decodeBody(w, r, &upd); err != nil {
		writeError(w, err)
		return
	}
	if len(upd.Config) == 0 {
		writeError(w, invalidInput("config is required"))
		return
	}
	incoming, err := config.DecodeFileJSON(upd.Config)
	if err != nil {
		writeError(w, invalidInput(err.Error()))
		return
	}

	merged, report, err := s.writeAndApplyConfig(r, incoming, upd.Revision)
	if err != nil {
		writeError(w, err)
		return
	}
	s.logger.Info("config updated via API", "path", s.cfg.ConfigPath)
	writeJSON(w, http.StatusOK, map[string]any{
		"path":     s.cfg.ConfigPath,
		"revision": config.RevisionOf(merged),
		"applied":  report,
	})
}

// writeAndApplyConfig runs the serialized read-modify-write-apply cycle:
// optimistic revision check, secret restoration against the stored file,
// startup validation (which yields the resolved runtime config),
// comment-preserving merge, atomic write, then hot-apply of the resolved
// config — all under configMu, so writes and applies can never interleave
// out of order. The hot-apply uses the in-memory resolved config (the
// exact one that was validated and written), never a disk re-read; its
// context outlives the request so a client disconnect cannot abort MCP
// reconnects halfway.
func (s *Server) writeAndApplyConfig(r *http.Request, incoming *config.File, revision string) ([]byte, app.ConfigApplyReport, error) {
	s.configMu.Lock()
	defer s.configMu.Unlock()

	raw, err := os.ReadFile(s.cfg.ConfigPath)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, app.ConfigApplyReport{}, err
	}
	// A missing file has the empty revision, mirroring GET.
	curRev := ""
	if raw != nil {
		curRev = config.RevisionOf(raw)
	}
	if curRev != revision {
		return nil, app.ConfigApplyReport{}, &statusError{
			status:  http.StatusConflict,
			code:    "config_conflict",
			message: "the config file changed on disk; reload it and re-apply your edits",
		}
	}
	cur := &config.File{}
	if raw != nil {
		var perr error
		cur, perr = config.ParseFile(raw)
		if perr != nil {
			return nil, app.ConfigApplyReport{}, &statusError{
				status:  http.StatusInternalServerError,
				code:    "config_invalid",
				message: "config file cannot be parsed (fix it by hand): " + perr.Error(),
			}
		}
	}
	if err := incoming.RestoreSecretsFrom(cur); err != nil {
		return nil, app.ConfigApplyReport{}, invalidInput(err.Error())
	}
	resolved, err := incoming.Resolve(s.cfg.ConfigPath, os.LookupEnv)
	if err != nil {
		return nil, app.ConfigApplyReport{}, invalidInput(err.Error())
	}
	merged, err := config.MergeIntoYAML(raw, incoming)
	if err != nil {
		return nil, app.ConfigApplyReport{}, err
	}
	if err := config.WriteFileAtomic(s.cfg.ConfigPath, merged); err != nil {
		return nil, app.ConfigApplyReport{}, err
	}
	report := s.svc.ApplyConfig(context.WithoutCancel(r.Context()), resolved)
	return merged, report, nil
}
