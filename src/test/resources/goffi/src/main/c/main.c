#include <stdio.h>
extern long long go_multiply(long long a, long long b);
int main(void) {
    long long r = go_multiply(6, 7);
    printf("go_multiply(6, 7) = %lld\n", r);
    return r == 42 ? 0 : 1;
}
