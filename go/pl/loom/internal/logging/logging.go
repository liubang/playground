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
// Created: 2026/08/05

// Package logging implements loom's unified file logging: a glog-style
// slog handler writing to date-rotated files (<logsDir>/loom.YYYY-MM-DD.log,
// rolling over at local midnight). TUI and serve modes share it; the
// server audit logger inherits it via server.New's audit==nil fallback.
package logging

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// 日志配额（字节）：单文件超过 DefaultMaxFileBytes 后同日序号轮转
// （loom.2026-08-05.1.log …）；目录总量超过 DefaultMaxTotalBytes 后从
// 最旧的文件开始清理（当前打开的文件永不删）。
const (
	DefaultMaxFileBytes  = 2 << 30  // 2 GiB
	DefaultMaxTotalBytes = 10 << 30 // 10 GiB
)

// Quotas bounds log file sizes (config.yaml logging.max_file_mb /
// max_total_mb 经 config.resolveLogging 换算后传入）。零值字段取
// Default* 常量；负值表示不限制。
type Quotas struct {
	MaxFileBytes  int64
	MaxTotalBytes int64
}

func (q Quotas) fileCap() int64 {
	if q.MaxFileBytes == 0 {
		return DefaultMaxFileBytes
	}
	return q.MaxFileBytes
}

func (q Quotas) totalCap() int64 {
	if q.MaxTotalBytes == 0 {
		return DefaultMaxTotalBytes
	}
	return q.MaxTotalBytes
}

// NewFileLogger builds the shared loom logger: glog-style records written to
// <logsDir>/loom.YYYY-MM-DD.log. The directory is created and today's file
// opened eagerly so path/permission problems surface at startup instead of
// at the first log line. level nil means INFO.
func NewFileLogger(logsDir string, level slog.Leveler, quotas Quotas) (*slog.Logger, error) {
	w := &dailyWriter{
		dir: logsDir, prefix: "loom", now: time.Now,
		maxFile: quotas.fileCap(), maxTotal: quotas.totalCap(),
	}
	if _, err := w.Write(nil); err != nil {
		return nil, err
	}
	return slog.New(NewGlogHandler(w, level)), nil
}

// NewGlogHandler renders slog records in glog text format:
//
//	I0805 18:02:38.869838  server.go:42] message key=value
//
// (level letter, local mmdd hh:mm:ss.μs, source file:line, message, attrs).
// The writer must be concurrency-safe (dailyWriter is).
func NewGlogHandler(w io.Writer, level slog.Leveler) slog.Handler {
	if level == nil {
		level = slog.LevelInfo
	}
	return &glogHandler{w: w, level: level}
}

// --- glog-style handler ---

type glogHandler struct {
	w      io.Writer
	level  slog.Leveler
	prefix string   // pre-rendered WithAttrs
	groups []string // WithGroups path
}

func (h *glogHandler) Enabled(_ context.Context, l slog.Level) bool {
	return l >= h.level.Level()
}

func (h *glogHandler) Handle(_ context.Context, r slog.Record) error {
	var b bytes.Buffer
	b.WriteByte(levelChar(r.Level))
	b.WriteString(r.Time.Format("0102 15:04:05.000000"))
	b.WriteString("  ")
	b.WriteString(sourceRef(r.PC))
	b.WriteString("] ")
	b.WriteString(r.Message)
	if h.prefix != "" {
		b.WriteByte(' ')
		b.WriteString(h.prefix)
	}
	r.Attrs(func(a slog.Attr) bool {
		b.WriteByte(' ')
		writeAttr(&b, h.groups, a)
		return true
	})
	b.WriteByte('\n')
	_, err := h.w.Write(b.Bytes())
	return err
}

func (h *glogHandler) WithAttrs(attrs []slog.Attr) slog.Handler {
	var b bytes.Buffer
	b.WriteString(h.prefix)
	for _, a := range attrs {
		if b.Len() > 0 {
			b.WriteByte(' ')
		}
		writeAttr(&b, h.groups, a)
	}
	return &glogHandler{w: h.w, level: h.level, prefix: b.String(), groups: h.groups}
}

func (h *glogHandler) WithGroup(name string) slog.Handler {
	if name == "" {
		return h
	}
	groups := make([]string, 0, len(h.groups)+1)
	groups = append(groups, h.groups...)
	groups = append(groups, name)
	return &glogHandler{w: h.w, level: h.level, prefix: h.prefix, groups: groups}
}

func levelChar(l slog.Level) byte {
	switch {
	case l >= slog.LevelError:
		return 'E'
	case l >= slog.LevelWarn:
		return 'W'
	case l >= slog.LevelInfo:
		return 'I'
	default:
		return 'D'
	}
}

func sourceRef(pc uintptr) string {
	if pc == 0 {
		return "???:0"
	}
	frame, _ := runtime.CallersFrames([]uintptr{pc}).Next()
	return filepath.Base(frame.File) + ":" + strconv.Itoa(frame.Line)
}

// writeAttr renders one attribute as key=value, flattening groups with dots
// and quoting values that contain whitespace or quotes.
func writeAttr(b *bytes.Buffer, groups []string, a slog.Attr) {
	a.Value = a.Value.Resolve()
	if a.Value.Kind() == slog.KindGroup {
		for _, ga := range a.Value.Group() {
			writeAttr(b, appendKey(groups, a.Key), ga)
		}
		return
	}
	key := strings.Join(appendKey(groups, a.Key), ".")
	v := a.Value.String()
	if v == "" || strings.ContainsAny(v, " \t\n\"") {
		v = strconv.Quote(v)
	}
	b.WriteString(key)
	b.WriteByte('=')
	b.WriteString(v)
}

func appendKey(groups []string, key string) []string {
	out := make([]string, 0, len(groups)+1)
	out = append(out, groups...)
	return append(out, key)
}

// --- date/size-rotated file writer with a total-size quota ---

// dailyWriter appends to <dir>/<prefix>.YYYY-MM-DD.log, rolling to a new
// file when the local date changes, to a same-day sequence file
// (<prefix>.<day>.<n>.log) when maxFile is exceeded, and garbage-collecting
// the oldest files when the directory total exceeds maxTotal. Zero caps
// mean unlimited. Safe for concurrent use.
type dailyWriter struct {
	mu       sync.Mutex
	dir      string
	prefix   string
	now      func() time.Time // injectable for tests
	maxFile  int64
	maxTotal int64
	day      string
	seq      int
	current  string
	size     int64
	f        *os.File
}

func (w *dailyWriter) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	today := w.now().Format("2006-01-02")
	switch {
	case w.f == nil || today != w.day:
		w.day, w.seq = today, 0
		if err := w.openCurrent(); err != nil {
			return 0, err
		}
		// 进程重启后当天的文件可能已达上限：直接推进到序号文件。
		for w.maxFile > 0 && w.size >= w.maxFile {
			w.seq++
			if err := w.openCurrent(); err != nil {
				return 0, err
			}
		}
		w.gc()
	case w.maxFile > 0 && w.size > 0 && w.size+int64(len(p)) > w.maxFile:
		w.seq++
		if err := w.openCurrent(); err != nil {
			return 0, err
		}
		w.gc()
	}
	n, err := w.f.Write(p)
	w.size += int64(n)
	return n, err
}

func (w *dailyWriter) fileName() string {
	if w.seq == 0 {
		return w.prefix + "." + w.day + ".log"
	}
	return fmt.Sprintf("%s.%s.%d.log", w.prefix, w.day, w.seq)
}

func (w *dailyWriter) openCurrent() error {
	if w.f != nil {
		_ = w.f.Close()
		w.f = nil
	}
	if err := os.MkdirAll(w.dir, 0o755); err != nil {
		return err
	}
	name := w.fileName()
	f, err := os.OpenFile(filepath.Join(w.dir, name), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	info, err := f.Stat()
	if err != nil {
		_ = f.Close()
		return err
	}
	w.f, w.current, w.size = f, name, info.Size()
	return nil
}

// gc deletes the oldest log files until the directory total fits maxTotal.
// The currently open file is never removed.
func (w *dailyWriter) gc() {
	if w.maxTotal <= 0 {
		return
	}
	entries, err := os.ReadDir(w.dir)
	if err != nil {
		return
	}
	type cand struct {
		name    string
		size    int64
		modTime time.Time
	}
	var files []cand
	var total int64
	for _, e := range entries {
		if e.IsDir() || !strings.HasPrefix(e.Name(), w.prefix+".") || !strings.HasSuffix(e.Name(), ".log") {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		files = append(files, cand{e.Name(), info.Size(), info.ModTime()})
		total += info.Size()
	}
	sort.Slice(files, func(i, j int) bool { return files[i].modTime.Before(files[j].modTime) })
	for _, c := range files {
		if total <= w.maxTotal {
			return
		}
		if c.name == w.current {
			continue
		}
		if err := os.Remove(filepath.Join(w.dir, c.name)); err == nil {
			total -= c.size
		}
	}
}

// Close flushes and releases the current file.
func (w *dailyWriter) Close() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.f == nil {
		return nil
	}
	err := w.f.Close()
	w.f = nil
	return err
}
