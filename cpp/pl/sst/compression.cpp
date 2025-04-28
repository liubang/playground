// Copyright (c) 2025 The Authors. All rights reserved.
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

#include "cpp/pl/sst/compression.h"
#include "snappy.h"
#include <isa-l/crc.h>
#include <zstd.h>

namespace pl {

Result<Void> SnappyCompressionAdapter::compress(std::string_view input, std::string* output) {
    // TODO(liubang): implement
    RETURN_VOID;
}

Result<Void> SnappyCompressionAdapter::uncompress(std::string_view input, std::string* output) {
    size_t ulen;
    if (!snappy::GetUncompressedLength(input.data(), input.size(), &ulen)) {
        return makeError(StatusCode::kDataCorruption);
    }
    auto ubuf = std::make_unique<char[]>(ulen);
    if (!snappy::RawUncompress(input.data(), input.size(), ubuf.get())) {
        return makeError(StatusCode::kDataCorruption);
    }
    output->assign(ubuf.release(), ulen);

    RETURN_VOID;
}

} // namespace pl
