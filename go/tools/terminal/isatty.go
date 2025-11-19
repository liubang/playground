package terminal

import (
	"syscall"
	"unsafe"
)

// Isatty returns true if the file descriptor is a terminal.
func Isatty(fd uintptr) bool {
	var termios Termios
	_, _, err := syscall.Syscall6(
		ioctlSyscall,
		fd,
		uintptr(ioctlReadTermios),
		uintptr(unsafe.Pointer(&termios)),
		0, 0, 0,
	)
	return err == 0
}
