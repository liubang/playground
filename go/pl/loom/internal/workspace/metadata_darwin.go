// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/07/22 21:10

//go:build darwin

package workspace

import (
	"encoding/binary"
	"errors"
	"fmt"
	"syscall"
	"unsafe"
)

const (
	darwinXattrNoFollow            = 0x0001
	attrBitMapCount                = 5
	attrCMNExtendedSecurity        = 0x00400000
	kauthFilesecMagic       uint32 = 0x012cc16d
	kauthFilesecNoACL       uint32 = ^uint32(0)
	darwinACLProbeBufSize          = 8192
)

type darwinAttrList struct {
	bitmapCount uint16
	reserved    uint16
	commonAttr  uint32
	volAttr     uint32
	dirAttr     uint32
	fileAttr    uint32
	forkAttr    uint32
}

type darwinAttrReference struct {
	Offset int32
	Length uint32
}

func readFileXAttrs(path string) ([]xattrEntry, error) {
	names, err := listDarwinXAttrs(path)
	if err != nil {
		return nil, err
	}
	entries := make([]xattrEntry, 0, len(names))
	for _, name := range names {
		value, err := getDarwinXAttr(path, name)
		if err != nil {
			return nil, fmt.Errorf("read xattr %q: %w", name, err)
		}
		entries = append(entries, xattrEntry{name: name, value: value})
	}
	return entries, nil
}

func writeFileXAttr(path, name string, value []byte) error {
	if err := setDarwinXAttr(path, name, value); err != nil {
		return fmt.Errorf("set xattr %q: %w", name, err)
	}
	return nil
}

func ensureAtomicWriteACLPolicy(path string) error {
	hasACL, err := hasNonTrivialDarwinACL(path)
	if err != nil {
		return err
	}
	if hasACL {
		return fmt.Errorf("non-trivial ACL metadata is not supported for atomic overwrite")
	}
	return nil
}

func listDarwinXAttrs(path string) ([]string, error) {
	size, err := darwinListXAttr(path, nil)
	if err != nil {
		if errors.Is(err, syscall.ENOATTR) {
			return nil, nil
		}
		return nil, err
	}
	if size == 0 {
		return nil, nil
	}
	buf := make([]byte, size)
	size, err = darwinListXAttr(path, buf)
	if err != nil {
		return nil, err
	}
	buf = buf[:size]
	var names []string
	start := 0
	for idx, b := range buf {
		if b != 0 {
			continue
		}
		if idx > start {
			names = append(names, string(buf[start:idx]))
		}
		start = idx + 1
	}
	return names, nil
}

func getDarwinXAttr(path, name string) ([]byte, error) {
	size, err := darwinGetXAttr(path, name, nil)
	if err != nil {
		return nil, err
	}
	if size == 0 {
		return []byte{}, nil
	}
	buf := make([]byte, size)
	size, err = darwinGetXAttr(path, name, buf)
	if err != nil {
		return nil, err
	}
	return append([]byte(nil), buf[:size]...), nil
}

func hasNonTrivialDarwinACL(path string) (bool, error) {
	pathPtr, err := syscall.BytePtrFromString(path)
	if err != nil {
		return false, err
	}
	attrList := darwinAttrList{
		bitmapCount: attrBitMapCount,
		commonAttr:  attrCMNExtendedSecurity,
	}
	buf := make([]byte, darwinACLProbeBufSize)
	_, _, errno := syscall.Syscall6(
		syscall.SYS_GETATTRLIST,
		uintptr(unsafe.Pointer(pathPtr)),
		uintptr(unsafe.Pointer(&attrList)),
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(len(buf)),
		darwinXattrNoFollow,
		0,
	)
	if errno != 0 {
		if errno == syscall.ENOATTR {
			return false, nil
		}
		return false, errno
	}
	if len(buf) < 12 {
		return false, fmt.Errorf("extended security probe returned short buffer")
	}
	returnedSize := int(binary.LittleEndian.Uint32(buf[:4]))
	if returnedSize < 12 || returnedSize > len(buf) {
		return false, fmt.Errorf("extended security probe returned invalid size %d", returnedSize)
	}
	ref := darwinAttrReference{
		Offset: int32(binary.LittleEndian.Uint32(buf[4:8])),
		Length: binary.LittleEndian.Uint32(buf[8:12]),
	}
	if ref.Length == 0 {
		return false, nil
	}
	dataStart := 12 + int(ref.Offset)
	dataEnd := dataStart + int(ref.Length)
	if dataStart < 12 || dataEnd > returnedSize || dataEnd < dataStart {
		return false, fmt.Errorf("extended security probe returned invalid attrreference")
	}
	if ref.Length < 44 {
		return false, fmt.Errorf("extended security payload too short")
	}
	payload := buf[dataStart:dataEnd]
	if binary.LittleEndian.Uint32(payload[:4]) != kauthFilesecMagic {
		return false, fmt.Errorf("extended security payload magic mismatch")
	}
	entryCount := binary.LittleEndian.Uint32(payload[40:44])
	return entryCount != kauthFilesecNoACL, nil
}

func darwinListXAttr(path string, dest []byte) (int, error) {
	pathPtr, err := syscall.BytePtrFromString(path)
	if err != nil {
		return 0, err
	}
	var destPtr uintptr
	if len(dest) > 0 {
		destPtr = uintptr(unsafe.Pointer(&dest[0]))
	}
	r0, _, errno := syscall.Syscall6(
		syscall.SYS_LISTXATTR,
		uintptr(unsafe.Pointer(pathPtr)),
		destPtr,
		uintptr(len(dest)),
		darwinXattrNoFollow,
		0,
		0,
	)
	if errno != 0 {
		return 0, errno
	}
	return int(r0), nil
}

func darwinGetXAttr(path, name string, dest []byte) (int, error) {
	pathPtr, err := syscall.BytePtrFromString(path)
	if err != nil {
		return 0, err
	}
	namePtr, err := syscall.BytePtrFromString(name)
	if err != nil {
		return 0, err
	}
	var destPtr uintptr
	if len(dest) > 0 {
		destPtr = uintptr(unsafe.Pointer(&dest[0]))
	}
	r0, _, errno := syscall.Syscall6(
		syscall.SYS_GETXATTR,
		uintptr(unsafe.Pointer(pathPtr)),
		uintptr(unsafe.Pointer(namePtr)),
		destPtr,
		uintptr(len(dest)),
		0,
		darwinXattrNoFollow,
	)
	if errno != 0 {
		return 0, errno
	}
	return int(r0), nil
}

func setDarwinXAttr(path, name string, value []byte) error {
	pathPtr, err := syscall.BytePtrFromString(path)
	if err != nil {
		return err
	}
	namePtr, err := syscall.BytePtrFromString(name)
	if err != nil {
		return err
	}
	var valuePtr uintptr
	if len(value) > 0 {
		valuePtr = uintptr(unsafe.Pointer(&value[0]))
	}
	_, _, errno := syscall.Syscall6(
		syscall.SYS_SETXATTR,
		uintptr(unsafe.Pointer(pathPtr)),
		uintptr(unsafe.Pointer(namePtr)),
		valuePtr,
		uintptr(len(value)),
		0,
		darwinXattrNoFollow,
	)
	if errno != 0 {
		return errno
	}
	return nil
}
