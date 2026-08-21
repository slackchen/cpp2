// float_trap — 浮点→整型转换越界检查(DESIGN §6.3)
module traps;
import std;

main: () -> int = {
    big: double := 1.0e300;
    n: i32 := big as i32;              // ← 期望 trap: float-to-integer conversion out of range
    std::print("unreachable ");
    std::print(n);
    return 0;
}
