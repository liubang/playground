// Copyright (c) 2022 The Authors. All rights reserved.
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
// Created: 2022/08/14 21:55

use rand::Rng;
use std::{cmp::Ordering, io};

fn main() {
    println!("猜数游戏！");
    let secret_number = rand::thread_rng().gen_range(1..100);
    println!("神秘数字是：");

    let mut guess = String::new();
    io::stdin().read_line(&mut guess).expect("无法读取行");
    let guess: i32 = guess.trim().parse().expect("");

    match guess.cmp(&secret_number) {
        Ordering::Less => println!("小了"),
        Ordering::Greater => println!("大了"),
        Ordering::Equal => println!("你赢了"),
    }
}
