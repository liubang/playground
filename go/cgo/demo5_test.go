package cgo

import (
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
)

var _ = Describe("demo5", Label("cgo"), func() {
	Describe("Demo5_ReturnCstruct", func() {
		It("should return a c struct", func() {
			s := Demo5_ReturnCstruct()
			Expect(int(s.a)).Should(Equal(1))
			Expect(int(s.b)).Should(Equal(2))
		})
	})
})
