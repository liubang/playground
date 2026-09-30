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
	"bytes"
	"encoding/json"
	"fmt"
	"reflect"
	"sort"
	"strconv"
	"strings"
)

// DecodeLenient decodes model-supplied tool arguments the way DecodeStrict
// does, after repairing the argument-shape deviations models make in the
// wild (see NormalizeArgsJSON). Use it at the model boundary (Prepare);
// canonical arguments produced by Prepare itself need only DecodeStrict.
func DecodeLenient[T any](raw json.RawMessage) (T, error) {
	var sample T
	return DecodeStrict[T](NormalizeArgsJSON(raw, reflect.TypeOf(&sample).Elem()))
}

// NormalizeArgsJSON repairs the argument-shape deviations models make in
// the wild before the strict decode, replacing the per-tool normalizers
// (the former per-tool point fixes) with one type-driven mechanism:
//
//   - known object fields whose value is JSON null are dropped: models
//     mirror the schema with explicit nulls for optional properties they
//     have no value for, and null means absent in every tool schema.
//     Unknown fields are NEVER dropped, even when null: a silently
//     deleted hallucinated field (e.g. max_output_tokens_note: null)
//     gives the model zero corrective feedback, and the tolerated shape
//     keeps getting replayed in history as a successful example the
//     model imitates and escalates (sess_23234e5b6235ccceb04652b13cfbf732).
//     Unknown fields pass through so the strict decoder reports them
//     with the valid-field list and a did-you-mean;
//   - a stringified scalar ("1", "true") is coerced when the target field
//     is numeric or boolean and the parse is lossless;
//   - a lone value where the target field is a slice is wrapped into a
//     one-element array (some models inherit scalar-typed fields from
//     other toolkits).
//
// String fields are never rewritten — the literal "null" is a legitimate
// search pattern — and everything the walker does not recognize passes
// through untouched, so the strict decoder still reports genuinely
// unknown shapes.
func NormalizeArgsJSON(raw json.RawMessage, target reflect.Type) json.RawMessage {
	if len(raw) == 0 || target == nil {
		return raw
	}
	out, changed := normalizeJSONValue(raw, target)
	if !changed {
		return raw
	}
	return out
}

// normalizeJSONValue walks raw guided by typ, returning the repaired JSON
// and whether anything changed. Unrecognized shapes pass through with
// changed=false so callers can keep the original bytes.
func normalizeJSONValue(raw json.RawMessage, typ reflect.Type) (json.RawMessage, bool) {
	for typ.Kind() == reflect.Pointer {
		typ = typ.Elem()
	}
	switch typ.Kind() {
	case reflect.Struct:
		return normalizeJSONObject(raw, typ)
	case reflect.Bool:
		text, ok := jsonStringValue(raw)
		if !ok {
			return raw, false
		}
		switch strings.ToLower(strings.TrimSpace(text)) {
		case "true":
			return json.RawMessage("true"), true
		case "false":
			return json.RawMessage("false"), true
		}
		return raw, false
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		text, ok := jsonStringValue(raw)
		if !ok {
			return raw, false
		}
		n, err := strconv.ParseInt(strings.TrimSpace(text), 10, typ.Bits())
		if err != nil {
			return raw, false
		}
		return json.RawMessage(strconv.FormatInt(n, 10)), true
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		text, ok := jsonStringValue(raw)
		if !ok {
			return raw, false
		}
		n, err := strconv.ParseUint(strings.TrimSpace(text), 10, typ.Bits())
		if err != nil {
			return raw, false
		}
		return json.RawMessage(strconv.FormatUint(n, 10)), true
	case reflect.Float32, reflect.Float64:
		text, ok := jsonStringValue(raw)
		if !ok {
			return raw, false
		}
		n, err := strconv.ParseFloat(strings.TrimSpace(text), typ.Bits())
		if err != nil {
			return raw, false
		}
		return json.RawMessage(strconv.FormatFloat(n, 'g', -1, typ.Bits())), true
	case reflect.Slice, reflect.Array:
		trimmed := bytes.TrimSpace(raw)
		if len(trimmed) == 0 || isJSONNullRaw(trimmed) {
			return raw, false
		}
		if trimmed[0] == '[' {
			var elems []json.RawMessage
			if err := json.Unmarshal(raw, &elems); err != nil {
				return raw, false
			}
			changed := false
			for i, elem := range elems {
				if out, c := normalizeJSONValue(elem, typ.Elem()); c {
					elems[i] = out
					changed = true
				}
			}
			if !changed {
				return raw, false
			}
			out, err := json.Marshal(elems)
			if err != nil {
				return raw, false
			}
			return out, true
		}
		// A lone value where a slice is expected: wrap it, except for
		// []byte whose JSON form IS a base64 string.
		if typ.Elem().Kind() == reflect.Uint8 {
			return raw, false
		}
		inner, _ := normalizeJSONValue(raw, typ.Elem())
		wrapped, err := json.Marshal([]json.RawMessage{inner})
		if err != nil {
			return raw, false
		}
		return wrapped, true
	default:
		// Strings, maps, interfaces, json.RawMessage: left untouched.
		return raw, false
	}
}

// normalizeJSONObject drops null-valued KNOWN fields and repairs the
// remaining ones whose names map to a struct field. Unknown keys pass
// through untouched — null included — so the strict decoder owns
// reporting them: silently dropping a hallucinated field removes the
// model's only corrective signal (see NormalizeArgsJSON).
func normalizeJSONObject(raw json.RawMessage, typ reflect.Type) (json.RawMessage, bool) {
	trimmed := bytes.TrimSpace(raw)
	if len(trimmed) == 0 || trimmed[0] != '{' {
		return raw, false
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(raw, &fields); err != nil {
		return raw, false
	}
	fieldTypes := structJSONFieldTypes(typ)
	changed := false
	for key, value := range fields {
		ft, ok := fieldTypes[key]
		if !ok {
			continue
		}
		if isJSONNullRaw(value) {
			delete(fields, key)
			changed = true
			continue
		}
		if out, c := normalizeJSONValue(value, ft); c {
			fields[key] = out
			changed = true
		}
	}
	if !changed {
		return raw, false
	}
	out, err := json.Marshal(fields)
	if err != nil {
		return raw, false
	}
	return out, true
}

// structJSONFieldTypes maps each JSON property name of typ to its field
// type, promoting anonymous embedded structs the way encoding/json does.
func structJSONFieldTypes(typ reflect.Type) map[string]reflect.Type {
	out := make(map[string]reflect.Type, typ.NumField())
	for i := 0; i < typ.NumField(); i++ {
		f := typ.Field(i)
		if f.Anonymous {
			ft := f.Type
			for ft.Kind() == reflect.Pointer {
				ft = ft.Elem()
			}
			if ft.Kind() == reflect.Struct {
				for name, embedded := range structJSONFieldTypes(ft) {
					if _, exists := out[name]; !exists {
						out[name] = embedded
					}
				}
				continue
			}
		}
		if !f.IsExported() {
			continue
		}
		name := f.Name
		if tag, ok := f.Tag.Lookup("json"); ok {
			name, _, _ = strings.Cut(tag, ",")
			if name == "-" {
				continue
			}
		}
		if name == "" {
			name = f.Name
		}
		out[name] = f.Type
	}
	return out
}

// unknownFieldName extracts the offending property from a
// DisallowUnknownFields decode error ("json: unknown field \"name\"").
func unknownFieldName(err error) (string, bool) {
	const prefix = `json: unknown field "`
	msg := err.Error()
	i := strings.Index(msg, prefix)
	if i < 0 {
		return "", false
	}
	rest := msg[i+len(prefix):]
	j := strings.IndexByte(rest, '"')
	if j <= 0 {
		return "", false
	}
	return rest[:j], true
}

// unknownFieldGuidance renders the recovery hint appended to the strict
// decode error when the model invents a property: the valid field list,
// so the model stops guessing, plus a did-you-mean when one valid name
// is close enough. Cross-toolkit field inheritance (another toolkit's
// max_output_tokens for our max_output_bytes) is the common case observed
// in live transcripts — without the hint models burn several round-trips
// retrying the identical shape, even when their own reasoning already
// names the right field.
func unknownFieldGuidance(name string, typ reflect.Type) string {
	for typ.Kind() == reflect.Pointer {
		typ = typ.Elem()
	}
	if typ.Kind() != reflect.Struct {
		return ""
	}
	fields := structJSONFieldTypes(typ)
	if len(fields) == 0 {
		return ""
	}
	names := make([]string, 0, len(fields))
	for n := range fields {
		names = append(names, n)
	}
	sort.Strings(names)
	var b strings.Builder
	fmt.Fprintf(&b, "; unknown field %q", name)
	if best, dist := closestString(name, names); dist <= 6 && dist*2 <= len(best) {
		fmt.Fprintf(&b, " — did you mean %q?", best)
	}
	b.WriteString(". Valid fields: ")
	b.WriteString(strings.Join(names, ", "))
	return b.String()
}

// closestString returns the candidate with the smallest Levenshtein
// distance to target.
func closestString(target string, candidates []string) (string, int) {
	best, bestDist := "", len(target)+1
	for _, c := range candidates {
		if d := levenshtein(target, c); d < bestDist {
			best, bestDist = c, d
		}
	}
	return best, bestDist
}

// levenshtein computes the rune-aware edit distance between a and b.
func levenshtein(a, b string) int {
	ra, rb := []rune(a), []rune(b)
	prev := make([]int, len(rb)+1)
	for j := range prev {
		prev[j] = j
	}
	for i, ca := range ra {
		cur := make([]int, len(rb)+1)
		cur[0] = i + 1
		for j, cb := range rb {
			cost := 1
			if ca == cb {
				cost = 0
			}
			cur[j+1] = min(cur[j]+1, min(prev[j+1]+1, prev[j]+cost))
		}
		prev = cur
	}
	return prev[len(rb)]
}

// jsonStringValue reports whether raw is a JSON string and returns its
// decoded contents.
func jsonStringValue(raw json.RawMessage) (string, bool) {
	var text string
	if err := json.Unmarshal(raw, &text); err != nil {
		return "", false
	}
	return text, true
}

// isJSONNullRaw reports whether raw is the JSON null literal.
func isJSONNullRaw(raw json.RawMessage) bool {
	return bytes.Equal(bytes.TrimSpace(raw), []byte("null"))
}

// ApproxBytesPerToken converts a Codex-style max_output_tokens alias value
// into a byte budget: 1 token ~ 4 bytes, the same fixed approximation
// OpenAI Codex applies internally to its shell tool's output budget — no
// tokenizer is involved on either side.
const ApproxBytesPerToken int64 = 4

// OutputTokensToBytes folds a max_output_tokens alias value into a byte
// budget clamped to [minBytes, maxBytes]. The multiply saturates: a tokens
// value at or beyond the ceiling maps to maxBytes, and anything below the
// floor (including negative input) maps to minBytes.
func OutputTokensToBytes(tokens, minBytes, maxBytes int64) int64 {
	if tokens < 0 {
		return minBytes
	}
	if tokens > maxBytes/ApproxBytesPerToken {
		return maxBytes
	}
	bytes := tokens * ApproxBytesPerToken
	if bytes < minBytes {
		return minBytes
	}
	return bytes
}
