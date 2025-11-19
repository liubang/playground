package string

import (
	"github.com/mattn/go-runewidth"
	"github.com/rivo/uniseg"
)

func StrPrintLenInTerm(text string) uint64 {
	gr := uniseg.NewGraphemes(text)
	var totalWidth uint64 = 0
	for gr.Next() {
		cluster := gr.Str()
		width := runewidth.StringWidth(cluster)
		totalWidth += uint64(width)
	}
	return totalWidth
}
