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
// Created: 2025/11/19 17:20

// This example demonstrates various combinations of the pretty.Table API.
// Run with: go run .
package main

import (
	"fmt"
	"os"

	p "github.com/liubang/playground/go/tools/pretty"
)

func main() {
	// ---------------------------------------------------------------
	// 1. Basic table: header + data rows
	//
	//   +===========================+
	//   | Name     | Age | Desc     |
	//   +===========================+
	//   | zhangsan | 12  | student  |
	//   | lisi     | 25  | engineer |
	//   | wangwu   | 30  | manager  |
	//   +===========================+
	// ---------------------------------------------------------------
	fmt.Println("=== Basic table ===")
	{
		var t p.Table
		t.Add("Name", "Age", "Desc").
			Add("zhangsan", "12", "student").
			Add("lisi", "25", "engineer").
			Add("wangwu", "30", "manager")
		t.Render()
	}

	// ---------------------------------------------------------------
	// 2. Table with separator lines
	//
	//   +===========================+
	//   | Name     | Age | Desc     |
	//   +===========================+
	//   | zhangsan | 12  | student  |
	//   +───────────────────────────+
	//   | lisi     | 25  | engineer |
	//   +---------------------------+
	//   | wangwu   | 30  | manager  |
	//   +===========================+
	// ---------------------------------------------------------------
	fmt.Println("\n=== With separators ===")
	{
		var t p.Table
		t.Add("Name", "Age", "Desc").
			Add("zhangsan", "12", "student").
			Add("─"). // auto-detected separator, fills full width
			Add("lisi", "25", "engineer").
			Add("---"). // another separator style (dashes)
			Add("wangwu", "30", "manager")
		t.Render()
	}

	// ---------------------------------------------------------------
	// 3. CJK characters — column widths are measured by display width
	//
	//   +=================================================+
	//   | 姓名     | 年龄 | 城市 | 备注                   |
	//   +=================================================+
	//   | 张三     | 25   | 北京 | 这是一条很长的备注信息 |
	//   | 李四     | 30   | 上海 | 短备注                 |
	//   +━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━+
	//   | 王五六七 | 28   | 深圳 | 中等长度备注           |
	//   +=================================================+
	// ---------------------------------------------------------------
	fmt.Println("\n=== CJK characters ===")
	{
		var t p.Table
		t.Add("姓名", "年龄", "城市", "备注").
			Add("张三", "25", "北京", "这是一条很长的备注信息").
			Add("李四", "30", "上海", "短备注").
			Add("━"). // heavy separator
			Add("王五六七", "28", "深圳", "中等长度备注")
		t.Render()
	}

	// ---------------------------------------------------------------
	// 4. Auto-expanding columns — Add() grows the table naturally
	//
	//   +======================+
	//   | Key    |       |     |
	//   +======================+
	//   | animal | cat   |     |
	//   | fruit  | apple | red |
	//   +======================+
	// ---------------------------------------------------------------
	fmt.Println("\n=== Auto-expanding columns ===")
	{
		var t p.Table
		t.Add("Key").                   // 1 col
			Add("animal", "cat").        // 2 cols → auto-expand
			Add("fruit", "apple", "red") // 3 cols → auto-expand
		t.Render()
	}

	// ---------------------------------------------------------------
	// 5. Empty cells — missing values are rendered as blank
	//
	//   +=======================+
	//   | Col A | Col B | Col C |
	//   +=======================+
	//   | a1    |       | c1    |
	//   |       | b2    |       |
	//   | a3    | b3    | c3    |
	//   +=======================+
	// ---------------------------------------------------------------
	fmt.Println("\n=== Empty cells ===")
	{
		var t p.Table
		t.Add("Col A", "Col B", "Col C").
			Add("a1", "", "c1").
			Add("", "b2", "").
			Add("a3", "b3", "c3")
		t.Render()
	}

	// ---------------------------------------------------------------
	// 6. String() — capture rendered output as a string
	//
	//   +===================================+
	//   | Method      | Result              |
	//   +===================================+
	//   | Render()    | writes to stdout    |
	//   | String()    | returns a string    |
	//   +-----------------------------------+
	//   | RenderTo(w) | writes to io.Writer |
	//   +===================================+
	// ---------------------------------------------------------------
	fmt.Println("\n=== String() ===")
	{
		var t p.Table
		t.Add("Method", "Result").
			Add("Render()", "writes to stdout").
			Add("String()", "returns a string").
			Add("---").
			Add("RenderTo(w)", "writes to io.Writer")
		fmt.Print(t.String())
	}

	// ---------------------------------------------------------------
	// 7. RenderTo — write to any io.Writer (here: stderr)
	//
	//   +--------+-------+
	//   | Status | Count |
	//   +--------+-------+
	//   | OK     | 42    |
	//   | ERR    | 3     |
	//   +--------+-------+
	// ---------------------------------------------------------------
	fmt.Println("\n=== RenderTo ===")
	{
		var t p.Table
		t.Add("Status", "Count").
			Add("OK", "42").
			Add("ERR", "3")
		t.RenderTo(os.Stderr)
	}

	// ---------------------------------------------------------------
	// 8. Single-column table — separators are skipped
	//
	//   +========+
	//   | Fruits |
	//   +========+
	//   | apple  |
	//   | banana |
	//   | cherry |
	//   +========+
	// ---------------------------------------------------------------
	fmt.Println("\n=== Single column ===")
	{
		var t p.Table
		t.Add("Fruits").
			Add("apple").
			Add("banana").
			Add("cherry")
		t.Render()
	}

	// ---------------------------------------------------------------
	// 9. Mixed: empty cells + separators
	//
	//   +=====================================+
	//   | Name    | Phone | Email             |
	//   +=====================================+
	//   | Alice   | 1234  | alice@example.com |
	//   +─────────────────────────────────────+
	//   | Bob     |       | bob@example.com   |
	//   | Charlie | 5678  |                   |
	//   +=====================================+
	// ---------------------------------------------------------------
	fmt.Println("\n=== Mixed empty cells + separators ===")
	{
		var t p.Table
		t.Add("Name", "Phone", "Email").
			Add("Alice", "1234", "alice@example.com").
			Add("─").
			Add("Bob", "", "bob@example.com").
			Add("Charlie", "5678", "")
		t.Render()
	}
}
