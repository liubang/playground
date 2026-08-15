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
// Created: 2026/08/15

package imagegen

import (
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// baseTool embeds the shared toolkit.BaseTool skeleton (definition +
// signer + prepare/verify protocol, REVIEW R3); imagegen has no
// tool-specific dependencies beyond it.
type baseTool struct {
	toolkit.BaseTool
}

func newBaseTool(def domain.ToolDefinition) (baseTool, error) {
	bt, err := toolkit.NewBaseTool(def)
	if err != nil {
		return baseTool{}, err
	}
	return baseTool{BaseTool: bt}, nil
}
