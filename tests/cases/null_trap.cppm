// null_trap — 空安全检查注入:智能指针解引用前必须有非空证明
module traps;
import std;

Point: type = {
    x: int = 0;
    y: int = 0;
}

main: () -> int = {
    v: vector<shared<Point>> = {};
    v.resize(2);                       // 元素默认构造:空的 shared
    bad: int := v[0].x;                // ← 期望 trap: null dereference(位置映射回本行)
    std::print("unreachable ");
    std::print(bad);
    return 0;
}
