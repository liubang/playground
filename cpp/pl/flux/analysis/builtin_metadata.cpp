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
// Created: 2026/05/24 11:04

#include "cpp/pl/flux/analysis/builtin_metadata.h"

#include <algorithm>
#include <unordered_set>

namespace pl::flux::analysis {
namespace {

using Kind = BuiltinParamKind;

BuiltinParam Req(std::string name, std::string type = "dynamic") {
    return {.name = std::move(name), .type = std::move(type), .kind = Kind::Required};
}

BuiltinParam Opt(std::string name, std::string type = "dynamic", std::string def = "") {
    BuiltinParam param{.name = std::move(name), .type = std::move(type), .kind = Kind::Optional};
    if (!def.empty()) {
        param.default_value = std::move(def);
    }
    return param;
}

BuiltinParam Pipe(std::string name, std::string type = "stream[A]") {
    return {.name = std::move(name), .type = std::move(type), .kind = Kind::Pipe};
}

BuiltinSignature Sig(std::string package,
                     std::string name,
                     std::string return_type,
                     std::string summary,
                     std::vector<BuiltinParam> params,
                     bool implemented = true,
                     bool provider = false,
                     BuiltinExportKind kind = BuiltinExportKind::Function) {
    std::string fq_name = package.empty() ? name : package + "." + name;
    return {
        .package = std::move(package),
        .name = std::move(name),
        .fq_name = std::move(fq_name),
        .return_type = std::move(return_type),
        .summary = std::move(summary),
        .params = std::move(params),
        .implemented = implemented,
        .provider = provider,
        .kind = kind,
    };
}

BuiltinSignature Val(std::string package,
                     std::string name,
                     std::string return_type,
                     std::string summary,
                     bool implemented = true) {
    return Sig(std::move(package),
               std::move(name),
               std::move(return_type),
               std::move(summary),
               {},
               implemented,
               false,
               BuiltinExportKind::Value);
}

const std::vector<BuiltinSignature>& signatures() {
    static const std::vector<BuiltinSignature> sigs = {
        Sig("", "len", "int", "Return string, array, or object length.", {Req("v")}),
        Sig("", "string", "string", "Convert a value to Flux display string.", {Req("v")}),
        Sig("",
            "contains",
            "bool",
            "Check whether an array contains a value.",
            {Req("set", "[A]"), Req("value", "A")}),
        Sig("",
            "range",
            "stream[A]",
            "Filter rows by _time range.",
            {Pipe("tables"), Req("start", "time|duration"), Req("stop", "time|duration")}),
        Sig("",
            "filter",
            "stream[A]",
            "Filter rows by predicate.",
            {Pipe("tables"), Req("fn", "(r: A) => bool"), Opt("onEmpty", "string")}),
        Sig("",
            "map",
            "stream[B]",
            "Transform each row with an object mapping.",
            {Pipe("tables"), Req("fn", "(r: A) => B")}),
        Sig("",
            "keep",
            "stream[A]",
            "Keep selected columns.",
            {Pipe("tables"), Req("columns", "[string]")}),
        Sig("",
            "drop",
            "stream[A]",
            "Drop selected columns.",
            {Pipe("tables"), Req("columns", "[string]")}),
        Sig("",
            "rename",
            "stream[A]",
            "Rename columns.",
            {Pipe("tables"), Req("columns", "[string:string]")}),
        Sig("",
            "duplicate",
            "stream[A]",
            "Duplicate one column into another.",
            {Pipe("tables"), Req("column", "string"), Req("as", "string")}),
        Sig("",
            "set",
            "stream[A]",
            "Set a column to a fixed value.",
            {Pipe("tables"), Req("key", "string"), Req("value", "string")}),
        Sig("",
            "limit",
            "stream[A]",
            "Take the first n rows.",
            {Pipe("tables"), Req("n", "int"), Opt("offset", "int", "0")}),
        Sig("",
            "tail",
            "stream[A]",
            "Take the last n rows.",
            {Pipe("tables"), Req("n", "int"), Opt("offset", "int", "0")}),
        Sig("",
            "sort",
            "stream[A]",
            "Sort rows by columns.",
            {Pipe("tables"), Req("columns", "[string]"), Opt("desc", "bool", "false")}),
        Sig("",
            "group",
            "stream[A]",
            "Regroup rows by group key.",
            {Pipe("tables"), Opt("columns", "[string]"), Opt("mode", "string", "by")}),
        Sig("",
            "pivot",
            "stream[A]",
            "Pivot row/column values into wide rows.",
            {Pipe("tables"),
             Req("rowKey", "[string]"),
             Req("columnKey", "[string]"),
             Req("valueColumn", "string")}),
        Sig("",
            "fill",
            "stream[A]",
            "Fill null values.",
            {Pipe("tables"),
             Req("column", "string"),
             Opt("value"),
             Opt("usePrevious", "bool", "false")}),
        Sig("",
            "union",
            "stream[A]",
            "Union multiple table streams.",
            {Req("tables", "[stream[A]]")}),
        Sig("", "sum", "dynamic", "Sum numeric values.", {Opt("column", "string", "_value")}),
        Sig("", "mean", "float", "Calculate mean value.", {Opt("column", "string", "_value")}),
        Sig("", "min", "dynamic", "Find minimum value.", {Opt("column", "string", "_value")}),
        Sig("", "max", "dynamic", "Find maximum value.", {Opt("column", "string", "_value")}),
        Sig("", "count", "int", "Count rows.", {Pipe("tables"), Opt("column", "string")}),
        Sig("",
            "spread",
            "dynamic",
            "Calculate max minus min.",
            {Pipe("tables"), Opt("column", "string", "_value")}),
        Sig("",
            "quantile",
            "dynamic",
            "Calculate quantile.",
            {Pipe("tables"), Req("q", "float"), Opt("column", "string", "_value")}),
        Sig("",
            "median",
            "dynamic",
            "Calculate median.",
            {Pipe("tables"), Opt("column", "string", "_value")}),
        Sig("", "first", "stream[A]", "Select first row per table.", {Pipe("tables")}),
        Sig("", "last", "stream[A]", "Select last row per table.", {Pipe("tables")}),
        Sig("",
            "top",
            "stream[A]",
            "Select top n rows.",
            {Pipe("tables"), Req("n", "int"), Opt("columns", "[string]")}),
        Sig("",
            "bottom",
            "stream[A]",
            "Select bottom n rows.",
            {Pipe("tables"), Req("n", "int"), Opt("columns", "[string]")}),
        Sig("",
            "reduce",
            "stream[B]",
            "Fold rows into an object.",
            {Pipe("tables"), Req("identity", "B"), Req("fn", "(r: A, accumulator: B) => B")}),
        Sig("",
            "distinct",
            "stream[A]",
            "Return distinct column values.",
            {Pipe("tables"), Opt("column", "string", "_value")}),
        Sig("",
            "window",
            "stream[A]",
            "Window rows by time.",
            {Pipe("tables"),
             Req("every", "duration"),
             Opt("period", "duration"),
             Opt("offset", "duration"),
             Opt("createEmpty", "bool", "false"),
             Opt("location", "record")}),
        Sig("",
            "aggregateWindow",
            "stream[A]",
            "Window rows and aggregate each window.",
            {Pipe("tables"),
             Req("every", "duration"),
             Req("fn", "function"),
             Opt("column", "string", "_value"),
             Opt("timeSrc", "string", "_stop"),
             Opt("timeDst", "string", "_time"),
             Opt("createEmpty", "bool", "true")}),
        Sig("",
            "elapsed",
            "stream[A]",
            "Calculate elapsed time between rows.",
            {Pipe("tables"),
             Opt("unit", "duration", "1s"),
             Opt("timeColumn", "string", "_time"),
             Opt("columnName", "string", "elapsed")}),
        Sig("",
            "difference",
            "stream[A]",
            "Calculate row-to-row differences.",
            {Pipe("tables"),
             Opt("columns", "[string]"),
             Opt("column", "string", "_value"),
             Opt("nonNegative", "bool", "false"),
             Opt("keepFirst", "bool", "false")}),
        Sig("",
            "derivative",
            "stream[A]",
            "Calculate rate of change.",
            {Pipe("tables"),
             Opt("unit", "duration", "1s"),
             Opt("column", "string", "_value"),
             Opt("timeColumn", "string", "_time"),
             Opt("nonNegative", "bool", "false"),
             Opt("initialZero", "bool", "false")}),
        Sig("",
            "join",
            "stream[A]",
            "Join two or more streams.",
            {Req("tables", "record"), Req("on", "[string]|function"), Opt("method", "string")}),
        Sig("", "columns", "[string]", "List column names.", {Pipe("tables")}),
        Sig("", "keys", "[string]", "List group key columns.", {Pipe("tables")}),
        Sig("",
            "findColumn",
            "[A]",
            "Extract a column as an array.",
            {Pipe("tables"), Req("fn", "(key: record) => bool"), Req("column", "string")}),
        Sig("",
            "findRecord",
            "record",
            "Extract one record.",
            {Pipe("tables"), Req("fn", "(key: record) => bool"), Req("idx", "int")}),
        Sig("", "explain", "record", "Return the query plan explanation.", {Pipe("tables")}),
        Sig("",
            "yield",
            "stream[A]",
            "Name and emit a result.",
            {Pipe("tables"), Opt("name", "string", "_result")}),

        Sig("array",
            "from",
            "stream[A]",
            "Create a table stream from rows.",
            {Req("rows", "[record]")},
            true,
            true),
        Sig("array", "concat", "[A]", "Concatenate arrays.", {Req("arr", "[A]"), Req("v", "[A]")}),
        Sig("array",
            "filter",
            "[A]",
            "Filter array elements.",
            {Req("arr", "[A]"), Req("fn", "(x: A) => bool")}),
        Sig("array",
            "map",
            "[B]",
            "Map array elements.",
            {Req("arr", "[A]"), Req("fn", "(x: A) => B")}),
        Sig("array",
            "contains",
            "bool",
            "Check array membership.",
            {Req("arr", "[A]"), Req("value", "A")}),
        Sig("array",
            "reduce",
            "B",
            "Fold an array.",
            {Req("arr", "[A]"), Req("identity", "B"), Req("fn", "(x: A, accumulator: B) => B")}),
        Sig("array",
            "any",
            "bool",
            "True if any element matches.",
            {Req("arr", "[A]"), Req("fn", "(x: A) => bool")}),
        Sig("array",
            "all",
            "bool",
            "True if all elements match.",
            {Req("arr", "[A]"), Req("fn", "(x: A) => bool")}),
        Sig("array",
            "range",
            "[int]",
            "Generate an integer range.",
            {Req("start", "int"), Req("stop", "int"), Opt("step", "int", "1")}),
        Sig("array", "repeat", "[A]", "Repeat a value.", {Req("value", "A"), Req("n", "int")}),
        Sig("array", "length", "int", "Return array length.", {Req("arr", "[A]")}),
        Sig("array",
            "get",
            "A",
            "Safely get an array element.",
            {Req("arr", "[A]"), Req("index", "int"), Opt("default", "A")}),
        Sig("array",
            "slice",
            "[A]",
            "Slice an array.",
            {Req("arr", "[A]"), Opt("start", "int", "0"), Opt("end", "int")}),
        Sig("array",
            "sort",
            "[A]",
            "Sort scalar array.",
            {Req("arr", "[A]"), Opt("desc", "bool", "false")}),
        Sig("array",
            "flatMap",
            "[B]",
            "Map and flatten array elements.",
            {Req("arr", "[A]"), Req("fn", "(x: A) => [B]")}),
        Sig("array",
            "find",
            "A",
            "Return first matching element.",
            {Req("arr", "[A]"), Req("fn", "(x: A) => bool"), Opt("default", "A")}),
        Sig("array",
            "findIndex",
            "int",
            "Return first matching index.",
            {Req("arr", "[A]"), Req("fn", "(x: A) => bool")}),
        Sig("array", "take", "[A]", "Take first n elements.", {Req("arr", "[A]"), Req("n", "int")}),
        Sig("array", "drop", "[A]", "Drop first n elements.", {Req("arr", "[A]"), Req("n", "int")}),
        Sig("array", "reverse", "[A]", "Reverse an array.", {Req("arr", "[A]")}),
        Sig("array", "unique", "[A]", "Remove duplicate elements.", {Req("arr", "[A]")}),
        Sig("array",
            "unfold",
            "[A]",
            "Generate values from state.",
            {Req("seed"), Req("fn", "(state: A) => record"), Opt("limit", "int")}),
        Sig("array",
            "scan",
            "[B]",
            "Running fold over an array.",
            {Req("arr", "[A]"), Req("identity", "B"), Req("fn", "(x: A, accumulator: B) => B")}),
        Sig("array",
            "zip",
            "[record]",
            "Zip two arrays.",
            {Req("left", "[A]"), Req("right", "[B]")}),
        Sig("array", "enumerate", "[record]", "Add element indexes.", {Req("arr", "[A]")}),

        Sig("csv",
            "from",
            "stream[A]",
            "Parse CSV data into a table stream.",
            {Opt("csv", "string"), Opt("file", "string"), Opt("mode", "string", "raw")},
            true,
            true),
        Sig("sqlite",
            "from",
            "stream[A]",
            "Scan a SQLite table.",
            {Req("path", "string"), Req("table", "string")},
            true,
            true),
        Sig("mysql",
            "from",
            "stream[A]",
            "Scan a MySQL table.",
            {Opt("dsn", "string"),
             Opt("host", "string"),
             Opt("user", "string"),
             Opt("password", "string"),
             Opt("database", "string"),
             Req("table", "string"),
             Opt("port", "int", "3306")},
            true,
            true),

        Sig("date",
            "add",
            "time",
            "Add a duration to a time.",
            {Req("d", "duration"), Req("to", "time")}),
        Sig("date",
            "sub",
            "time",
            "Subtract a duration from a time.",
            {Req("d", "duration"), Req("from", "time")}),
        Sig("date",
            "truncate",
            "time",
            "Truncate a time.",
            {Req("t", "time"), Req("unit", "duration")}),
        Sig("date", "year", "int", "Extract year.", {Req("t", "time")}),
        Sig("date", "month", "int", "Extract month.", {Req("t", "time")}),
        Sig("date", "monthDay", "int", "Extract day of month.", {Req("t", "time")}),
        Sig("date", "weekDay", "int", "Extract day of week.", {Req("t", "time")}),
        Sig("date", "hour", "int", "Extract hour.", {Req("t", "time")}),
        Sig("date", "minute", "int", "Extract minute.", {Req("t", "time")}),
        Sig("date", "second", "int", "Extract second.", {Req("t", "time")}),
        Val("timezone", "utc", "record", "UTC timezone location."),
        Sig("timezone",
            "fixed",
            "record",
            "Create fixed-offset timezone.",
            {Req("offset", "duration")}),
        Sig("timezone", "location", "record", "Resolve named timezone.", {Req("name", "string")}),
        Sig("strings",
            "containsStr",
            "bool",
            "Check substring.",
            {Req("v", "string"), Req("substr", "string")}),
        Sig("strings",
            "hasPrefix",
            "bool",
            "Check prefix.",
            {Req("v", "string"), Req("prefix", "string")}),
        Sig("strings",
            "hasSuffix",
            "bool",
            "Check suffix.",
            {Req("v", "string"), Req("suffix", "string")}),
        Sig("strings",
            "joinStr",
            "string",
            "Join strings.",
            {Req("arr", "[string]"), Req("v", "string")}),
        Sig("strings",
            "replaceAll",
            "string",
            "Replace all occurrences.",
            {Req("v", "string"), Req("t", "string"), Req("u", "string")}),
        Sig("strings",
            "split",
            "[string]",
            "Split string.",
            {Req("v", "string"), Req("t", "string")}),
        Sig("strings", "toUpper", "string", "Uppercase string.", {Req("v", "string")}),
        Sig("strings", "toLower", "string", "Lowercase string.", {Req("v", "string")}),
        Sig("strings", "trimSpace", "string", "Trim whitespace.", {Req("v", "string")}),
        Sig("math", "abs", "float", "Absolute value.", {Req("x", "number")}),
        Sig("math", "ceil", "float", "Ceiling.", {Req("x", "number")}),
        Sig("math", "floor", "float", "Floor.", {Req("x", "number")}),
        Sig("math", "round", "float", "Round.", {Req("x", "number")}),
        Sig("math", "sqrt", "float", "Square root.", {Req("x", "number")}),
        Sig("math", "pow", "float", "Power.", {Req("x", "number"), Req("y", "number")}),
        Sig("regexp", "compile", "regexp", "Compile regex.", {Req("v", "string")}),
        Sig("regexp",
            "findString",
            "string",
            "Find first regex match.",
            {Req("r", "regexp"), Req("v", "string")}),
        Sig("regexp",
            "matchRegexpString",
            "bool",
            "Test regex match.",
            {Req("r", "regexp"), Req("v", "string")}),
        Sig("regexp", "quoteMeta", "string", "Escape regex metacharacters.", {Req("v", "string")}),
        Sig("dict",
            "fromList",
            "record",
            "Create a dictionary from pairs.",
            {Req("pairs", "[record]")}),
        Sig("dict",
            "get",
            "A",
            "Get a dictionary value.",
            {Req("dict", "record"), Req("key", "string"), Opt("default", "A")}),
        Sig("dict",
            "insert",
            "record",
            "Insert a dictionary value.",
            {Req("dict", "record"), Req("key", "string"), Req("value", "A")}),
        Sig("dict",
            "remove",
            "record",
            "Remove a dictionary key.",
            {Req("dict", "record"), Req("key", "string")}),
        Sig("join",
            "inner",
            "stream[A]",
            "Inner join streams.",
            {Req("left", "stream[A]"),
             Req("right", "stream[B]"),
             Req("on", "[string]|function"),
             Opt("as", "function")}),
        Sig("join",
            "left",
            "stream[A]",
            "Left join streams.",
            {Req("left", "stream[A]"),
             Req("right", "stream[B]"),
             Req("on", "[string]|function"),
             Opt("as", "function")}),
        Sig("join",
            "right",
            "stream[A]",
            "Right join streams.",
            {Req("left", "stream[A]"),
             Req("right", "stream[B]"),
             Req("on", "[string]|function"),
             Opt("as", "function")}),
        Sig("join",
            "full",
            "stream[A]",
            "Full join streams.",
            {Req("left", "stream[A]"),
             Req("right", "stream[B]"),
             Req("on", "[string]|function"),
             Opt("as", "function")}),
        Sig("json", "encode", "string", "Encode a value as JSON.", {Req("v")}),
        Sig("runtime", "version", "string", "Return Flux runtime version.", {}),
        Sig("system", "time", "time", "Return current UTC time.", {}),
        Sig("types", "isNumeric", "bool", "Check whether value is numeric.", {Req("v")}),
        Sig("types", "isType", "bool", "Check value type.", {Req("v"), Req("type", "string")}),
        Sig("types", "isString", "bool", "Check string value.", {Req("v")}),
        Sig("types", "isDuration", "bool", "Check duration value.", {Req("v")}),
        Sig("types", "isBool", "bool", "Check bool value.", {Req("v")}),
        Sig("types", "isInt", "bool", "Check int value.", {Req("v")}),
        Sig("types", "isUInt", "bool", "Check uint value.", {Req("v")}),
        Sig("types", "isFloat", "bool", "Check float value.", {Req("v")}),
        Sig("types", "isTime", "bool", "Check time value.", {Req("v")}),
        Sig("types", "isRegexp", "bool", "Check regexp value.", {Req("v")}),
    };
    return sigs;
}

} // namespace

const std::vector<BuiltinSignature>& AllBuiltinSignatures() {
    return signatures();
}

const BuiltinSignature* FindBuiltinSignature(std::string_view package, std::string_view name) {
    for (const auto& sig : signatures()) {
        if (sig.package == package && sig.name == name) {
            return &sig;
        }
    }
    return nullptr;
}

const BuiltinSignature* FindUniverseBuiltinSignature(std::string_view name) {
    return FindBuiltinSignature("", name);
}

std::vector<const BuiltinSignature*> BuiltinsForPackage(std::string_view package) {
    std::vector<const BuiltinSignature*> result;
    for (const auto& sig : signatures()) {
        if (sig.package == package) {
            result.push_back(&sig);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->name < rhs->name;
    });
    return result;
}

std::vector<std::string> KnownPackages() {
    std::unordered_set<std::string> unique;
    for (const auto& sig : signatures()) {
        if (!sig.package.empty()) {
            unique.insert(sig.package);
        }
    }
    std::vector<std::string> result(unique.begin(), unique.end());
    std::sort(result.begin(), result.end());
    return result;
}

bool IsKnownPackage(std::string_view package) {
    for (const auto& sig : signatures()) {
        if (sig.package == package) {
            return true;
        }
    }
    return false;
}

bool IsCallableBuiltin(const BuiltinSignature& sig) {
    return sig.kind == BuiltinExportKind::Function;
}

std::string SignatureLabel(const BuiltinSignature& sig) {
    if (!IsCallableBuiltin(sig)) {
        return sig.fq_name + ": " + sig.return_type;
    }
    std::string label = sig.fq_name + "(";
    bool first = true;
    for (const auto& param : sig.params) {
        if (!first) {
            label += ", ";
        }
        first = false;
        if (param.kind == Kind::Pipe) {
            label += "<-";
        } else if (param.kind == Kind::Optional) {
            label += "?";
        }
        label += param.name + ": " + param.type;
        if (param.default_value.has_value()) {
            label += " = " + *param.default_value;
        }
    }
    label += ") => " + sig.return_type;
    return label;
}

std::string SignatureDetail(const BuiltinSignature& sig) {
    return SignatureLabel(sig) + " - " + sig.summary;
}

std::vector<std::string> CompletionParams(const BuiltinSignature& sig) {
    std::vector<std::string> params;
    if (!IsCallableBuiltin(sig)) {
        return params;
    }
    for (const auto& param : sig.params) {
        if (param.kind == Kind::Pipe) {
            continue;
        }
        if (param.kind == Kind::Optional) {
            params.push_back("?" + param.name + ":");
        } else {
            params.push_back(param.name + ":");
        }
    }
    return params;
}

} // namespace pl::flux::analysis
