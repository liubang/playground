package pretty

import (
	"fmt"
	"os"

	s "github.com/playground/go/tools/string"
	"github.com/playground/go/tools/terminal"
	log "github.com/sirupsen/logrus"
)

type CellType int64

const (
	CT_STRING CellType = 0
	CT_SEP    CellType = 9
)

type PrettyCell struct {
	val string
	t   CellType
}

type Pretty struct {
	lines            [][]*PrettyCell
	cell_max_length  []int
	maxcell_per_line int
	cell_idx         int
	show_sep_flag    bool
}

func NewPretty(headers []string) *Pretty {
	maxcell_per_line := len(headers)
	pretty := &Pretty{
		lines:            make([][]*PrettyCell, 0),
		cell_max_length:  make([]int, maxcell_per_line),
		maxcell_per_line: maxcell_per_line,
		cell_idx:         0,
		show_sep_flag:    terminal.Isatty(os.Stdout.Fd()),
	}
	return pretty.Next().AddStrs(headers)
}

func (p *Pretty) Reset(headers []string) *Pretty {
	maxcell_per_line := len(headers)
	p.maxcell_per_line = maxcell_per_line
	p.cell_max_length = make([]int, maxcell_per_line)
	p.lines = make([][]*PrettyCell, 0)
	p.cell_idx = 0
	return p.Next().AddStrs(headers)
}

func (p *Pretty) AddRow(vals []string) *Pretty {
	p.Next()
	p.AddStrs(vals)
	return p
}

func (p *Pretty) AddStrs(vals []string) *Pretty {
	for _, val := range vals {
		p.AddStr(val)
	}
	return p
}

func (p *Pretty) AddStr(val string) *Pretty {
	return p.AddCell(val, CT_STRING)
}

func (p *Pretty) AddSep(val string) *Pretty {
	return p.AddCell(val, CT_SEP)
}

func (p *Pretty) AddCell(val string, t CellType) *Pretty {
	lines := len(p.lines)
	cells := p.lines[lines-1]
	if p.cell_idx > p.maxcell_per_line {
		log.Fatalf("reach max cell limit per line")
	}
	strlen := int(s.StrPrintLenInTerm(val))
	if p.cell_max_length[p.cell_idx] < strlen {
		p.cell_max_length[p.cell_idx] = strlen
	}
	cells[p.cell_idx] = &PrettyCell{
		val: val,
		t:   t,
	}
	p.cell_idx++
	return p
}

func (p *Pretty) Next() *Pretty {
	p.cell_idx = 0
	p.lines = append(p.lines, make([]*PrettyCell, p.maxcell_per_line))
	return p
}

func (p *Pretty) padRight(str string, length int, pad rune) string {
	for int(s.StrPrintLenInTerm(str)) < length {
		str += string(pad)
	}
	return str
}

func (p *Pretty) printHeaderLine(len int, ch rune) {
	line := "+"
	line += p.padRight("", len-2, ch)
	line += "+"
	fmt.Fprintln(os.Stdout, line)
}

func (p *Pretty) printHeader(len int, ch rune) {
	p.printHeaderLine(len, ch)
	header := p.lines[0]
	p.printLine(header)
	p.printHeaderLine(len, ch)
}

func (p *Pretty) printLine(cells []*PrettyCell) {
	line := ""
	if p.show_sep_flag {
		line += "| "
	} else {
		line += " "
	}
	for idx, cell := range cells {
		if cell == nil {
			line += p.padRight("", p.cell_max_length[idx], ' ')
		} else if cell.t == CT_SEP {
			line += p.padRight(cell.val, p.cell_max_length[idx], rune(cell.val[0]))
		} else {
			line += p.padRight(cell.val, p.cell_max_length[idx], ' ')
		}
		if p.show_sep_flag {
			line += " | "
		} else {
			line += " "
		}
	}
	fmt.Fprintln(os.Stdout, line)
}

func (p *Pretty) Render() {
	length := p.maxcell_per_line*3 + 1
	for _, mlen := range p.cell_max_length {
		length += mlen
	}
	if p.show_sep_flag {
		p.printHeader(length, '=')
	}
	for i := 1; i < len(p.lines); i++ {
		if len(p.lines[i]) > 0 && p.lines[i][0].t == CT_SEP {
			p.printHeaderLine(length, rune(p.lines[i][0].val[0]))
		} else {
			p.printLine(p.lines[i])
		}
	}
	if p.show_sep_flag {
		p.printHeaderLine(length, '=')
	}
}
