package main

/*
struct A {
    int a;
    int b;
};
*/
import "C"
import "fmt"

func main() {
	var a C.struct_A
	a.a = 1
	a.b = 2
	fmt.Println(a.a)
	fmt.Println(a.b)
}
