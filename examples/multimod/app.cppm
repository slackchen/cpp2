// app — 根模块(main 所在)
module app;
import std;
import app.util;

main: () -> int = {
    p: Point := Point{.x = 3, .y = 4};
    std::print("norm2 = ");
    std::print(p.norm2());                 // 25
    std::print("\n");

    std::print("add(2,3) = ");
    std::print(add(2, 3));                 // 5
    std::print("\n");

    std::print("doubled_add(2,3) = ");
    std::print(doubled_add(2, 3));         // 10
    std::print("\n");
    return 0;
}
