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
