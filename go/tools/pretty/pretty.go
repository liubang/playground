// Copyright (c) 2025 The Authors. All rights reserved.
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
// Created: 2025/11/19 17:20

// Package pretty renders ASCII tables with automatic column alignment and
// support for CJK characters.
//
// The Table type represents a table as pure data — the rendering is a
// separate operation, so the same table can be rendered to different outputs.
//
// Usage:
//
//	var t pretty.Table
//	t.Add("Name", "Age", "City")   // first row = header
//	t.Add("Alice", "30", "NYC")
//	t.Add("Bob", "25", "LA")
//	t.Add("─")                     // separator line (single repeated char)
//	t.Add("Total", "2", "")
//	fmt.Println(&t)
package pretty

import (
	"fmt"
	"io"
	"os"
	"strings"
	"unicode"

	sc "github.com/liubang/playground/go/tools/string"
	"github.com/liubang/playground/go/tools/terminal"
)

// ---------------------------------------------------------------------------
// internal helpers
// ---------------------------------------------------------------------------

type rowKind uint8

const (
	rowData rowKind = iota
	rowSep
)

type row struct {
	cols    []string
	kind    rowKind
	sepRune rune // character to repeat for separator rows
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

// Table is an ASCII table. The zero value is ready to use — just call Add.
type Table struct {
	rows    []row
	maxCols int
	tty     bool
	ttySet  bool
}

// Add appends a row to the table. The first call defines the header and
// determines the initial column count — subsequent rows adapt automatically.
//
// A single string that consists of a single non-alphanumeric character
// repeated (e.g. "─", "---", "=", "━") is rendered as a horizontal
// separator spanning all columns.
func (t *Table) Add(cols ...string) *Table {
	if len(cols) == 0 {
		return t
	}
	if !t.ttySet {
		t.tty = terminal.Isatty(os.Stdout.Fd())
		t.ttySet = true
	}
	if len(cols) > t.maxCols {
		t.maxCols = len(cols)
	}

	r := row{cols: cols}
	if t.maxCols > 1 && len(cols) == 1 && isSep(cols[0]) {
		r.kind = rowSep
		r.sepRune = firstRune(cols[0])
	}
	t.rows = append(t.rows, r)
	return t
}

// RenderTo writes the formatted table to w.
func (t *Table) RenderTo(w io.Writer) (err error) {
	if w == nil || len(t.rows) == 0 {
		return nil
	}
	widths := t.colWidths()
	total := totalWidth(widths, t.maxCols)

	if t.tty {
		if err = printBorder(w, total, '='); err != nil {
			return err
		}
		if err = printData(w, t.rows[0].cols, widths, t.maxCols, t.tty); err != nil {
			return err
		}
		if err = printBorder(w, total, '='); err != nil {
			return err
		}
	}
	for i := 1; i < len(t.rows); i++ {
		r := t.rows[i]
		if r.kind == rowSep {
			if t.tty {
				if err = printBorder(w, total, r.sepRune); err != nil {
					return err
				}
			}
		} else {
			if err = printData(w, r.cols, widths, t.maxCols, t.tty); err != nil {
				return err
			}
		}
	}
	if t.tty {
		if err = printBorder(w, total, '='); err != nil {
			return err
		}
	}
	return nil
}

// Render writes the formatted table to os.Stdout.
func (t *Table) Render() { _ = t.RenderTo(os.Stdout) }

// String returns the rendered table as a string.
func (t *Table) String() string {
	var b strings.Builder
	_ = t.RenderTo(&b) // strings.Builder never fails
	return b.String()
}

// ---------------------------------------------------------------------------
// column-width calculation
// ---------------------------------------------------------------------------

func (t *Table) colWidths() []int {
	w := make([]int, t.maxCols)
	for _, r := range t.rows {
		if r.kind == rowSep {
			continue
		}
		for i, col := range r.cols {
			if cw := int(sc.StrPrintLenInTerm(col)); cw > w[i] {
				w[i] = cw
			}
		}
	}
	return w
}

func totalWidth(widths []int, maxCols int) int {
	n := maxCols*3 + 1
	for _, w := range widths {
		n += w
	}
	return n
}

// ---------------------------------------------------------------------------
// rendering primitives
// ---------------------------------------------------------------------------

func printBorder(w io.Writer, totalLen int, ch rune) error {
	var b strings.Builder
	b.WriteByte('+')
	b.WriteString(strings.Repeat(string(ch), totalLen-2))
	b.WriteByte('+')
	_, err := fmt.Fprintln(w, b.String())
	return err
}

func printData(w io.Writer, cols []string, widths []int, maxCols int, tty bool) error {
	var b strings.Builder
	if tty {
		b.WriteString("| ")
	} else {
		b.WriteByte(' ')
	}
	for i := 0; i < maxCols; i++ {
		val := ""
		if i < len(cols) {
			val = cols[i]
		}
		b.WriteString(padRight(val, widths[i], ' '))
		if tty {
			b.WriteString(" |")
		}
		if i < maxCols-1 {
			b.WriteByte(' ')
		}
	}
	_, err := fmt.Fprintln(w, b.String())
	return err
}

// ---------------------------------------------------------------------------
// string helpers
// ---------------------------------------------------------------------------

func padRight(str string, width int, pad rune) string {
	cur := int(sc.StrPrintLenInTerm(str))
	if cur >= width {
		return str
	}
	var b strings.Builder
	b.WriteString(str)
	b.WriteString(strings.Repeat(string(pad), width-cur))
	return b.String()
}

func isSep(s string) bool {
	if len(s) == 0 {
		return false
	}
	var first rune
	for i, r := range s {
		if i == 0 {
			first = r
		} else if r != first {
			return false
		}
	}
	return !unicode.IsLetter(first) && !unicode.IsDigit(first) && !unicode.IsSpace(first)
}

func firstRune(s string) rune {
	for _, r := range s {
		return r
	}
	return 0
}
