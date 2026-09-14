#!/usr/bin/env python3
# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Authors: liubang (it.liubang@gmail.com)
# Created: 2026/09/13 15:52

"""Extracts candidate SQL statements from Trino's TestSqlParser.java.

Collects Java string literals and text blocks (merging '+'-concatenated
runs), decodes escapes, normalizes whitespace, deduplicates, and prints
one candidate SQL entry per line on stdout. Pipe the output through the
corpus_classify binary to sort entries into statements (S), expressions
(E) and rejects (F):

  extract_golden_corpus.py TestSqlParser.java | corpus_classify
"""

import re
import sys


def extract_literals(text):
    """Yields decoded contents of Java string literals and text blocks.

    Adjacent literals joined by '+' are merged into a single entry.
    """
    i, n = 0, len(text)
    current = None  # accumulator for a concatenated run
    while i < n:
        c = text[i]
        # Comments
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            i = n if end == -1 else end + 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end == -1 else end + 2
            continue
        # Char literal (not a string; skip)
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == '"':
            text_block = text.startswith('"""', i)
            j = i + (3 if text_block else 1)
            buf = []
            while j < n:
                if text_block:
                    if text.startswith('"""', j):
                        j += 3
                        break
                elif text[j] == '"':
                    j += 1
                    break
                if text[j] == "\\" and j + 1 < n:
                    buf.append(decode_escape(text, j))
                    j += escape_length(text, j)
                    continue
                buf.append(text[j])
                j += 1
            piece = "".join(buf)
            # Merge runs of  "a" + "b" + ...  into one logical string.
            k = j
            merged = piece
            while True:
                m = re.match(r"\s*\+\s*", text[k:])
                if not m:
                    break
                k2 = k + m.end()
                if k2 < n and text[k2] == '"':
                    sub = extract_concat_piece(text, k2)
                    if sub is None:
                        break
                    merged += sub[0]
                    k = sub[1]
                else:
                    break
            if current is not None:
                yield current
            current = merged
            i = k
            continue
        i += 1
    if current is not None:
        yield current


def extract_concat_piece(text, i):
    """Extracts a single literal starting at text[i] == '"'."""
    text_block = text.startswith('"""', i)
    j = i + (3 if text_block else 1)
    buf = []
    n = len(text)
    while j < n:
        if text_block:
            if text.startswith('"""', j):
                return "".join(buf), j + 3
        elif text[j] == '"':
            return "".join(buf), j + 1
        if text[j] == "\\" and j + 1 < n:
            buf.append(decode_escape(text, j))
            j += escape_length(text, j)
            continue
        buf.append(text[j])
        j += 1
    return None


def escape_length(text, i):
    c = text[i + 1]
    if c == "u":
        return 6
    if c in "01234567":
        m = re.match(r"\\[0-7]{1,3}", text[i:])
        return m.end() if m else 2
    return 2


def decode_escape(text, i):
    c = text[i + 1]
    simple = {
        "n": "\n",
        "t": "\t",
        "r": "\r",
        "b": "\b",
        "f": "\f",
        '"': '"',
        "'": "'",
        "\\": "\\",
    }
    if c in simple:
        return simple[c]
    if c == "u":
        try:
            return chr(int(text[i + 2 : i + 6], 16))
        except ValueError:
            return text[i : i + 6]
    if c in "01234567":
        m = re.match(r"\\([0-7]{1,3})", text[i:])
        return chr(int(m.group(1), 8)) if m else c
    return c


def looks_like_sql(s):
    if not s or len(s) < 2 or len(s) > 4000:
        return False
    lowered = s.lower()
    # Error message templates and prose, not SQL.
    if "%s" in s or "mismatched input" in lowered or lowered.startswith("line "):
        return False
    # Expected-error messages captured from assertInvalid* message arguments.
    if (
        lowered.startswith(("invalid ", "incomplete ", "unexpected "))
        or " not supported" in lowered
        or " not valid" in lowered
        or " must have" in lowered
        or " must contain" in lowered
        or " not allowed" in lowered
        or " did you mean" in lowered
    ):
        return False
    # Non-SQL literals from expected-AST construction (identifier strings like
    # order"2); real SQL containing quoted names always has a SQL keyword.
    if '"' in s and not re.search(
        r"\b(select|from|where|values|table|join|cast|case|insert|update|delete|"
        r"create|alter|drop|set|grant|revoke|deny|analyze|refresh|comment|"
        r"prepare|execute|describe|show|use|reset|start|commit|rollback|call|"
        r"explain|truncate|deallocate|role|schema|catalog|view|session|path)\b",
        lowered,
    ):
        return False
    if re.match(r"^[a-z ]+[.?!]?$", lowered) and not re.search(
        r"\b(select|from|where|values|table|join|cast|case|and|or|not|null|in|"
        r"is|like|between|as|by|on|over|row|array|interval|date|time|exists)\b",
        lowered,
    ):
        return False
    return True


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} TestSqlParser.java")
    # Some tests use \uXXXX escapes that decode to lone surrogates; they are
    # irrelevant for corpus purposes, so tolerate them on output.
    sys.stdout.reconfigure(errors="replace")
    with open(sys.argv[1], encoding="utf-8") as f:
        text = f.read()
    seen = set()
    for literal in extract_literals(text):
        normalized = " ".join(literal.split())
        if looks_like_sql(normalized) and normalized not in seen:
            seen.add(normalized)
            print(normalized)


if __name__ == "__main__":
    main()
