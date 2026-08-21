// M2d 示例:variant / 模式匹配(类型模式、解构、守卫、通配)/ match 表达式
// DESIGN §4.5 / §5.4 / §5.5
module shapes;
import std;

Circle: type = {
    r: double = 0;
}

Rect: type = {
    w: double = 0;
    h: double = 0;
}

// 类型安全的联合(DESIGN §5.5):match 是唯一合法的访问方式,穷尽性由编译器保证
Shape: variant = { Circle, Rect }

// match 表达式:直接产值;守卫失败落入后续同模式臂
area: (s: Shape) -> double = match s {
    Circle(r) if r > 0 => 3.14159 * r * r;
    Circle(_)          => 0;
    Rect(w, h)         => w * h;
}

// 语句形式 + enum 成员模式(DESIGN §5.4)
Signal: enum = { red, green, yellow }

describe: (s: Shape) -> string = match s {
    Circle _ => "circle";
    Rect _   => "rectangle";
}

main: () -> int = {
    c: Shape := Circle{.r = 2};
    std::println("circle area = {0}", area(c));

    z: Shape := Rect{.w = 3, .h = 4};
    std::println("rect area = {0}", area(z));

    n: Shape := Circle{.r = -1};            // 守卫失败 → 落入 Circle(_) => 0
    std::println("neg area = {0}", area(n));

    std::println("z is {0}", describe(z));

    sig: Signal := Signal::green;
    match sig {
        .red   => std::println("stop");
        .green => std::println("go");
        _      => std::println("slow");
    }
    return 0;
}
