// tools/native_rt.c ? native backend runtime shim (M7 P1/v1)
// libc only: print_int / div0 trap. Link: cc main.o native_rt.o -o prog
#include <stdio.h>
#include <stdlib.h>

void print_int(long long v) { printf("%lld\n", v); }
void cpp2_native_div0(void)
{
    fprintf(stderr, "cpp2 trap: division by zero\n");
    exit(101);
}
