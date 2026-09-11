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
// Created: 2026/09/11

#pragma once

#include <string>

#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/media/image.h"

namespace pl::mllm::media {

// Decode an image file (PNG/JPEG/whatever the host platform codec supports)
// into the RGB f32 representation. macOS resolves through ImageIO
// (zero third-party deps); other platforms return kUnsupported until a
// portable decoder is wired in. Decoding lives here — not in image.h — so
// the core container stays platform- and dependency-free.
[[nodiscard]] Result<Image> LoadImageFile(const std::string& path);

} // namespace pl::mllm::media
