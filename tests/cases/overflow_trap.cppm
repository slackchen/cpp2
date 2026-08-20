module traps;
import std;

main: () -> int = {
    big: i64 := 9'223'372'036'854'775'807;
    bigger: i64 := big + 1;                // ← 期望 trap: integer overflow
    std::print("unreachable ");
    std::print(bigger);
    return 0;
}
