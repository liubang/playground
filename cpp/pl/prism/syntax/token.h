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
// Created: 2026/09/13 13:24

#pragma once

#include <cstdint>
#include <string_view>

namespace pl::prism::syntax {

enum class TokenType : uint16_t {
    kEof = 0,
    kIllegal,

    // Identifiers and literals.
    kIdentifier,
    kQuotedIdentifier,
    kString,
    kUnicodeString,
    kBinaryString,
    kNumber,

    // Punctuation and operators.
    kLParen,
    kRParen,
    kLBracket,
    kRBracket,
    kComma,
    kSemicolon,
    kDot,
    kColon,
    kDoubleColon,
    kPlus,
    kMinus,
    kStar,
    kSlash,
    kPercent,
    kEq,
    kNeq,
    kLt,
    kGt,
    kLte,
    kGte,
    kConcat,
    kArrow,
    kFatArrow,

    // Reserved keywords (Trino 476). Non-reserved keywords are emitted as
    // kIdentifier and disambiguated by the parser.
    kKwAll,
    kKwAlter,
    kKwAnalyze,
    kKwAnd,
    kKwArray,
    kKwAs,
    kKwBetween,
    kKwBy,
    kKwCase,
    kKwCast,
    kKwConstraint,
    kKwCreate,
    kKwCross,
    kKwCube,
    kKwCurrent,
    kKwCurrentCatalog,
    kKwCurrentDate,
    kKwCurrentPath,
    kKwCurrentRole,
    kKwCurrentSchema,
    kKwCurrentTime,
    kKwCurrentTimestamp,
    kKwCurrentUser,
    kKwDeallocate,
    kKwDelete,
    kKwDescribe,
    kKwDistinct,
    kKwDrop,
    kKwElse,
    kKwEnd,
    kKwEscape,
    kKwExcept,
    kKwExecute,
    kKwExists,
    kKwExplain,
    kKwExtract,
    kKwFalse,
    kKwFor,
    kKwFrom,
    kKwFull,
    kKwGroup,
    kKwGrouping,
    kKwHaving,
    kKwIn,
    kKwInner,
    kKwInsert,
    kKwIntersect,
    kKwInterval,
    kKwInto,
    kKwIs,
    kKwJoin,
    kKwJsonArray,
    kKwJsonExists,
    kKwJsonObject,
    kKwJsonQuery,
    kKwJsonTable,
    kKwJsonValue,
    kKwLeft,
    kKwLike,
    kKwLimit,
    kKwListagg,
    kKwLocaltime,
    kKwLocaltimestamp,
    kKwNatural,
    kKwNormalize,
    kKwNot,
    kKwNull,
    kKwNullif,
    kKwOn,
    kKwOr,
    kKwOrder,
    kKwOuter,
    kKwPrepare,
    kKwRecursive,
    kKwRight,
    kKwRollup,
    kKwSelect,
    kKwSkip,
    kKwTable,
    kKwThen,
    kKwTo,
    kKwTrim,
    kKwTrue,
    kKwUescape,
    kKwUnion,
    kKwUnnest,
    kKwUsing,
    kKwValues,
    kKwWhen,
    kKwWhere,
    kKwWith,
};

// A zero-copy token: it only references [offset, offset + length) of the
// source text. The source must outlive any token produced from it.
struct Token {
    TokenType type = TokenType::kIllegal;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t line = 1;   // 1-based
    uint32_t column = 1; // 1-based

    [[nodiscard]] std::string_view text(std::string_view source) const {
        return source.substr(offset, length);
    }
};

} // namespace pl::prism::syntax
