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
// Created: 2023/11/26 22:05

pub fn parse_string(lit: &str) -> Result<(), String> {
    if lit.len() < 2 || !lit.starts_with('"') || !lit.ends_with('"') {
        return Err("invalid string literal".to_string());
    }
    println!("{}", &lit[1..lit.len() - 1]);
    println!("{}", lit);
    Ok(())
}

fn main() {
    let s = "\"this is test string\"";
    let _ = parse_string(s);
}
