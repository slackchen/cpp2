// post_trap.cppm — post 违反(old() 入口快照)→ trap
module posttrap;
import std;

bump_to: (inout x: int, target: int)
    post: x >= old(x)
= {
    x = target;
}

main: () -> int = {
    x: int := 10;
    bump_to(x, 3);                 // post 违反:3 >= 10 不成立
    return 0;
}
