// tools/native_rt.c ? native ?????????(M7 P1 ??)
// ? libc:print_int / ?? trap???:cc main.o native_rt.o -o prog
#include <stdio.h>
#include <stdlib.h>

void print_int(long long v) { printf("%lld\n", v); }
void cpp2_native_div0(void)
{
    fprintf(stderr, "cpp2 trap: division by zero\n");
    exit(101);
}
