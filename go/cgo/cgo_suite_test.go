package cgo_test

import (
	"testing"

	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
)

func TestCgo(t *testing.T) {
	RegisterFailHandler(Fail)
	RunSpecs(t, "Cgo Suite")
}
