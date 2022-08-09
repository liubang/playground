package tricks_test

import (
	"testing"

	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
)

func TestTricks(t *testing.T) {
	RegisterFailHandler(Fail)
	RunSpecs(t, "Tricks Suite")
}
