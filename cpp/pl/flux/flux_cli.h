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

#include "cpp/pl/flux/runtime_env.h"
#include <iosfwd>
#include <optional>
#include <string>

namespace pl {

enum class FluxOutputFormat {
    Human,
    Csv,
    Json,
};

struct FluxCliOptions {
    bool quiet = false;
    bool list_results = false;
    bool install_builtins = true;
    FluxOutputFormat output_format = FluxOutputFormat::Human;
    bool table_borders = true;
    std::optional<std::string> result_name;
};

struct FluxAstOptions {
    bool json = false;
};

struct FluxCliResult {
    int exit_code = 0;
    std::string output;
    std::string error;
};

Environment MakeFluxCliEnvironment(const FluxCliOptions& options = {});
FluxCliResult ExecuteFluxSource(const std::string& source,
                                const std::string& name,
                                Environment& env,
                                const FluxCliOptions& options = {});
FluxCliResult DumpFluxAstSource(const std::string& source,
                                const std::string& name,
                                const FluxAstOptions& options = {});
int RunFluxRepl(std::istream& input,
                std::ostream& output,
                std::ostream& error,
                bool interactive,
                const FluxCliOptions& options = {});

} // namespace pl
