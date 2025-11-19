package main

import (
	p "github.com/playground/go/tools/pretty"
)

func main() {
	pretty := p.NewPretty([]string{"Name", "Age", "Desc"})
	pretty.Next().AddStr("zhangsan").AddStr("12").AddStr("test")
	pretty.Next().AddStr("zhangsan").AddStr("12").AddStr("test")
	pretty.Next().AddStr("zhangsan").AddStr("12").AddStr("test")
	pretty.Next().AddSep("-").AddSep("-").AddSep("-")
	pretty.Next().AddStr("zhangsan").AddStr("12").AddStr("test")
	pretty.Next().AddStr("zhangsan").AddStr("12").AddStr("test")
	pretty.Next().AddStr("zhangsan").AddStr("12").AddStr("test")
	pretty.Render()
}
