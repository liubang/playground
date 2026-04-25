import "types"

{
    int_numeric: types.isNumeric(v: 42),
    float_numeric: types.isNumeric(v: 1.5),
    string_numeric: types.isNumeric(v: "cpu"),
    string_type: types.isType(v: "cpu", type: "string"),
    bool_type: types.isType(v: true, type: "bool"),
    time_type: types.isType(v: 2024-01-01T00:00:00Z, type: "time"),
    duration_type: types.isType(v: 5m, type: "duration"),
    regexp_type: types.isType(v: /cpu.*/, type: "regexp"),
    bytes_type: types.isType(v: "cpu", type: "bytes"),
    is_string: types.isString(v: "cpu"),
    is_int: types.isInt(v: 42),
    is_uint: types.isUInt(v: 42u),
    is_float: types.isFloat(v: 1.5),
    is_bool: types.isBool(v: true),
    is_time: types.isTime(v: 2024-01-01T00:00:00Z),
    is_duration: types.isDuration(v: 5m),
    is_regexp: types.isRegexp(v: /cpu.*/),
    string_is_duration: types.isDuration(v: "5m"),
}
