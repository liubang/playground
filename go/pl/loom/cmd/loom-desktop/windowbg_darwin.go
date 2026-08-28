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

// Sets the NSWindow backgroundColor to the web UI's dark-theme surface color
// (#1e2326, the SPA's --bg0). Wails' mac.Options does not expose window
// background tinting. During a live resize the system-frame step wins the
// race against the webview's next composite (cross-process layout + raster
// takes 30–100ms); the gap is filled with the window's backgroundColor — the
// default white, which reads as a bright streak against the dark page.
// Matching it makes the gap invisible.
//
// The Objective-C implementation lives in windowbg_darwin.m — cgo compiles
// the preamble as plain C, so any Obj-C syntax (dispatch_async, NSColor)
// must sit in a real .m source (the same layout wails uses internally).

package main

/*
#cgo LDFLAGS: -framework Cocoa

void loomSetWindowBackgroundColor(float r, float g, float b);
*/
import "C"

// applyWindowBackgroundColor schedules the window background tint on the
// Cocoa main queue; NSApp is live by the time Wails runs OnStartup.
func applyWindowBackgroundColor(r, g, b float64) {
	C.loomSetWindowBackgroundColor(C.float(r), C.float(g), C.float(b))
}
