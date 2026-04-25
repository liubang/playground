import "strings"

raw = "  edge-1.cpu  "
trimmed = strings.trimSpace(v: raw)

{
    contains: strings.containsStr(v: trimmed, substr: "edge"),
    prefix: strings.hasPrefix(v: trimmed, prefix: "edge"),
    suffix: strings.hasSuffix(v: trimmed, suffix: "cpu"),
    upper: strings.toUpper(v: trimmed),
    lower: strings.toLower(v: "CPU"),
    joined: strings.joinStr(arr: strings.split(v: trimmed, t: "."), v: "/"),
    replaced: strings.replaceAll(v: trimmed, t: "edge", u: "node"),
}

