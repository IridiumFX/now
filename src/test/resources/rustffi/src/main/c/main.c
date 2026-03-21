#include <stdio.h>

/* Rust FFI function */
extern int rust_add(int a, int b);

int main(void) {
    int result = rust_add(17, 25);
    printf("rust_add(17, 25) = %d\n", result);
    return result == 42 ? 0 : 1;
}
