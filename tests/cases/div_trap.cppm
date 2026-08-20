module traps;
import std;

main: () -> int = {
    a: int := 10;
    b: int := 0;
    q: int := a / b;                       // ← 期望 trap: division by zero
    std::print("unreachable ");
    std::print(q);
    return 0;
}
