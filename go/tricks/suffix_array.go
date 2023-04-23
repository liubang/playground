package tricks

import (
	"index/suffixarray"
	"unsafe"
)

func lastSubstring(s string) string {
	sa := (*struct {
		_  []byte
		sa []int32
	})(unsafe.Pointer(suffixarray.New([]byte(s)))).sa
	return s[sa[len(s)-1]:]
}
