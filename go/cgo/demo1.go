package cgo

/*
#include <stdio.h>

static void Demo1SayHello(const char* s) {
	puts(s);
}
*/
import "C"

func Demo1_SayHello(str string) {
	C.Demo1SayHello(C.CString(str))
}
