module traps;
import std;

main: () -> int = {
    v: list<int> := [1, 2, 3];
    std::print("v[2] = ");
    std::print(v[2]);
    std::print("\n");
    bad: int := v[7];                      // ← 期望 trap: index out of bounds
    std::print("unreachable ");
    std::print(bad);
    return 0;
}
