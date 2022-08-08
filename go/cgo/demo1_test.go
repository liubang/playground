package cgo

import (
	. "github.com/onsi/ginkgo/v2"
	// . "github.com/onsi/gomega"
)

var _ = Describe("demo1", Label("cgo"), func() {
	Describe("SayHello", func() {
		It("shoud print string", func() {
			Demo1_SayHello("hello world\n")
		})
	})
})
