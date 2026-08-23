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
// Created: 2026/08/23

package permission

import (
	"reflect"
	"testing"
)

var gitPushTestOpts = gitPushOpts // alias for readability

func TestParseOpts(t *testing.T) {
	tests := []struct {
		name      string
		args      []string
		wantFlags []string
		wantPos   []string
		wantOK    bool
	}{
		{name: "empty", args: nil, wantFlags: nil, wantPos: nil, wantOK: true},
		{
			name:      "plain positionals",
			args:      []string{"origin", "main"},
			wantFlags: nil, wantPos: []string{"origin", "main"}, wantOK: true,
		},
		{
			name:      "combined short flags split",
			args:      []string{"-fu", "origin"},
			wantFlags: []string{"-f", "-u"}, wantPos: []string{"origin"}, wantOK: true,
		},
		{
			name:      "long flag with equals",
			args:      []string{"--force-with-lease", "--repo=upstream", "main"},
			wantFlags: []string{"--force-with-lease", "--repo"}, wantPos: []string{"main"}, wantOK: true,
		},
		{
			name:      "value flag consumes next token",
			args:      []string{"-o", "ci.skip", "origin"},
			wantFlags: []string{"-o"}, wantPos: []string{"origin"}, wantOK: true,
		},
		{
			name:      "attached short value",
			args:      []string{"-oci.skip", "origin"},
			wantFlags: []string{"-o"}, wantPos: []string{"origin"}, wantOK: true,
		},
		{
			name:      "terminator",
			args:      []string{"--", "--not-a-flag"},
			wantFlags: nil, wantPos: []string{"--not-a-flag"}, wantOK: true,
		},
		{
			name:      "unique abbreviation resolves",
			args:      []string{"--force-w"},
			wantOK:    true,
			wantFlags: []string{"--force-with-lease"}, wantPos: nil,
		},
		{
			name:   "ambiguous abbreviation fails",
			args:   []string{"--forc"},
			wantOK: false,
		},
		{
			name:   "unknown long flag fails",
			args:   []string{"--forcce"},
			wantOK: false,
		},
		{
			name:   "unknown short flag fails",
			args:   []string{"-Z"},
			wantOK: false,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			opts, ok := ParseOpts(tt.args, gitPushTestOpts)
			if ok != tt.wantOK {
				t.Fatalf("ParseOpts(%v) ok = %v, want %v (flags=%v pos=%v)", tt.args, ok, tt.wantOK, opts.Flags, opts.Positional)
			}
			if !ok {
				return
			}
			if !reflect.DeepEqual(opts.Flags, tt.wantFlags) {
				t.Errorf("flags = %v, want %v", opts.Flags, tt.wantFlags)
			}
			if !reflect.DeepEqual(opts.Positional, tt.wantPos) {
				t.Errorf("positional = %v, want %v", opts.Positional, tt.wantPos)
			}
		})
	}
}

// TestParseOptsAbbreviation pins GNU getopt_long abbreviation semantics:
// an unambiguous prefix resolves, an ambiguous one fails the parse.
func TestParseOptsAbbreviation(t *testing.T) {
	table := OptTable{Long: map[string]bool{"recursive": false, "recover": true, "force": false}}
	if _, ok := ParseOpts([]string{"--rec", "x"}, table); ok {
		t.Fatal("--rec must be ambiguous (recursive/recover)")
	}
	opts, ok := ParseOpts([]string{"--recur", "x"}, table)
	if !ok || !opts.Has("--recursive") {
		t.Fatalf("--recur must resolve to --recursive, got %+v ok=%v", opts, ok)
	}
}

func TestOptsHas(t *testing.T) {
	opts := Opts{Flags: []string{"-f", "--delete"}, Positional: []string{"origin"}}
	if !opts.Has("-f", "--force") {
		t.Error("Has(-f) = false, want true")
	}
	if opts.Has("-u") {
		t.Error("Has(-u) = true, want false")
	}
}
