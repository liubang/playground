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

#include "cpp/pl/flux/runtime_env.h"

namespace pl {

Environment::Environment() = default;

Environment::Environment(std::shared_ptr<Environment> parent) : parent_(std::move(parent)) {}

void Environment::define(const std::string& name, const Value& value) { bindings_[name] = value; }

void Environment::define_option(const std::string& name, const Value& value) {
    options_[name] = value;
}

bool Environment::has_local(const std::string& name) const { return bindings_.contains(name); }

bool Environment::has_local_option(const std::string& name) const {
    return options_.contains(name);
}

absl::Status Environment::assign(const std::string& name, const Value& value) {
    Environment* owner = find_binding_owner(name);
    if (owner == nullptr) {
        return absl::NotFoundError("undefined variable: " + name);
    }
    owner->bindings_[name] = value;
    return absl::OkStatus();
}

absl::StatusOr<Value> Environment::lookup(const std::string& name) const {
    if (auto it = bindings_.find(name); it != bindings_.end()) {
        return it->second;
    }
    if (parent_ != nullptr) {
        return parent_->lookup(name);
    }
    return absl::NotFoundError("undefined variable: " + name);
}

absl::StatusOr<Value> Environment::lookup_option(const std::string& name) const {
    if (auto it = options_.find(name); it != options_.end()) {
        return it->second;
    }
    if (parent_ != nullptr) {
        return parent_->lookup_option(name);
    }
    return absl::NotFoundError("undefined option: " + name);
}

const Environment* Environment::find_binding_owner(const std::string& name) const {
    if (bindings_.contains(name)) {
        return this;
    }
    if (parent_ != nullptr) {
        return parent_->find_binding_owner(name);
    }
    return nullptr;
}

Environment* Environment::find_binding_owner(const std::string& name) {
    if (bindings_.contains(name)) {
        return this;
    }
    if (parent_ != nullptr) {
        return parent_->find_binding_owner(name);
    }
    return nullptr;
}

} // namespace pl
