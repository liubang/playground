package cgo

// void Demo3SayHello(const char* s);
import "C"

func Demo3_SayHello(str string) bool {
	C.Demo3SayHello(C.CString(str))
	return true
}
