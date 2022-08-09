package tricks

import (
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
)

var _ = Describe("t1", Label("tricks"), func() {
	Describe("SliceTricks1", func() {
		It("should returns 100", func() {
			Ω(SliceTricks1()).Should(Equal(100))
		})
	})
})
