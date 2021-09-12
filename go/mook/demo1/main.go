package main

import (
	"fmt"
	"log"
	"reflect"
	"syscall"
	"unsafe"
)

func sum(a, b int) int {
	return a + b
}

func mook_sum(a, b int) int {
	log.Println("mook_sum run")
	return a - b
}

func patch(target, replacement interface{}) {
	from := reflect.ValueOf(target).Pointer()
	to := reflect.ValueOf(replacement).Pointer()

	data := jmp(to)
	copyTo(from, data)
}

func jmp(to uintptr) []byte {
	return []byte{
		0x48, 0xBA,
		byte(to),
		byte(to >> 8),
		byte(to >> 16),
		byte(to >> 24),
		byte(to >> 32),
		byte(to >> 40),
		byte(to >> 48),
		byte(to >> 56), // movabs rdx,to
		0xFF, 0xE2,     // jmp rdx
	}
}

func copyTo(location uintptr, data []byte) {
	f := rawMemoryAccess(location, len(data))

	mprotectCrossPage(location, len(data), syscall.PROT_READ|syscall.PROT_EXEC|syscall.PROT_WRITE)
	copy(f, data[:])
	mprotectCrossPage(location, len(data), syscall.PROT_READ|syscall.PROT_EXEC)
}

func rawMemoryAccess(p uintptr, length int) []byte {
	return *(*[]byte)(unsafe.Pointer(&reflect.SliceHeader{
		Data: p,
		Len:  length,
		Cap:  length,
	}))
}

func mprotectCrossPage(addr uintptr, length int, prot int) {
	pageSize := syscall.Getpagesize()
	for p := pageStart(addr); p < addr+uintptr(length); p += uintptr(pageSize) {
		page := rawMemoryAccess(p, pageSize)
		if err := syscall.Mprotect(page, prot); err != nil {
			panic(err)
		}
	}
}

func pageStart(ptr uintptr) uintptr {
	return ptr & ^(uintptr(syscall.Getpagesize() - 1))
}

func main() {
	patch(sum, mook_sum)
	fmt.Println(sum(1, 3))
}
