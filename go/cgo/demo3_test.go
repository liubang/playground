package cgo

import (
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
)

var _ = Describe("demo3", Label("cgo"), func() {
	Describe("SayHello", func() {
		It("should print string and return true", func() {
			Expect(Demo3_SayHello("hello world")).Should(BeTrue())
		})
	})
})
