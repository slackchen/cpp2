// colors.cppm — scoped enum、as 转换、底层类型
module colors;
import std;

Color: enum = { red, green, blue }

Priority: enum: u8 = { low, normal, high }

main: () -> int = {
    c: Color := Color::green;
    std::print("green as int = ");
    std::print(c as int);                  // 1(enum 默认从 0 起)
    std::print("\n");

    n: int := 2;
    d: Color := n as Color;
    std::print("d == Color::blue: ");
    std::print(d == Color::blue);          // true
    std::print("\n");

    p: Priority := Priority::high;
    std::print("high as int = ");
    std::print(p as int);                  // 2
    std::print("\n");
    return 0;
}
