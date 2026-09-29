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
// Created: 2026/09/27

package toolkit

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

type normalizeInner struct {
	Count int    `json:"count"`
	Note  string `json:"note"`
}

type normalizeSample struct {
	Pattern string            `json:"pattern"`
	Depth   int               `json:"depth"`
	Limit   int64             `json:"limit"`
	Ratio   float64           `json:"ratio"`
	Verbose bool              `json:"verbose"`
	Glob    []string          `json:"glob"`
	Specs   []normalizeInner  `json:"specs"`
	Blob    []byte            `json:"blob"`
	Extra   json.RawMessage   `json:"extra"`
	Labels  map[string]string `json:"labels"`
}

func normalizeSampleType() reflect.Type {
	return reflect.TypeOf(normalizeSample{})
}

func TestNormalizeArgsJSONDropsNullFields(t *testing.T) {
	raw := json.RawMessage(`{"pattern":"fib","depth":null,"glob":null,"verbose":null}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())

	var fields map[string]json.RawMessage
	if err := json.Unmarshal(out, &fields); err != nil {
		t.Fatalf("normalized output is invalid JSON: %v", err)
	}
	for _, key := range []string{"depth", "glob", "verbose"} {
		if _, ok := fields[key]; ok {
			t.Fatalf("null field %q should have been dropped, got %s", key, out)
		}
	}
	if string(fields["pattern"]) != `"fib"` {
		t.Fatalf("real value rewritten: %s", out)
	}
}

func TestNormalizeArgsJSONCoercesStringifiedScalars(t *testing.T) {
	raw := json.RawMessage(`{"depth":"2","limit":" 300 ","ratio":"0.5","verbose":"True"}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())

	var args normalizeSample
	if err := json.Unmarshal(out, &args); err != nil {
		t.Fatalf("normalized output does not decode: %v (%s)", err, out)
	}
	if args.Depth != 2 || args.Limit != 300 || args.Ratio != 0.5 || !args.Verbose {
		t.Fatalf("coercion wrong: %+v (%s)", args, out)
	}
}

func TestNormalizeArgsJSONLeavesUnparseableScalarsForStrictDecoder(t *testing.T) {
	raw := json.RawMessage(`{"depth":"two","verbose":"yes"}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())
	if string(out) != string(raw) {
		t.Fatalf("unparseable scalars should pass through untouched, got %s", out)
	}
}

func TestNormalizeArgsJSONWrapsLoneValuesIntoSlices(t *testing.T) {
	raw := json.RawMessage(`{"glob":"**/*.go","specs":{"count":"3","note":"x"}}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())

	var args normalizeSample
	if err := json.Unmarshal(out, &args); err != nil {
		t.Fatalf("normalized output does not decode: %v (%s)", err, out)
	}
	if len(args.Glob) != 1 || args.Glob[0] != "**/*.go" {
		t.Fatalf("lone glob string not wrapped: %+v", args.Glob)
	}
	if len(args.Specs) != 1 || args.Specs[0].Count != 3 || args.Specs[0].Note != "x" {
		t.Fatalf("lone spec object not wrapped and coerced: %+v (%s)", args.Specs, out)
	}
}

func TestNormalizeArgsJSONNormalizesSliceElements(t *testing.T) {
	raw := json.RawMessage(`{"specs":[{"count":"1"},{"count":2}]}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())

	var args normalizeSample
	if err := json.Unmarshal(out, &args); err != nil {
		t.Fatalf("normalized output does not decode: %v (%s)", err, out)
	}
	if args.Specs[0].Count != 1 || args.Specs[1].Count != 2 {
		t.Fatalf("element coercion wrong: %+v", args.Specs)
	}
}

func TestNormalizeArgsJSONLeavesByteSlicesAlone(t *testing.T) {
	raw := json.RawMessage(`{"blob":"aGVsbG8="}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())
	if string(out) != string(raw) {
		t.Fatalf("[]byte base64 string must not be wrapped, got %s", out)
	}
}

func TestNormalizeArgsJSONNeverRewritesStringFields(t *testing.T) {
	// "null" is a legitimate search pattern; only JSON null means absent.
	raw := json.RawMessage(`{"pattern":"null","note":"null"}`)
	// note is not a field of the sample; use pattern only.
	raw = json.RawMessage(`{"pattern":"null"}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())
	if string(out) != string(raw) {
		t.Fatalf("string field rewritten: %s", out)
	}
}

func TestNormalizeArgsJSONKeepsUnknownFieldsForStrictDecoder(t *testing.T) {
	raw := json.RawMessage(`{"pattern":"x","bogus":1}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())
	if !strings.Contains(string(out), `"bogus"`) {
		t.Fatalf("unknown field should pass through, got %s", out)
	}
}

// A hallucinated field with a null value must NOT be silently dropped:
// the silent drop removed the model's only corrective signal, and the
// tolerated shape kept replaying in history as a successful example the
// model imitated and escalated (sess_23234e5b6235ccceb04652b13cfbf732:
// max_output_tokens_note{,2,3,...}: null sailed through Prepare while
// the strict decoder's did-you-mean fixed non-null inventions in one
// shot). Unknown keys pass through — null included — so the strict
// decoder reports them with the valid-field list.
func TestNormalizeArgsJSONKeepsUnknownNullFieldsForStrictDecoder(t *testing.T) {
	raw := json.RawMessage(`{"pattern":"fib","depth":null,"bogus_note":null}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())

	var fields map[string]json.RawMessage
	if err := json.Unmarshal(out, &fields); err != nil {
		t.Fatalf("normalized output is invalid JSON: %v", err)
	}
	if _, ok := fields["depth"]; ok {
		t.Fatalf("known null field should have been dropped, got %s", out)
	}
	if value, ok := fields["bogus_note"]; !ok || string(value) != "null" {
		t.Fatalf("unknown null field should pass through untouched, got %s", out)
	}

	// And the strict decode then produces the corrective error.
	_, err := DecodeLenient[normalizeSample](raw)
	if err == nil {
		t.Fatal("DecodeLenient accepted an unknown null field, want the corrective error")
	}
	if !strings.Contains(err.Error(), `unknown field "bogus_note"`) || !strings.Contains(err.Error(), "Valid fields:") {
		t.Fatalf("error lacks the corrective guidance: %s", err.Error())
	}
}

func TestNormalizeArgsJSONUnchangedReturnsOriginalBytes(t *testing.T) {
	raw := json.RawMessage(`{"pattern":"fib","depth":2,"glob":["a","b"]}`)
	out := NormalizeArgsJSON(raw, normalizeSampleType())
	if string(out) != string(raw) {
		t.Fatalf("clean input should pass through byte-identical, got %s", out)
	}
}

func TestNormalizeArgsJSONToleratesNonObjectInput(t *testing.T) {
	for _, raw := range []string{`"text"`, `42`, `null`, `[1,2]`, `{bad`} {
		in := json.RawMessage(raw)
		if out := NormalizeArgsJSON(in, normalizeSampleType()); string(out) != string(in) {
			t.Fatalf("non-object input %s rewritten to %s", in, out)
		}
	}
}

func TestDecodeLenientAcceptsDeviationsStrictRejects(t *testing.T) {
	raw := json.RawMessage(`{"depth":"3","verbose":"true","glob":"*.go","limit":null}`)
	args, err := DecodeLenient[normalizeSample](raw)
	if err != nil {
		t.Fatalf("DecodeLenient rejected repairable input: %v", err)
	}
	if args.Depth != 3 || !args.Verbose || len(args.Glob) != 1 || args.Glob[0] != "*.go" || args.Limit != 0 {
		t.Fatalf("decoded args wrong: %+v", args)
	}
}

func TestDecodeLenientStillRejectsUnknownShapes(t *testing.T) {
	for _, raw := range []string{
		`{"pattern":"x","bogus":1}`,   // unknown field
		`{"depth":"two"}`,             // unparseable scalar coercion
		`{"pattern":42}`,              // wrong type for a string field
		`{"pattern":"x"} trailing {}`, // more than one JSON value
	} {
		if _, err := DecodeLenient[normalizeSample](json.RawMessage(raw)); err == nil {
			t.Fatalf("DecodeLenient accepted %s, want a strict-decode error", raw)
		}
	}
}

func TestDecodeStrictUnknownFieldSuggestsClosestMatch(t *testing.T) {
	// The cross-toolkit inheritance case from live transcripts: another
	// toolkit's max_output_tokens for our max_output_bytes.
	type runCmdLike struct {
		Command        string `json:"command"`
		MaxOutputBytes int    `json:"max_output_bytes"`
		TimeoutMs      int    `json:"timeout_ms"`
	}
	_, err := DecodeStrict[runCmdLike](json.RawMessage(`{"command":"ls","max_output_tokens":4096}`))
	if err == nil {
		t.Fatal("DecodeStrict accepted an unknown field")
	}
	msg := err.Error()
	if !strings.Contains(msg, `did you mean "max_output_bytes"?`) {
		t.Fatalf("error lacks the did-you-mean hint: %s", msg)
	}
	if !strings.Contains(msg, "Valid fields: command, max_output_bytes, timeout_ms") {
		t.Fatalf("error lacks the valid field list: %s", msg)
	}
}

func TestDecodeStrictUnknownFieldListsValidFieldsWithoutFuzzyMatch(t *testing.T) {
	// A far-off invented field gets the valid list but no did-you-mean.
	_, err := DecodeStrict[normalizeSample](json.RawMessage(`{"pattern":"x","pattern_placeholder":""}`))
	if err == nil {
		t.Fatal("DecodeStrict accepted an unknown field")
	}
	msg := err.Error()
	if strings.Contains(msg, "did you mean") {
		t.Fatalf("distant field must not produce a fuzzy hint: %s", msg)
	}
	if !strings.Contains(msg, "Valid fields:") || !strings.Contains(msg, "pattern") {
		t.Fatalf("error lacks the valid field list: %s", msg)
	}
}

func TestDecodeLenientCarriesUnknownFieldGuidance(t *testing.T) {
	_, err := DecodeLenient[normalizeSample](json.RawMessage(`{"pattern":"x","verbse":true}`))
	if err == nil {
		t.Fatal("DecodeLenient accepted an unknown field")
	}
	if !strings.Contains(err.Error(), `did you mean "verbose"?`) {
		t.Fatalf("DecodeLenient error lacks the did-you-mean hint: %s", err.Error())
	}
}

func TestLevenshtein(t *testing.T) {
	cases := []struct {
		a, b string
		want int
	}{
		{"", "", 0},
		{"abc", "abc", 0},
		{"max_output_tokens", "max_output_bytes", 4},
		{"pattern2", "pattern", 1},
		{"pattern_placeholder", "pattern", 12},
	}
	for _, tc := range cases {
		if got := levenshtein(tc.a, tc.b); got != tc.want {
			t.Fatalf("levenshtein(%q, %q) = %d, want %d", tc.a, tc.b, got, tc.want)
		}
	}
}

type normalizeEmbedded struct {
	normalizeInner
	Flag bool `json:"flag"`
}

func TestNormalizeArgsJSONPromotesEmbeddedStructFields(t *testing.T) {
	raw := json.RawMessage(`{"count":"7","flag":"TRUE"}`)
	out := NormalizeArgsJSON(raw, reflect.TypeOf(normalizeEmbedded{}))

	var args normalizeEmbedded
	if err := json.Unmarshal(out, &args); err != nil {
		t.Fatalf("normalized output does not decode: %v (%s)", err, out)
	}
	if args.Count != 7 || !args.Flag {
		t.Fatalf("embedded field coercion wrong: %+v (%s)", args, out)
	}
}

func TestOutputTokensToBytes(t *testing.T) {
	cases := []struct {
		name             string
		tokens, min, max int64
		want             int64
	}{
		{"negative clamps to the floor", -5, 1, 1 << 20, 1},
		{"zero below a positive floor", 0, 1, 1 << 20, 1},
		{"zero allowed by a zero floor", 0, 0, 65536, 0},
		{"the codex 1-token-to-4-bytes conversion", 1024, 1, 1 << 20, 4096},
		{"exact ceiling", 16384, 0, 65536, 65536},
		{"past the ceiling saturates", 16385, 0, 65536, 65536},
		{"huge input does not overflow", 1 << 62, 1, 1 << 20, 1 << 20},
	}
	for _, tc := range cases {
		if got := OutputTokensToBytes(tc.tokens, tc.min, tc.max); got != tc.want {
			t.Fatalf("OutputTokensToBytes(%d, %d, %d) = %d, want %d (%s)",
				tc.tokens, tc.min, tc.max, got, tc.want, tc.name)
		}
	}
}
