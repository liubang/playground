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
// Created: 2026/08/22

package server

import (
	"context"
	"errors"
	"io"
	"io/fs"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// Workspace explorer: the right-hand panel of the WebUI (file tree + git
// changes). All paths are workspace-relative on the wire and confined to the
// registered workspace root after symlink resolution — the explorer can never
// escape the root the user registered. Reads only; there is deliberately no
// write/commit surface here.

// maxWorkspaceEntries caps one directory listing; maxWorkspaceFileBytes caps
// inline file previews; maxGitDiffBytes caps a served unified diff.
const (
	maxWorkspaceEntries   = 1000
	maxWorkspaceFileBytes = 256 << 10
	maxGitDiffBytes       = 512 << 10
	gitCmdTimeout         = 10 * time.Second
)

// workspaceExplorerRoot resolves the canonical (symlink-evaluated) root of
// the workspace addressed by the {id} path parameter.
func (s *Server) workspaceExplorerRoot(ctx context.Context, r *http.Request) (domain.WorkspaceID, string, error) {
	id, err := parseWorkspaceIDParam(r.PathValue("id"))
	if err != nil {
		return domain.WorkspaceID{}, "", err
	}
	ws, err := s.svc.GetWorkspace(ctx, id)
	if err != nil {
		return domain.WorkspaceID{}, "", err
	}
	root, err := filepath.EvalSymlinks(ws.RootPath)
	if err != nil {
		return domain.WorkspaceID{}, "", invalidInput("workspace root unavailable")
	}
	return id, root, nil
}

// confineWorkspacePath maps a workspace-relative wire path to an absolute
// path guaranteed to sit under root. Symlinks are resolved and the result is
// re-checked: a link pointing outside the workspace is rejected.
func confineWorkspacePath(root, rel string) (string, error) {
	rel = strings.TrimSpace(rel)
	if rel == "" || rel == "." {
		return root, nil
	}
	if filepath.IsAbs(rel) {
		return "", invalidInput("path must be workspace-relative")
	}
	cleaned := filepath.Clean(rel)
	if cleaned == ".." || strings.HasPrefix(cleaned, ".."+string(filepath.Separator)) {
		return "", invalidInput("path escapes the workspace")
	}
	resolved, err := filepath.EvalSymlinks(filepath.Join(root, cleaned))
	if err != nil {
		return "", invalidInput("path does not exist")
	}
	if resolved != root && !strings.HasPrefix(resolved, root+string(filepath.Separator)) {
		return "", invalidInput("path escapes the workspace")
	}
	return resolved, nil
}

// --- directory listing (file tree) ---

// wsFileEntry is one tree node; Path is workspace-relative ("" for the root).
type wsFileEntry struct {
	Name    string `json:"name"`
	Path    string `json:"path"`
	Kind    string `json:"kind"` // dir | file
	Size    int64  `json:"size,omitempty"`
	ModTime string `json:"mod_time,omitempty"`
}

// handleListWorkspaceFiles serves GET /v1/workspaces/{id}/files?path=<rel>&all=1.
// Hidden (dot-prefixed) entries are skipped unless all=1; .git is always
// skipped. Directories sort before files, each by name.
func (s *Server) handleListWorkspaceFiles(w http.ResponseWriter, r *http.Request) {
	_, root, err := s.workspaceExplorerRoot(r.Context(), r)
	if err != nil {
		writeError(w, err)
		return
	}
	rel := strings.TrimSpace(r.URL.Query().Get("path"))
	abs, err := confineWorkspacePath(root, rel)
	if err != nil {
		writeError(w, err)
		return
	}
	info, err := os.Stat(abs)
	if err != nil || !info.IsDir() {
		writeError(w, invalidInput("path is not a directory"))
		return
	}
	showAll := r.URL.Query().Get("all") == "1"
	dirEntries, err := os.ReadDir(abs)
	if err != nil {
		writeError(w, invalidInput("cannot read directory"))
		return
	}
	entries := make([]wsFileEntry, 0, len(dirEntries))
	truncated := false
	for _, e := range dirEntries {
		name := e.Name()
		if name == ".git" || (!showAll && hiddenDir.MatchString(name)) {
			continue
		}
		if len(entries) >= maxWorkspaceEntries {
			truncated = true
			break
		}
		entry := wsFileEntry{
			Name: name,
			Path: name,
			Kind: "file",
		}
		if rel != "" {
			entry.Path = rel + "/" + name
		}
		fi, ierr := e.Info()
		if e.IsDir() {
			entry.Kind = "dir"
		} else if e.Type()&os.ModeSymlink != 0 {
			// ReadDir is lstat: follow symlinks to classify link→dir.
			if target, terr := os.Stat(filepath.Join(abs, name)); terr == nil && target.IsDir() {
				entry.Kind = "dir"
			}
		}
		if ierr == nil {
			entry.Size = fi.Size()
			entry.ModTime = fi.ModTime().UTC().Format(time.RFC3339)
		}
		entries = append(entries, entry)
	}
	sort.Slice(entries, func(i, j int) bool {
		if (entries[i].Kind == "dir") != (entries[j].Kind == "dir") {
			return entries[i].Kind == "dir"
		}
		return entries[i].Name < entries[j].Name
	})
	writeJSON(w, http.StatusOK, map[string]any{
		"path":      rel,
		"entries":   entries,
		"truncated": truncated,
	})
}

// --- file preview ---

// handleReadWorkspaceFile serves GET /v1/workspaces/{id}/file?path=<rel>.
// Text content is returned inline (capped at maxWorkspaceFileBytes); binary
// files are reported with binary=true and no content.
func (s *Server) handleReadWorkspaceFile(w http.ResponseWriter, r *http.Request) {
	_, root, err := s.workspaceExplorerRoot(r.Context(), r)
	if err != nil {
		writeError(w, err)
		return
	}
	rel := strings.TrimSpace(r.URL.Query().Get("path"))
	if rel == "" {
		writeError(w, invalidInput("path is required"))
		return
	}
	abs, err := confineWorkspacePath(root, rel)
	if err != nil {
		writeError(w, err)
		return
	}
	info, err := os.Stat(abs)
	if err != nil {
		writeError(w, invalidInput("path does not exist"))
		return
	}
	if info.IsDir() {
		writeError(w, invalidInput("path is a directory"))
		return
	}
	f, err := os.Open(abs)
	if err != nil {
		writeError(w, invalidInput("cannot read file"))
		return
	}
	defer f.Close()
	buf, err := io.ReadAll(io.LimitReader(f, maxWorkspaceFileBytes))
	if err != nil {
		writeError(w, invalidInput("cannot read file"))
		return
	}
	truncated := info.Size() > maxWorkspaceFileBytes
	binary := looksBinary(buf)
	resp := map[string]any{
		"path":      rel,
		"size":      info.Size(),
		"truncated": truncated,
		"binary":    binary,
	}
	if !binary {
		resp["content"] = string(buf)
	}
	writeJSON(w, http.StatusOK, resp)
}

// looksBinary sniffs the content sample: a NUL byte or a non-textual
// detected content type marks the file as binary.
func looksBinary(sample []byte) bool {
	if len(sample) == 0 {
		return false
	}
	head := sample
	if len(head) > 512 {
		head = head[:512]
	}
	if strings.IndexByte(string(head), 0) >= 0 {
		return true
	}
	ct := http.DetectContentType(head)
	return !strings.HasPrefix(ct, "text/") &&
		!strings.HasPrefix(ct, "application/json") &&
		!strings.HasPrefix(ct, "application/xml") &&
		!strings.HasPrefix(ct, "image/svg")
}

// --- git status / diff (changes tab) ---

// gitFileEntry is one changed file; paths are workspace-relative. Status is a
// single letter: M/A/D/R/T (tracked) or U (untracked). Adds/Dels merge the
// staged and unstaged numstats; NoStat marks binary or untracked files.
type gitFileEntry struct {
	Path     string `json:"path"`
	Status   string `json:"status"`
	Staged   bool   `json:"staged"`
	Unstaged bool   `json:"unstaged"`
	Adds     int    `json:"adds"`
	Dels     int    `json:"dels"`
	NoStat   bool   `json:"no_stat"`
}

// runGit executes git inside dir with a hard timeout and returns stdout.
func runGit(ctx context.Context, dir string, args ...string) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, gitCmdTimeout)
	defer cancel()
	full := append([]string{"-C", dir}, args...)
	cmd := exec.CommandContext(ctx, "git", full...)
	var stdout, stderr strings.Builder
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return stdout.String(), &gitError{err: err, stderr: strings.TrimSpace(stderr.String())}
	}
	return stdout.String(), nil
}

// gitError carries stderr for diagnostics; exit-code-1 from diff --no-index
// is handled by callers via ExitCode.
type gitError struct {
	err    error
	stderr string
}

func (e *gitError) Error() string {
	if e.stderr != "" {
		return e.stderr
	}
	return e.err.Error()
}

func (e *gitError) Unwrap() error { return e.err }

// gitRepoContext locates the enclosing repo of the workspace root and the
// repo-relative prefix of the root ("" when the workspace is the repo root).
// Git talks in repo-relative paths; the wire talks in workspace-relative
// paths — the prefix is the translation, and changes outside the workspace
// subtree are filtered out.
func gitRepoContext(ctx context.Context, root string) (toplevel, prefix string, err error) {
	out, err := runGit(ctx, root, "rev-parse", "--is-inside-work-tree")
	if err != nil || strings.TrimSpace(out) != "true" {
		return "", "", errNotGitRepo
	}
	out, err = runGit(ctx, root, "rev-parse", "--show-toplevel")
	if err != nil {
		return "", "", err
	}
	toplevel, err = filepath.EvalSymlinks(strings.TrimSpace(out))
	if err != nil {
		return "", "", err
	}
	if toplevel == root {
		return toplevel, "", nil
	}
	rel, rerr := filepath.Rel(toplevel, root)
	if rerr != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		// Not a subtree (worktree/symlink oddity): fall back to repo scope.
		return toplevel, "", nil
	}
	return toplevel, filepath.ToSlash(rel) + "/", nil
}

// errNotGitRepo signals the workspace root is not inside a git work tree;
// the status handler answers is_git=false instead of an error.
var errNotGitRepo = errors.New("not a git repository")

// stripRepoPrefix converts a repo-relative git path to workspace-relative;
// ok=false means the path lies outside the workspace subtree.
func stripRepoPrefix(prefix, repoRel string) (string, bool) {
	if prefix == "" {
		return repoRel, true
	}
	if strings.HasPrefix(repoRel, prefix) {
		return repoRel[len(prefix):], true
	}
	return "", false
}

// parsePorcelainZ parses `git status --porcelain=v1 -z` output into entries,
// consuming the second path field of rename/copy records.
func parsePorcelainZ(raw string) [][2]string {
	fields := strings.Split(raw, "\x00")
	out := make([][2]string, 0, len(fields)/2)
	for i := 0; i < len(fields); i++ {
		rec := fields[i]
		if len(rec) < 4 {
			continue
		}
		xy, p := rec[:2], rec[3:]
		if strings.ContainsAny(xy, "RC") {
			i++ // rename/copy: the following field carries the old path
		}
		out = append(out, [2]string{xy, p})
	}
	return out
}

type lineStat struct {
	adds, dels int
	binary     bool
}

// parseNumstatZ parses `git diff --numstat -z` output keyed by (repo-relative)
// path. Binary files carry "-" counts. Rename records have an empty path
// field followed by old and new paths; the new path is the key.
func parseNumstatZ(raw string) map[string]lineStat {
	stats := map[string]lineStat{}
	fields := strings.Split(raw, "\x00")
	for i := 0; i < len(fields); i++ {
		rec := fields[i]
		if rec == "" {
			continue
		}
		parts := strings.SplitN(rec, "\t", 3)
		if len(parts) < 3 {
			continue
		}
		st := lineStat{}
		if parts[0] == "-" || parts[1] == "-" {
			st.binary = true
		} else {
			st.adds, _ = strconv.Atoi(parts[0])
			st.dels, _ = strconv.Atoi(parts[1])
		}
		p := parts[2]
		if p == "" && i+2 < len(fields) {
			// rename: "\0old\0new"
			p = fields[i+2]
			i += 2
		}
		if p != "" {
			stats[p] = st
		}
	}
	return stats
}

// handleWorkspaceGitStatus serves GET /v1/workspaces/{id}/git/status. The
// response carries is_git=false (not an error) for non-repo workspaces so the
// frontend can hide the changes tab.
func (s *Server) handleWorkspaceGitStatus(w http.ResponseWriter, r *http.Request) {
	_, root, err := s.workspaceExplorerRoot(r.Context(), r)
	if err != nil {
		writeError(w, err)
		return
	}
	ctx := r.Context()
	toplevel, prefix, err := gitRepoContext(ctx, root)
	if err != nil {
		writeJSON(w, http.StatusOK, map[string]any{"is_git": false})
		return
	}
	branch, _ := runGit(ctx, toplevel, "rev-parse", "--abbrev-ref", "HEAD")
	branch = strings.TrimSpace(branch)

	raw, err := runGit(ctx, toplevel, "-c", "status.relativePaths=false",
		"status", "--porcelain=v1", "-z", "--untracked-files=normal")
	if err != nil {
		writeError(w, invalidInput("git status failed: "+err.Error()))
		return
	}
	unstagedStats, _ := runGit(ctx, toplevel, "diff", "--numstat", "-z")
	stagedStats, _ := runGit(ctx, toplevel, "diff", "--cached", "--numstat", "-z")
	unstaged := parseNumstatZ(unstagedStats)
	staged := parseNumstatZ(stagedStats)

	files := make([]gitFileEntry, 0)
	totalAdds, totalDels := 0, 0
	for _, rec := range parsePorcelainZ(raw) {
		xy, repoRel := rec[0], rec[1]
		wsRel, ok := stripRepoPrefix(prefix, repoRel)
		if !ok {
			continue // change outside the workspace subtree
		}
		entry := gitFileEntry{Path: wsRel}
		switch {
		case xy == "??":
			entry.Status = "U"
			entry.Unstaged = true
			entry.NoStat = true
		default:
			x, y := xy[0], xy[1]
			entry.Staged = x != ' ' && x != '!'
			entry.Unstaged = y != ' ' && y != '!'
			letter := x
			if letter == ' ' || letter == '!' {
				letter = y
			}
			entry.Status = string(letter)
			u := unstaged[repoRel]
			st := staged[repoRel]
			entry.Adds = u.adds + st.adds
			entry.Dels = u.dels + st.dels
			entry.NoStat = u.binary || st.binary
		}
		totalAdds += entry.Adds
		totalDels += entry.Dels
		files = append(files, entry)
	}
	sort.Slice(files, func(i, j int) bool { return files[i].Path < files[j].Path })
	writeJSON(w, http.StatusOK, map[string]any{
		"is_git": true,
		"branch": branch,
		"files":  files,
		"adds":   totalAdds,
		"dels":   totalDels,
	})
}

// confineWorkspacePathLoose is confineWorkspacePath without the existence
// requirement: the path is validated lexically, with symlinks resolved
// through the nearest existing ancestor (workspace.Canonicalize). Git diff
// targets may be deleted from the worktree, so existence cannot be required.
func confineWorkspacePathLoose(root, rel string) (string, error) {
	if filepath.IsAbs(rel) {
		return "", invalidInput("path must be workspace-relative")
	}
	cleaned := filepath.Clean(rel)
	if cleaned == ".." || strings.HasPrefix(cleaned, ".."+string(filepath.Separator)) {
		return "", invalidInput("path escapes the workspace")
	}
	abs := workspace.Canonicalize(filepath.Join(root, cleaned))
	if abs != root && !strings.HasPrefix(abs, root+string(filepath.Separator)) {
		return "", invalidInput("path escapes the workspace")
	}
	return abs, nil
}

// handleWorkspaceGitDiff serves GET /v1/workspaces/{id}/git/diff?path=<rel>&staged=1.
// Tracked files default to `git diff HEAD` (staged+unstaged combined, matching
// the merged numstat of the status rows); staged=1 serves the index diff.
// Untracked files get a synthesized full-addition diff via --no-index;
// untracked directories are reported with is_dir (no meaningful diff).
func (s *Server) handleWorkspaceGitDiff(w http.ResponseWriter, r *http.Request) {
	_, root, err := s.workspaceExplorerRoot(r.Context(), r)
	if err != nil {
		writeError(w, err)
		return
	}
	rel := strings.TrimSpace(r.URL.Query().Get("path"))
	if rel == "" {
		writeError(w, invalidInput("path is required"))
		return
	}
	abs, err := confineWorkspacePathLoose(root, rel)
	if err != nil {
		writeError(w, err)
		return
	}
	ctx := r.Context()
	toplevel, prefix, err := gitRepoContext(ctx, root)
	if err != nil {
		writeError(w, invalidInput("not a git repository"))
		return
	}
	repoRel := prefix + filepath.ToSlash(filepath.Clean(rel))
	staged := r.URL.Query().Get("staged") == "1"

	resp := map[string]any{"path": rel}
	tracked := true
	if _, err := runGit(ctx, toplevel, "ls-files", "--error-unmatch", "--", repoRel); err != nil {
		tracked = false
	}
	var diff string
	if tracked {
		if staged {
			diff, _ = runGit(ctx, toplevel, "diff", "--cached", "--", repoRel)
		} else {
			out, derr := runGit(ctx, toplevel, "diff", "HEAD", "--", repoRel)
			if derr != nil {
				// Unborn HEAD (no commits yet): fall back to the worktree diff.
				out, _ = runGit(ctx, toplevel, "diff", "--", repoRel)
			}
			diff = out
		}
	} else if _, serr := os.Stat(abs); serr != nil {
		// Not in the index and absent from the worktree: a staged deletion.
		// `git diff HEAD` carries the removed content.
		diff, _ = runGit(ctx, toplevel, "diff", "HEAD", "--", repoRel)
	} else if info, serr := os.Stat(abs); serr == nil && info.IsDir() {
		resp["is_dir"] = true
	} else {
		// /dev/null vs worktree → a pure-addition diff. Exit code 1 means
		// "differences found" and is the success path here.
		out, derr := runGit(ctx, root, "diff", "--no-index", "--", os.DevNull, filepath.ToSlash(rel))
		if out != "" {
			diff = out
		} else if derr != nil {
			var ee *exec.ExitError
			if !errors.As(derr, &ee) || ee.ExitCode() != 1 {
				writeError(w, invalidInput("git diff failed"))
				return
			}
		}
		resp["untracked"] = true
	}
	if len(diff) > maxGitDiffBytes {
		diff = diff[:maxGitDiffBytes]
		resp["truncated"] = true
	}
	resp["diff"] = diff
	writeJSON(w, http.StatusOK, resp)
}

// --- fuzzy file search (composer @ completion) ---

// Search is a bounded recursive walk confined to the workspace root:
// case-insensitive substring match on the workspace-relative path, ranked
// basename-prefix > path-prefix > basename-substring > path-substring.
// Hidden entries, .git and node_modules are skipped; two caps (visited
// entries, returned matches) bound the cost on large trees.
const (
	maxSearchVisited = 20000
	maxSearchResults = 50
)

// wsFileMatch is one @-completion candidate.
type wsFileMatch struct {
	Path string `json:"path"` // workspace-relative
	Name string `json:"name"`
	Kind string `json:"kind"` // dir | file
}

// handleSearchWorkspaceFiles serves GET /v1/workspaces/{id}/files/search?q=...
func (s *Server) handleSearchWorkspaceFiles(w http.ResponseWriter, r *http.Request) {
	_, root, err := s.workspaceExplorerRoot(r.Context(), r)
	if err != nil {
		writeError(w, err)
		return
	}
	q := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("q")))
	if q == "" {
		writeError(w, invalidInput("q is required"))
		return
	}

	visited := 0
	truncated := false
	matches := make([]wsFileMatch, 0, 16)
	_ = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil // unreadable entries are simply skipped
		}
		if p == root {
			return nil
		}
		visited++
		if visited > maxSearchVisited || len(matches) >= maxSearchResults*4 {
			truncated = true
			return fs.SkipAll
		}
		name := d.Name()
		rel := filepath.ToSlash(p[len(root)+1:])
		if d.IsDir() {
			if name == ".git" || name == "node_modules" || hiddenDir.MatchString(name) {
				return filepath.SkipDir
			}
		} else if hiddenDir.MatchString(name) {
			return nil
		}
		if strings.Contains(strings.ToLower(rel), q) {
			kind := "file"
			if d.IsDir() {
				kind = "dir"
			}
			matches = append(matches, wsFileMatch{Path: rel, Name: name, Kind: kind})
		}
		return nil
	})

	rank := func(m wsFileMatch) int {
		name, path := strings.ToLower(m.Name), strings.ToLower(m.Path)
		switch {
		case strings.HasPrefix(name, q):
			return 0
		case strings.HasPrefix(path, q):
			return 1
		case strings.Contains(name, q):
			return 2
		default:
			return 3
		}
	}
	sort.Slice(matches, func(i, j int) bool {
		ri, rj := rank(matches[i]), rank(matches[j])
		if ri != rj {
			return ri < rj
		}
		// Same rank: files first (the common @-completion target), then shorter paths
		if (matches[i].Kind == "file") != (matches[j].Kind == "file") {
			return matches[i].Kind == "file"
		}
		return len(matches[i].Path) < len(matches[j].Path)
	})
	if len(matches) > maxSearchResults {
		matches = matches[:maxSearchResults]
		truncated = true
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"query":     q,
		"matches":   matches,
		"truncated": truncated,
	})
}

// --- approval mode quick switch (composer picker) ---

// handleGetWorkspaceApprovalMode serves GET /v1/workspaces/{id}/approval-mode:
// the effective mode (live override or configured default). Reloaded pages
// read this instead of the config file, so they never misreport a live
// override set earlier in the process.
func (s *Server) handleGetWorkspaceApprovalMode(w http.ResponseWriter, r *http.Request) {
	id, err := parseWorkspaceIDParam(r.PathValue("id"))
	if err != nil {
		writeError(w, err)
		return
	}
	mode, err := s.svc.WorkspaceApprovalMode(r.Context(), id)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"mode": mode})
}

type setWorkspaceApprovalModeRequest struct {
	Mode string `json:"mode"`
}

// handleSetWorkspaceApprovalMode serves POST /v1/workspaces/{id}/approval-mode.
// The override applies to the workspace's live bootstrap (subsequent turns;
// a turn already constructed keeps its captured policy) and is intentionally
// NOT persisted — a config reload or process restart falls back to the
// configured approval.mode.
func (s *Server) handleSetWorkspaceApprovalMode(w http.ResponseWriter, r *http.Request) {
	id, err := parseWorkspaceIDParam(r.PathValue("id"))
	if err != nil {
		writeError(w, err)
		return
	}
	var req setWorkspaceApprovalModeRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	mode, err := permission.ParseApprovalMode(strings.TrimSpace(req.Mode))
	if err != nil {
		writeError(w, invalidInput("invalid approval mode"))
		return
	}
	if err := s.svc.SetWorkspaceApprovalMode(r.Context(), id, mode); err != nil {
		writeError(w, err)
		return
	}
	s.auditf("workspace.approval_mode", domain.SessionID{}, "workspace_id", id.String(), "mode", string(mode))
	writeJSON(w, http.StatusOK, map[string]any{"mode": string(mode)})
}
