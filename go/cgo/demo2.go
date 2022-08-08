package cgo

// void Demo2SayHello(const char* str);
import "C"

func Demo2_SayHello(str string) bool {
	C.Demo2SayHello(C.CString(str))
	return true
}
