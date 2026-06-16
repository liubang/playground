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
	"strings"
	"testing"
)

func TestTableZeroValue(t *testing.T) {
	var tbl Table
	// zero value should be safe to render
	out := tbl.String()
	if out != "" {
		t.Errorf("empty table String() = %q, want \"\"", out)
	}
}

func TestTableBasic(t *testing.T) {
	var tbl Table
	tbl.Add("Name", "Age").
		Add("Alice", "30")

	if tbl.maxCols != 2 {
		t.Errorf("maxCols = %d, want 2", tbl.maxCols)
	}
	if len(tbl.rows) != 2 {
		t.Errorf("rows = %d, want 2", len(tbl.rows))
	}
}

func TestTableRender(t *testing.T) {
	var tbl Table
	tbl.Add("Name", "Age").
		Add("Alice", "30")

	out := tbl.String()
	if !strings.Contains(out, "Alice") {
		t.Error("output should contain 'Alice'")
	}
	if !strings.Contains(out, "30") {
		t.Error("output should contain '30'")
	}
}

func TestTableHeaderIsFirstRow(t *testing.T) {
	var tbl Table
	tbl.Add("Header1", "Header2").
		Add("data1", "data2")

	if tbl.rows[0].cols[0] != "Header1" {
		t.Error("first row should be header")
	}
	if tbl.rows[0].kind != rowData {
		t.Error("header row kind should be rowData")
	}
}

func TestTableSeparator(t *testing.T) {
	var tbl Table
	tbl.Add("Col1", "Col2").
		Add("a", "b").
		Add("─").
		Add("c", "d")

	out := tbl.String()
	if !strings.Contains(out, "a") {
		t.Error("should contain 'a'")
	}
	if !strings.Contains(out, "c") {
		t.Error("should contain 'c'")
	}

	// verify the separator row was detected
	if tbl.rows[2].kind != rowSep {
		t.Error("'─' row should be detected as separator")
	}
}

func TestTableSeparatorVariants(t *testing.T) {
	tests := []string{"─", "━", "---", "===", "***", "___", "———", "···"}
	for _, sep := range tests {
		var tbl Table
		tbl.Add("A", "B").Add(sep)
		if tbl.rows[1].kind != rowSep {
			t.Errorf("%q should be detected as separator", sep)
		}
	}
}

func TestTableSeparatorNotDetectedForAlnum(t *testing.T) {
	tests := []string{"a", "1", "abc", "123"}
	for _, s := range tests {
		var tbl Table
		tbl.Add("A", "B").Add(s)
		if tbl.rows[1].kind == rowSep {
			t.Errorf("%q should NOT be detected as separator", s)
		}
	}
}

func TestTableSingleColumn(t *testing.T) {
	// Single-column tables don't have separators (no spanning needed)
	var tbl Table
	tbl.Add("Col").Add("data").Add("─")
	if tbl.rows[2].kind == rowSep {
		t.Error("single-column '─' should not be a separator")
	}
}

func TestTableColumnAutoExpand(t *testing.T) {
	var tbl Table
	tbl.Add("A"). // 1 column
			Add("B", "C").     // 2 columns — maxCols should grow
			Add("D", "E", "F") // 3 columns

	if tbl.maxCols != 3 {
		t.Errorf("maxCols = %d, want 3", tbl.maxCols)
	}
}

func TestTableRenderTo(t *testing.T) {
	var tbl Table
	tbl.Add("K", "V").Add("key1", "value1")

	var buf bytes.Buffer
	tbl.RenderTo(&buf)
	out := buf.String()

	if !strings.Contains(out, "key1") {
		t.Error("RenderTo output should contain 'key1'")
	}
}

func TestTableAddEmpty(t *testing.T) {
	var tbl Table
	tbl.Add() // no-op
	if len(tbl.rows) != 0 {
		t.Error("Add() with no args should be a no-op")
	}
}

func TestTableRenderNilWriter(t *testing.T) {
	var tbl Table
	tbl.Add("A").Add("1")
	// RenderTo(nil) should not panic
	tbl.RenderTo(nil)
}

func TestTableString(t *testing.T) {
	var tbl Table
	tbl.Add("Col").Add("data")

	out := tbl.String()
	// In non-TTY mode, header is skipped, only data rows printed
	if !strings.Contains(out, "data") {
		t.Error("String() should contain 'data'")
	}
}

func TestTableChineseChars(t *testing.T) {
	var tbl Table
	tbl.Add("姓名", "年龄").
		Add("张三", "25")

	out := tbl.String()
	if !strings.Contains(out, "张三") {
		t.Error("output should contain '张三'")
	}
	if !strings.Contains(out, "25") {
		t.Error("output should contain '25'")
	}
}

func TestTablePadRight(t *testing.T) {
	result := padRight("hi", 5, ' ')
	if len(result) < 5 {
		t.Errorf("padRight result length = %d, want >= 5", len(result))
	}
	if result[:2] != "hi" {
		t.Errorf("padRight should preserve original prefix, got %q", result)
	}
}

func TestTableEmptyRows(t *testing.T) {
	var tbl Table
	tbl.Add("A").Add("1").Add("")

	out := tbl.String()
	// Empty string cells should not cause issues
	if !strings.Contains(out, "1") {
		t.Error("output should contain '1'")
	}
}

func TestTableMethodChaining(t *testing.T) {
	var tbl Table
	tbl.Add("A", "B").
		Add("1", "2").
		Add("3", "4")

	if len(tbl.rows) != 3 {
		t.Errorf("expected 3 rows, got %d", len(tbl.rows))
	}
}
