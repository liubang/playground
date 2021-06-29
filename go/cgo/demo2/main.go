package main

//void SayHello(const char* s);
import "C"

func main() {
	C.SayHello(C.CString("hello world"))
}
