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

// Native macOS dialogs via AppleScript (Standard Additions). These run
// before (or outside) the Wails GUI loop, where Cocoa's main-thread rules
// make NSAlert/NSOpenPanel awkward; AppleScript dialogs need no event loop
// and behave identically for Finder- and terminal-launched processes.

package main

import (
	"errors"
	"fmt"
	"os/exec"
	"strings"
)

// errPickCancelled marks a user-cancelled dialog (osascript exits non-zero
// with "User canceled." on stderr).
var errPickCancelled = errors.New("dialog cancelled by user")

// appleScriptString quotes s as an AppleScript string literal.
func appleScriptString(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, `"`, `\"`)
	return `"` + s + `"`
}

// showAlert displays a modal alert dialog. Best effort: stderr remains the
// fallback channel, so delivery failures are ignored.
func showAlert(title, message string) {
	script := "display dialog " + appleScriptString(message) +
		" with title " + appleScriptString(title) +
		` with icon caution buttons {"OK"} default button 1`
	_ = exec.Command("osascript", "-e", script).Run()
}

// chooseFolder prompts the user to pick a directory. Returns
// errPickCancelled when the user cancels; other errors mean AppleScript is
// unavailable and the caller should fall back to a default.
func chooseFolder(prompt string) (string, error) {
	if _, err := exec.LookPath("osascript"); err != nil {
		return "", err
	}
	script := "POSIX path of (choose folder with prompt " + appleScriptString(prompt) + ")"
	out, err := exec.Command("osascript", "-e", script).Output()
	if err != nil {
		// Distinguish a real cancel from an infrastructure failure (e.g.
		// no window-server access over SSH): the latter must reach the
		// caller's fallback instead of looking like a deliberate cancel.
		// Output() captures the failing command's stderr into ExitError.
		var exitErr *exec.ExitError
		if errors.As(err, &exitErr) && !strings.Contains(string(exitErr.Stderr), "User canceled") {
			return "", fmt.Errorf("osascript: %w: %s", err, strings.TrimSpace(string(exitErr.Stderr)))
		}
		return "", errPickCancelled
	}
	p := strings.TrimSpace(string(out))
	if p == "" {
		return "", errPickCancelled
	}
	if p != "/" {
		p = strings.TrimRight(p, "/")
	}
	return p, nil
}
