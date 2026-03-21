package main

import "C"

//export go_multiply
func go_multiply(a, b C.longlong) C.longlong {
    return a * b
}

func main() {}
