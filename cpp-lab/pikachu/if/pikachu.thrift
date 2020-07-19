namespace cpp2 pikachu

enum Status {
    OK = 0,
    RS_KEY_NOT_FOUND = 1,
    RS_KEY_EXPIRED = 2,
}

exception PikachuException {
    1: required string message
    2: required Status status 
}

struct Key {
    1: required string db_name
    2: required string tb_name
    3: required list<string> pks
}

struct Keys {
    1: required list<Key> ks
}

union Val {
    1: string string_value
    2: map<string, string> map_value
    3: bool null_value
}

struct KeyVal {
    1: required Key key
    2: required Val val
}

union Response {
    1: string string_data
    2: i64 int_data
    3: list<string> list_string_data
    4: list<i64> list_int_data
    5: map<string, string> map_data
    6: bool bool_data
}

service PikachuService {
    // key
    Response delkey(1: Key key) throws (1: PikachuException e)
    Response exists(1: Key key) throws (1: PikachuException e)
    // raw string
    Response get(1: Key key) throws (1: PikachuException e)
    Response sset(1: KeyVal kv) throws (1: PikachuException e)
}
