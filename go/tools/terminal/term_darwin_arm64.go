//go:build darwin

package terminal

import "syscall"

// macOS uses TIOCGETA instead of TCGETS
const ioctlReadTermios = syscall.TIOCGETA
