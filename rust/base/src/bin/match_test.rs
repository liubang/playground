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
// Created: 2022/08/09 11:27

enum Direction {
    East,
    West,
    North,
    South,
}

fn demo1() {
    let dire = Direction::South;
    match dire {
        Direction::East => println!("East"),
        Direction::North | Direction::South => {
            println!("South or North");
        }
        _ => println!("West"),
    };
}

enum IpAddr {
    Ipv4,
    Ipv6,
}

fn demo2() {
    let ip1 = IpAddr::Ipv6;
    let ip_str = match ip1 {
        IpAddr::Ipv4 => "127.0.0.1",
        _ => "::1",
    };
    println!("ip_str: {}", ip_str)
}

#[derive()]
enum FooBar {
    Foo,
    Bar,
}

fn demo3() {
    let v = vec![FooBar::Foo, FooBar::Bar, FooBar::Foo];
    // 这里匹配了所有的结果
    let mut m = v.iter().filter(|x| matches!(x, FooBar::Foo));
    loop {
        match m.next() {
            None => break,
            Some(_el) => {
                println!("OK")
            }
        }
    }
}

fn main() {
    demo1();
    demo2();
    demo3();
}
