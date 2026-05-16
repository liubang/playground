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
// Created: 2026/05/16 20:57

package pretty

import (
	"bytes"
	"io"
	"os"
	"strings"
	"testing"
)

// captureStdout captures stdout output from a function call.
func captureStdout(f func()) string {
	old := os.Stdout
	r, w, _ := os.Pipe()
	os.Stdout = w

	f()

	w.Close()
	os.Stdout = old

	var buf bytes.Buffer
	io.Copy(&buf, r)
	return buf.String()
}

func TestNewPretty(t *testing.T) {
	headers := []string{"Name", "Age", "City"}
	p := NewPretty(headers)
	if p == nil {
		t.Fatal("NewPretty returned nil")
	}
	if p.maxcell_per_line != 3 {
		t.Errorf("maxcell_per_line = %d, want 3", p.maxcell_per_line)
	}
	if len(p.lines) != 1 {
		t.Errorf("lines count = %d, want 1 (header row)", len(p.lines))
	}
	if len(p.cell_max_length) != 3 {
		t.Errorf("cell_max_length len = %d, want 3", len(p.cell_max_length))
	}
}

func TestAddRow(t *testing.T) {
	headers := []string{"Name", "Age"}
	p := NewPretty(headers)
	p.AddRow([]string{"Alice", "30"})
	p.AddRow([]string{"Bob", "25"})

	// header + 2 data rows = 3 lines
	if len(p.lines) != 3 {
		t.Errorf("lines count = %d, want 3", len(p.lines))
	}
}

func TestReset(t *testing.T) {
	headers := []string{"A", "B"}
	p := NewPretty(headers)
	p.AddRow([]string{"1", "2"})

	newHeaders := []string{"X", "Y", "Z"}
	p.Reset(newHeaders)

	if p.maxcell_per_line != 3 {
		t.Errorf("after Reset, maxcell_per_line = %d, want 3", p.maxcell_per_line)
	}
	if len(p.lines) != 1 {
		t.Errorf("after Reset, lines count = %d, want 1", len(p.lines))
	}
}

func TestCellMaxLength(t *testing.T) {
	headers := []string{"Name", "Value"}
	p := NewPretty(headers)
	p.AddRow([]string{"LongName", "x"})
	p.AddRow([]string{"A", "LongValue"})

	// "LongName" has length 8, which is > "Name" (4)
	if p.cell_max_length[0] < 8 {
		t.Errorf("cell_max_length[0] = %d, want >= 8", p.cell_max_length[0])
	}
	// "LongValue" has length 9, which is > "Value" (5)
	if p.cell_max_length[1] < 9 {
		t.Errorf("cell_max_length[1] = %d, want >= 9", p.cell_max_length[1])
	}
}

func TestRender(t *testing.T) {
	headers := []string{"Name", "Age"}
	p := NewPretty(headers)
	p.AddRow([]string{"Alice", "30"})

	output := captureStdout(func() {
		p.Render()
	})

	// In non-tty (test sandbox), header is skipped, only data rows are rendered
	if !strings.Contains(output, "Alice") {
		t.Error("Render output should contain 'Alice'")
	}
	if !strings.Contains(output, "30") {
		t.Error("Render output should contain '30'")
	}
}

func TestRenderWithSep(t *testing.T) {
	headers := []string{"Col1", "Col2"}
	p := NewPretty(headers)
	p.AddRow([]string{"a", "b"})
	p.Next()
	p.AddSep("-").AddSep("-")
	p.AddRow([]string{"c", "d"})

	output := captureStdout(func() {
		p.Render()
	})

	if !strings.Contains(output, "a") {
		t.Error("Render output should contain 'a'")
	}
	if !strings.Contains(output, "d") {
		t.Error("Render output should contain 'd'")
	}
}

func TestRenderChineseChars(t *testing.T) {
	headers := []string{"姓名", "年龄"}
	p := NewPretty(headers)
	p.AddRow([]string{"张三", "25"})

	output := captureStdout(func() {
		p.Render()
	})

	// In non-tty (test sandbox), header is skipped, only data rows are rendered
	if !strings.Contains(output, "张三") {
		t.Error("Render output should contain '张三'")
	}
	if !strings.Contains(output, "25") {
		t.Error("Render output should contain '25'")
	}
}

func TestPadRight(t *testing.T) {
	p := &Pretty{}
	result := p.padRight("hi", 5, ' ')
	if len(result) < 5 {
		t.Errorf("padRight result length = %d, want >= 5", len(result))
	}
	if result[:2] != "hi" {
		t.Errorf("padRight should preserve original string prefix, got %q", result)
	}
}

func TestAddStr(t *testing.T) {
	headers := []string{"A"}
	p := NewPretty(headers)
	p.Next()
	p.AddStr("test")

	if len(p.lines) != 2 {
		t.Errorf("lines count = %d, want 2", len(p.lines))
	}
	if p.lines[1][0] == nil {
		t.Fatal("cell should not be nil")
	}
	if p.lines[1][0].val != "test" {
		t.Errorf("cell val = %q, want %q", p.lines[1][0].val, "test")
	}
	if p.lines[1][0].t != CT_STRING {
		t.Errorf("cell type = %d, want CT_STRING(%d)", p.lines[1][0].t, CT_STRING)
	}
}
