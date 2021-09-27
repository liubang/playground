package main

import (
	"encoding/json"
	"log"
)

type Foo struct {
	Name string `json:"name"`
}

type Bar struct {
	Foo *Foo `json:"foo"`
}

func main() {
	f := Bar{Foo: &Foo{Name: "hello"}}

	j, err := json.Marshal(f)
	if err != nil {
		log.Println(err)
	} else {
		log.Println(string(j))
	}

	{
		s := `{"foo": {"name": "hello"}}`

		var b Bar
		err = json.Unmarshal([]byte(s), &b)
		if err != nil {
			log.Println(err)
		} else {
			log.Println(b.Foo.Name)
		}
	}

	{
		s := `{}`

		var b Bar
		err = json.Unmarshal([]byte(s), &b)
		if err != nil {
			log.Println(err)
		} else {
			log.Println(b.Foo.Name)
		}
	}
}
