// Copyright (c) 2021 The Authors. All rights reserved.
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
// Created: 2021/11/12 17:34

package main

import "log"

type Foo struct {
	Name string
}

func main() {
	arr := make([]*Foo, 2)
	arr[0] = &Foo{Name: "aaa"}
	arr[1] = &Foo{Name: "bbb"}

	arr1 := make([]*Foo, 2)

	for idx, f := range arr {
		arr1[idx] = f
	}

	for _, f := range arr1 {
		log.Println(f.Name)
	}
}
