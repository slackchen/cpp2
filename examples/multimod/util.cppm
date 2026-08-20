// app.util — 被依赖的库模块
module app.util;
import std;

export Point: type = {
    x: int = 0;
    y: int = 0;

    norm2: () -> int = x * x + y * y;
}

export add: (a: int, b: int) -> int = a + b;

double_it: (v: int) -> int = v * 2;        // 模块内部:未导出

export doubled_add: (a: int, b: int) -> int = double_it(add(a, b));


