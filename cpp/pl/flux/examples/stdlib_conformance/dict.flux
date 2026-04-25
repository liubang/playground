import "dict"

base = dict.fromList(
    pairs: [
        {key: "edge-1", value: 80},
        {key: "edge-2", value: 60},
    ],
)

updated = dict.insert(dict: base, key: "edge-3", value: 70)
overwritten = dict.insert(dict: updated, key: "edge-2", value: 65)
trimmed = dict.remove(dict: overwritten, key: "edge-1")
literal = [1: "one", 2: "two"]

{
    edge2: dict.get(dict: overwritten, key: "edge-2", default: 0),
    missing: dict.get(dict: overwritten, key: "edge-9", default: -1),
    removed: dict.get(dict: trimmed, key: "edge-1", default: 0),
    literal_two: dict.get(dict: literal, key: 2, default: ""),
}
