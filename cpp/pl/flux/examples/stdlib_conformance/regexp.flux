import "regexp"

r = regexp.compile(v: "edge-[0-9]+")

{
    match: regexp.matchRegexpString(r: r, v: "edge-12"),
    find: regexp.findString(r: /[a-z]+/, v: "123cpu456"),
    quoted: regexp.quoteMeta(v: "cpu.usage"),
}

