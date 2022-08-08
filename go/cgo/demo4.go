package cgo

// void Demo4SayHello(const char* s);
import "C"

func Demo4_SayHello(str string) bool {
	C.Demo4SayHello(C.CString(str))
	return true
}
