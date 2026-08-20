// smart.cppm — unique/shared 所有权、'.' 自动解引用、显式 move
module smart;
import std;

Point: type = {
    x: int = 0;
    y: int = 0;

    norm2: () -> int = x * x + y * y;
}

consume: (move p: unique<Point>) -> int = p.norm2();

main: () -> int = {
    p: unique<Point> := make_unique(Point{.x = 3, .y = 4});
    std::print("p.x = ");
    std::print(p.x);                       // '.' 自动解引用 → p->x
    std::print(", norm2 = ");
    std::print(p.norm2());                 // 25
    std::print("\n");

    n: int := consume(move p);             // 所有权转移,调用侧显式
    std::print("consumed norm2 = ");
    std::print(n);                         // 25
    std::print("\n");

    q: shared<Point> := make_shared(Point{.x = 1, .y = 2});
    r: shared<Point> := q;                 // 共享:引用计数 +1
    std::print("shared x = ");
    std::print(r.x);                       // 1
    std::print("\n");
    return 0;
}
