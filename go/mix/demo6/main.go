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
// Created: 2021/09/27 19:36

package main

import (
	"encoding/json"
	"log"
)

type Foo struct {
	Name string `json:"name"`
}

type Bar struct {
	Foo *Foo `json:"foo"`
}

func main() {
	f := Bar{Foo: &Foo{Name: "hello"}}

	j, err := json.Marshal(f)
	if err != nil {
		log.Println(err)
	} else {
		log.Println(string(j))
	}

	{
		s := `{"foo": {"name": "hello"}}`

		var b Bar
		err = json.Unmarshal([]byte(s), &b)
		if err != nil {
			log.Println(err)
		} else {
			log.Println(b.Foo.Name)
		}
	}

	{
		s := `{}`

		var b Bar
		err = json.Unmarshal([]byte(s), &b)
		if err != nil {
			log.Println(err)
		} else {
			log.Println(b.Foo.Name)
		}
	}
}
