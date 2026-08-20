// safety.cppm — 溢出/越界检查默认开启,@unchecked 块级退出
module safety;
import std;

main: () -> int = {
    // 有符号算术 → checked_*(溢出 trap)
    sum: int := 1 + 2 * 3;                 // 7
    std::print("sum = ");
    std::print(sum);
    std::print("\n");

    // 越界检查:list 可下标,越界 trap
    v: list<int> := [10, 20, 30];
    std::print("v[1] = ");
    std::print(v[1]);                      // 20
    std::print("\n");

    // @unchecked:块内退出检查(热点路径惯用)
    total: int := 0;
    @unchecked for i in 0..3 {
        total += v[i];                     // 不注入 index/checked
    }
    std::print("total = ");
    std::print(total);                     // 60
    std::print("\n");

    // 循环外恢复检查
    std::print("v[2] = ");
    std::print(v[2]);                      // 30
    std::print("\n");
    return 0;
}
