// Copyright (c) 2023 The Authors. All rights reserved.
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

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/flux/runtime_value.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace pl {

class Environment {
public:
    Environment();
    explicit Environment(std::shared_ptr<Environment> parent);

    void define(const std::string& name, const Value& value);
    void define_option(const std::string& name, const Value& value);

    [[nodiscard]] bool has_local(const std::string& name) const;
    [[nodiscard]] bool has_local_option(const std::string& name) const;

    [[nodiscard]] absl::Status assign(const std::string& name, const Value& value);
    [[nodiscard]] absl::StatusOr<Value> lookup(const std::string& name) const;
    [[nodiscard]] absl::StatusOr<Value> lookup_option(const std::string& name) const;

    [[nodiscard]] std::shared_ptr<Environment> parent() const { return parent_; }

private:
    [[nodiscard]] const Environment* find_binding_owner(const std::string& name) const;
    Environment* find_binding_owner(const std::string& name);

    std::shared_ptr<Environment> parent_;
    std::unordered_map<std::string, Value> bindings_;
    std::unordered_map<std::string, Value> options_;
};

} // namespace pl
