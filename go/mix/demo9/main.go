package main

import (
	"log"
	"math"
)

func main() {
	// nss := []string{"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k"}
	nss := []string{}
	groups := math.Ceil(float64(len(nss)) / 3.00)
	log.Println("len:", len(nss))
	log.Println("groups: ", groups)
	for i := 0; i < int(groups); i++ {
		var ns []string
		if i != int(groups)-1 {
			ns = nss[i*3 : (i+1)*3]
		} else {
			ns = nss[i*3:]
		}
		log.Println(ns)
	}
}
