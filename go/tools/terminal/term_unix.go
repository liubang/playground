//go:build darwin || linux || freebsd

package terminal

import "syscall"

// Termios is a platform-independent alias.
type Termios = syscall.Termios

// On Unix systems SYS_IOCTL is available.
const ioctlSyscall = syscall.SYS_IOCTL
