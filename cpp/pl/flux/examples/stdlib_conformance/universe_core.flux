values = ["cpu", "mem", "disk"]

{
    len_string: len("edge"),
    len_array: len(values),
    len_object: len({host: "edge-1", region: "west"}),
    string_time: string(2024-01-01T00:00:00Z),
    contains_mem: contains(set: values, value: "mem"),
}
