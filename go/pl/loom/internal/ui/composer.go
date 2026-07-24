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
// Created: 2026/07/23

package ui

// Composer-related logic is integrated into the main Model and view.
// This file exists to separate concerns for future expansion.

// MaxPasteBytes limits bracketed paste input to 1 MiB.
const MaxPasteBytes = 1024 * 1024

// ComposerHeight returns the appropriate composer height given available space.
func ComposerHeight(availableHeight int) int {
	if availableHeight < 4 {
		return 1
	}
	if availableHeight > 12 {
		return 8
	}
	return availableHeight - 4
}
