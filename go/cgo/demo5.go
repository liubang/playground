package cgo

/*
struct A {
    int a;
    int b;
};
*/
import "C"

func Demo5_ReturnCstruct() *C.struct_A {
	var a C.struct_A
	a.a = 1
	a.b = 2
	return &a
}
