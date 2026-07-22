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

//go:build linux

package workspace

import (
	"fmt"
	"syscall"
)

const (
	linuxACLAccessXAttr  = "system.posix_acl_access"
	linuxACLDefaultXAttr = "system.posix_acl_default"
)

func readFileXAttrs(path string) ([]xattrEntry, error) {
	names, err := listLinuxXAttrs(path)
	if err != nil {
		return nil, err
	}
	entries := make([]xattrEntry, 0, len(names))
	for _, name := range names {
		value, err := getLinuxXAttr(path, name)
		if err != nil {
			return nil, fmt.Errorf("read xattr %q: %w", name, err)
		}
		entries = append(entries, xattrEntry{name: name, value: value})
	}
	return entries, nil
}

func writeFileXAttr(path, name string, value []byte) error {
	if err := syscall.Setxattr(path, name, value, 0); err != nil {
		return fmt.Errorf("set xattr %q: %w", name, err)
	}
	return nil
}

func ensureAtomicWriteACLPolicy(path string) error {
	names, err := listLinuxXAttrs(path)
	if err != nil {
		return err
	}
	for _, name := range names {
		if name == linuxACLAccessXAttr || name == linuxACLDefaultXAttr {
			return fmt.Errorf("non-trivial ACL metadata is not supported for atomic overwrite")
		}
	}
	return nil
}

func listLinuxXAttrs(path string) ([]string, error) {
	size, err := syscall.Listxattr(path, nil)
	if err != nil {
		return nil, err
	}
	if size == 0 {
		return nil, nil
	}
	buf := make([]byte, size)
	size, err = syscall.Listxattr(path, buf)
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

func getLinuxXAttr(path, name string) ([]byte, error) {
	size, err := syscall.Getxattr(path, name, nil)
	if err != nil {
		return nil, err
	}
	if size == 0 {
		return []byte{}, nil
	}
	buf := make([]byte, size)
	size, err = syscall.Getxattr(path, name, buf)
	if err != nil {
		return nil, err
	}
	return append([]byte(nil), buf[:size]...), nil
}
