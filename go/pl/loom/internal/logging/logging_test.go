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

package logging

import (
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestGlogFormat(t *testing.T) {
	var b strings.Builder
	logger := slog.New(NewGlogHandler(&lockedBuilder{&b}, nil))
	logger.Info("session resumed", "session_id", "sess_abc", "count", 3)
	line := b.String()
	// I0805 18:02:38.869838  logging_test.go:NN] session resumed session_id=sess_abc count=3
	pattern := `^I\d{4} \d{2}:\d{2}:\d{2}\.\d{6}  logging_test\.go:\d+\] session resumed session_id=sess_abc count=3` + "\n$"
	if !regexp.MustCompile(pattern).MatchString(line) {
		t.Fatalf("glog format mismatch: %q", line)
	}
}

func TestGlogLevelsAndFilter(t *testing.T) {
	var b strings.Builder
	logger := slog.New(NewGlogHandler(&lockedBuilder{&b}, nil))
	logger.Debug("hidden")
	logger.Warn("warn line")
	logger.Error("error line")
	out := b.String()
	if strings.Contains(out, "hidden") {
		t.Fatalf("debug below default INFO level must be filtered: %q", out)
	}
	if !strings.Contains(out, "W") || !regexp.MustCompile(`(?m)^W\d{4} `).MatchString(out) {
		t.Fatalf("warn line must carry W prefix: %q", out)
	}
	if !regexp.MustCompile(`(?m)^E\d{4} `).MatchString(out) {
		t.Fatalf("error line must carry E prefix: %q", out)
	}
}

func TestGlogAttrQuoting(t *testing.T) {
	var b strings.Builder
	logger := slog.New(NewGlogHandler(&lockedBuilder{&b}, nil))
	logger.Info("m", "msg", "has space", "plain", "ok")
	out := b.String()
	if !strings.Contains(out, `msg="has space"`) || !strings.Contains(out, "plain=ok") {
		t.Fatalf("attr quoting mismatch: %q", out)
	}
}

func TestDailyWriterRotatesByDate(t *testing.T) {
	dir := t.TempDir()
	day := time.Date(2026, 8, 5, 23, 59, 59, 0, time.Local)
	w := &dailyWriter{dir: dir, prefix: "loom", now: func() time.Time { return day }}
	if _, err := w.Write([]byte("before midnight\n")); err != nil {
		t.Fatalf("write day1: %v", err)
	}
	day = day.Add(2 * time.Second) // rolls into 2026-08-06
	if _, err := w.Write([]byte("after midnight\n")); err != nil {
		t.Fatalf("write day2: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	day1, err := os.ReadFile(filepath.Join(dir, "loom.2026-08-05.log"))
	if err != nil {
		t.Fatalf("read day1 file: %v", err)
	}
	day2, err := os.ReadFile(filepath.Join(dir, "loom.2026-08-06.log"))
	if err != nil {
		t.Fatalf("read day2 file: %v", err)
	}
	if string(day1) != "before midnight\n" || string(day2) != "after midnight\n" {
		t.Fatalf("rotation split wrong: day1=%q day2=%q", day1, day2)
	}
}

func TestNewFileLoggerCreatesDir(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "logs")
	logger, err := NewFileLogger(dir, nil, Quotas{})
	if err != nil {
		t.Fatalf("NewFileLogger: %v", err)
	}
	logger.Info("hello")
	name := "loom." + time.Now().Format("2006-01-02") + ".log"
	content, err := os.ReadFile(filepath.Join(dir, name))
	if err != nil {
		t.Fatalf("read log file: %v", err)
	}
	if !strings.Contains(string(content), "hello") {
		t.Fatalf("log file missing record: %q", content)
	}
}

// 单文件超过 maxFile 后同日序号轮转：loom.<day>.log → .1.log → .2.log。
func TestDailyWriterFileSizeRotation(t *testing.T) {
	dir := t.TempDir()
	day := time.Date(2026, 8, 5, 12, 0, 0, 0, time.Local)
	w := &dailyWriter{dir: dir, prefix: "loom", now: func() time.Time { return day }, maxFile: 16}
	for _, chunk := range []string{"aaaaaaaaaa", "bbbbbbbbbb", "cccccccccc"} { // 10B each
		if _, err := w.Write([]byte(chunk)); err != nil {
			t.Fatalf("write %q: %v", chunk, err)
		}
	}
	if err := w.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	want := map[string]string{
		"loom.2026-08-05.log":   "aaaaaaaaaa",
		"loom.2026-08-05.1.log": "bbbbbbbbbb",
		"loom.2026-08-05.2.log": "cccccccccc",
	}
	for name, content := range want {
		got, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatalf("read %s: %v", name, err)
		}
		if string(got) != content {
			t.Fatalf("%s = %q, want %q", name, got, content)
		}
	}
}

// 目录总量超过 maxTotal 后从最旧文件开始清理；当前打开的文件永不删。
func TestDailyWriterTotalQuotaGC(t *testing.T) {
	dir := t.TempDir()
	day := time.Date(2026, 8, 5, 12, 0, 0, 0, time.Local)
	w := &dailyWriter{dir: dir, prefix: "loom", now: func() time.Time { return day }, maxTotal: 25}
	for i := 0; i < 4; i++ { // 4 天，每天 10B
		if _, err := w.Write([]byte("0123456789")); err != nil {
			t.Fatalf("write day %d: %v", i, err)
		}
		day = day.Add(24 * time.Hour)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "loom.2026-08-05.log")); !os.IsNotExist(err) {
		t.Fatalf("oldest file must be garbage-collected, stat err = %v", err)
	}
	for _, name := range []string{"loom.2026-08-06.log", "loom.2026-08-07.log", "loom.2026-08-08.log"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Fatalf("%s must survive GC: %v", name, err)
		}
	}
}

// lockedBuilder serializes writes (the production dailyWriter carries its
// own mutex; strings.Builder does not).
type lockedBuilder struct{ b *strings.Builder }

func (w *lockedBuilder) Write(p []byte) (int, error) { return w.b.Write(p) }

// TestDailyWriterConcurrentWriters hammers dailyWriter from many goroutines
// to verify that all lines land intact and no write is lost or interleaved.
func TestDailyWriterConcurrentWriters(t *testing.T) {
	dir := t.TempDir()
	day := time.Date(2026, 8, 5, 12, 0, 0, 0, time.Local)
	w := &dailyWriter{dir: dir, prefix: "loom", now: func() time.Time { return day }}

	const goroutines = 50
	const perGoroutine = 200

	var wg sync.WaitGroup
	start := make(chan struct{})
	for i := 0; i < goroutines; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			<-start
			for j := 0; j < perGoroutine; j++ {
				line := fmt.Sprintf("g%04d-l%04d\n", id, j)
				if _, err := w.Write([]byte(line)); err != nil {
					t.Errorf("write error: %v", err)
					return
				}
			}
		}(i)
	}
	close(start)
	wg.Wait()

	if err := w.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	content, err := os.ReadFile(filepath.Join(dir, "loom.2026-08-05.log"))
	if err != nil {
		t.Fatalf("read log file: %v", err)
	}

	// Every line must be a complete record; none should be truncated or
	// interleaved with another goroutine's output.
	lines := strings.Split(strings.TrimSuffix(string(content), "\n"), "\n")
	want := goroutines * perGoroutine
	if len(lines) != want {
		t.Fatalf("line count = %d, want %d", len(lines), want)
	}

	// Verify each line matches the expected format.
	lineRe := regexp.MustCompile(`^g\d{4}-l\d{4}$`)
	seen := make(map[string]bool, want)
	for _, line := range lines {
		if !lineRe.MatchString(line) {
			t.Fatalf("corrupted line: %q", line)
		}
		if seen[line] {
			t.Fatalf("duplicate line: %q", line)
		}
		seen[line] = true
	}
}
