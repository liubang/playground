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

#include "cpp/pl/minisearch/server/codec.h"

#include "cpp/pl/minisearch/analysis/analyzer.h"

namespace pl::minisearch::server {

namespace {

core::FieldType parse_type(const std::string& type, bool* ok) {
    *ok = true;
    if (type == "text")
        return core::FieldType::kText;
    if (type == "keyword")
        return core::FieldType::kKeyword;
    if (type == "numeric")
        return core::FieldType::kNumeric;
    if (type == "vector")
        return core::FieldType::kVector;
    *ok = false;
    return core::FieldType::kKeyword;
}

// Copies a repeated float field into std::vector<float>. Element-wise indexed
// access: the range constructor over protobuf iterators trips libc++ container
// annotations under ASan.
std::vector<float> copy_floats(const google::protobuf::RepeatedField<float>& field) {
    std::vector<float> out;
    out.reserve(field.size());
    for (int i = 0; i < field.size(); ++i) {
        out.push_back(field.Get(i));
    }
    return out;
}

} // namespace

bool ToCoreSchema(const proto::CollectionSpec& spec, core::Schema* out, std::string* error) {
    if (!analysis::IsKnownAnalyzer(spec.default_analyzer())) {
        *error = "unknown default analyzer: " + spec.default_analyzer();
        return false;
    }
    out->default_analyzer = spec.default_analyzer();
    for (const auto& f : spec.fields()) {
        bool ok = false;
        core::FieldDef def;
        def.name = f.name();
        def.type = parse_type(f.type(), &ok);
        if (!ok) {
            *error = "unknown field type: " + f.type();
            return false;
        }
        if (!f.analyzer().empty() && !analysis::IsKnownAnalyzer(f.analyzer())) {
            *error = "unknown analyzer for field " + f.name() + ": " + f.analyzer();
            return false;
        }
        def.indexed = f.indexed();
        def.stored = f.stored();
        def.analyzer = f.analyzer();
        def.dims = f.dims();
        def.metric = f.metric().empty() ? "cosine" : f.metric();
        def.source_field = f.source_field();
        def.server_embedded = f.mode() == "server";
        if (f.mode() != "server" && f.mode() != "client" && !f.mode().empty()) {
            *error = "vector mode must be server|client: " + f.mode();
            return false;
        }
        // 重复字段名必须报错，不能静默覆盖
        if (out->fields.count(def.name) > 0) {
            *error = "duplicate field name: " + def.name;
            return false;
        }
        out->fields[def.name] = def;
    }
    if (auto err = out->Validate(); err.has_value()) {
        *error = *err;
        return false;
    }
    return true;
}

proto::CollectionSpec ToProtoSpec(const core::Schema& schema, const std::string& name) {
    proto::CollectionSpec spec;
    spec.set_name(name);
    spec.set_default_analyzer(schema.default_analyzer);
    for (const auto& [field_name, def] : schema.fields) {
        proto::FieldDef* f = spec.add_fields();
        f->set_name(def.name);
        switch (def.type) {
            case core::FieldType::kText:
                f->set_type("text");
                break;
            case core::FieldType::kKeyword:
                f->set_type("keyword");
                break;
            case core::FieldType::kNumeric:
                f->set_type("numeric");
                break;
            case core::FieldType::kVector:
                f->set_type("vector");
                break;
        }
        f->set_indexed(def.indexed);
        f->set_stored(def.stored);
        f->set_analyzer(def.analyzer);
        f->set_dims(def.dims);
        f->set_metric(def.metric);
        f->set_source_field(def.source_field);
        if (def.type == core::FieldType::kVector) {
            f->set_mode(def.server_embedded ? "server" : "client");
        }
    }
    return spec;
}

void ToProtoDocument(const core::Document& doc, proto::Document* out, bool include_internal) {
    out->set_id(doc.id);
    out->set_version(doc.version);
    if (include_internal) {
        out->set_internal_docid(doc.internal_docid);
    }
    for (const auto& [name, value] : doc.fields) {
        proto::FieldValue& field = (*out->mutable_fields())[name];
        if (const auto* s = std::get_if<std::string>(&value)) {
            field.set_s(*s);
        } else if (const auto* n = std::get_if<double>(&value)) {
            field.set_n(*n);
        } else if (const auto* v = std::get_if<std::vector<float>>(&value)) {
            field.mutable_v()->mutable_data()->Assign(v->begin(), v->end());
        }
    }
}

bool ToCoreDocument(const proto::Document& doc, core::Document* out, std::string* error) {
    out->id = doc.id();
    out->version = doc.version();
    out->internal_docid = doc.internal_docid();
    for (const auto& [name, value] : doc.fields()) {
        switch (value.kind_case()) {
            case proto::FieldValue::kS:
                out->fields[name] = value.s();
                break;
            case proto::FieldValue::kN:
                out->fields[name] = value.n();
                break;
            case proto::FieldValue::kV:
                out->fields[name] = copy_floats(value.v().data());
                break;
            case proto::FieldValue::KIND_NOT_SET:
                *error = "field value without kind: " + name;
                return false;
        }
    }
    return true;
}

} // namespace pl::minisearch::server
