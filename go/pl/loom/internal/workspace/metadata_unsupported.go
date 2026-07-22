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
// Created: 2026/07/22 21:10

//go:build !darwin && !linux

package workspace

import "fmt"

func readFileXAttrs(string) ([]xattrEntry, error) {
	return nil, fmt.Errorf("extended metadata preservation is unsupported on this platform")
}

func writeFileXAttr(string, string, []byte) error {
	return fmt.Errorf("extended metadata preservation is unsupported on this platform")
}

func ensureAtomicWriteACLPolicy(string) error {
	return fmt.Errorf("extended metadata preservation is unsupported on this platform")
}
