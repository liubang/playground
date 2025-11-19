//go:build linux

package terminal

import "syscall"

const ioctlReadTermios = syscall.TCGETS
