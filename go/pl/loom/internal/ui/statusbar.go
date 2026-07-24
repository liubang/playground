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

import (
	"github.com/liubang/playground/go/pl/loom/internal/app"
)

// StatusLabel returns a human-readable state label.
func StatusLabel(state app.ControllerState) string {
	switch state {
	case app.ControllerStateIdle:
		return "idle"
	case app.ControllerStateRunning:
		return "running"
	case app.ControllerStateAwaitingApproval:
		return "approval"
	case app.ControllerStateCancelling:
		return "cancelling"
	case app.ControllerStateFatal:
		return "fatal"
	case app.ControllerStateBooting:
		return "starting"
	default:
		return string(state)
	}
}
