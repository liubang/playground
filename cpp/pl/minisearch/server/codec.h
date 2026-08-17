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
// Created: 2026/08/17

#pragma once

#include <string>
#include <vector>

#include "cpp/pl/minisearch/core/collection.h"
#include "cpp/pl/minisearch/core/schema.h"
#include "cpp/pl/minisearch/proto/minisearch.pb.h"

namespace pl::minisearch::server {

// proto <-> core conversions shared by the HTTP handlers and the checkpoint
// store (breaks the server <-> storage dependency cycle).

bool ToCoreSchema(const proto::CollectionSpec& spec, core::Schema* out, std::string* error);

proto::CollectionSpec ToProtoSpec(const core::Schema& schema, const std::string& name);

// include_internal: checkpoint 持久化需要 internal_docid；API 响应置 false
// 避免泄漏内部实现细节。
void ToProtoDocument(const core::Document& doc, proto::Document* out, bool include_internal);

bool ToCoreDocument(const proto::Document& doc, core::Document* out, std::string* error);

} // namespace pl::minisearch::server
