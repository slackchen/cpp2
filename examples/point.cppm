// point.cppm — 类型定义、方法、mutates、const 接收者、struct 字面量
module point;
import std;

export Point: type = {
    x: int = 0;
    y: int = 0;

    length: () -> double = std::sqrt(x * x + y * y);

    translate: (dx: int, dy: int) mutates = {
        x += dx;
        y += dy;
    }
}

main: () -> int = {
    p: Point := Point{.x = 3, .y = 4};
    std::print("len = ");
    std::print(p.length());                // 5
    std::print("\n");

    p.translate(1, 1);
    std::print("after translate: x = ");
    std::print(p.x);
    std::print(", y = ");
    std::print(p.y);                       // 4, 5
    std::print("\n");

    q: const Point := Point{};             // 未给字段 → 用默认值 0, 0
    // q.translate(1, 1);                  // 打开注释 → 编译错误:const 上调用 mutates
    std::print("q.len = ");
    std::print(q.length());                // 0(const 上调用只读方法 OK)
    std::print("\n");
    return 0;
}
