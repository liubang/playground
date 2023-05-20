package tricks

// a, b, c 的默认类型是 rune (rune 是 int32 的别名)，且 a, b, c 被当作 slice 的下标索引
// 由于 a, b, c 的 ascii 分别为 97, 98, 99, 所以下面的数组初始化等价于
// ```go
//
//	m := [...]int {
//	  97: 1,
//	  98: 2,
//	  99: 3,
//	}
//
// m 的最大下标为 99, 故 m 的长度为 100
// ```
func SliceTricks1() int {
	m := [...]int{
		'a': 1,
		'b': 2,
		'c': 3,
	}
	return len(m)
}
