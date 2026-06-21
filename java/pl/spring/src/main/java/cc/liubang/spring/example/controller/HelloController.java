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
// Created: 2026/05/16 21:43

package cc.liubang.spring.example.controller;

import cc.liubang.spring.example.dto.ApiResponse;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import java.time.LocalDateTime;
import java.util.Map;

@RestController
@RequestMapping("/api")
public class HelloController {

    @GetMapping("/hello")
    public ApiResponse<Map<String, Object>> hello() {
        return ApiResponse.success(Map.of(
            "message", "Hello, Spring Boot with Bazel!",
            "timestamp", LocalDateTime.now().toString()
        ));
    }

    @GetMapping("/hello/{name}")
    public ApiResponse<Map<String, Object>> helloName(@PathVariable String name) {
        return ApiResponse.success(Map.of(
            "message", String.format("Hello, %s!", name),
            "timestamp", LocalDateTime.now().toString()
        ));
    }
}
