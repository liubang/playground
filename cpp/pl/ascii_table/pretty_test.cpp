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

#include "pretty.h"

int main(int argc, char* argv[]) {
    pl::pretty::Pretty p({"ID", "Name", "Age", "Desc"});
    p.add_row({"1", "zhangsan", "12", "this is test zhangsan description"});
    p.add_row({"2", "lisi", "29", "this is test lisi description"});
    p.add_row({"3", "wangwu", "30", "no desc"});
    p.add_sep("~");
    p.add_row({"", "", "", ""});
    p.render();
    return 0;
}
