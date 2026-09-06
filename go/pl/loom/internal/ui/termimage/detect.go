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

package termimage

import (
	"os"
	"strings"
)

// Protocol identifies a terminal's native image-display capability.
type Protocol int

const (
	// ProtocolNone means the terminal cannot display images inline; the UI
	// keeps its text placeholder for image blocks.
	ProtocolNone Protocol = iota
	// ProtocolKitty covers terminals implementing kitty's graphics protocol
	// including Unicode placeholders: kitty itself and ghostty. WezTerm is
	// deliberately excluded: its kitty-protocol support does not cover
	// virtual placements/placeholders, and a wrong positive renders as a
	// large blank area — far worse than the text fallback.
	ProtocolKitty
)

// Detect probes the environment once at startup: the terminal family is fixed
// for the process lifetime, so plain environment enumeration is enough (no
// async query round-trip needed). Anything unrecognized — or running under
// tmux/screen, where graphics passthrough needs extra setup — maps to none
// and simply keeps the textual placeholder. NO_COLOR also disables inline
// images: the kitty placeholder scheme encodes the image id in the cell's
// SGR foreground color, which is exactly the color output NO_COLOR forbids.
func Detect(getenv func(string) string) Protocol {
	if getenv("TMUX") != "" || getenv("NO_COLOR") != "" {
		return ProtocolNone
	}
	term := getenv("TERM")
	termProg := getenv("TERM_PROGRAM")
	switch {
	case getenv("KITTY_WINDOW_ID") != "" || strings.Contains(term, "kitty"):
		return ProtocolKitty
	case strings.Contains(term, "ghostty") || termProg == "ghostty":
		return ProtocolKitty
	default:
		return ProtocolNone
	}
}

// DetectEnv probes the real process environment.
func DetectEnv() Protocol {
	return Detect(os.Getenv)
}
