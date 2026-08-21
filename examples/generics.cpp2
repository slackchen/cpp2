// M2d 示例:泛型 + concept、requires 子句、匿名函数、UFCS
// DESIGN §4.6 / §4.7 / §5.6
module generics;
import std;

// concept 定义(接口块):self 指代满足概念的类型
Ordered: concept = {
    operator<: (that: self) -> bool;
}

Printable: concept = {
    print_value: () -> string;
}

// 类型参数在名字后的 <> 中;约束用 : 附加(DESIGN §5.6)
clamp: <T: Ordered> (v: T, lo: T, hi: T) -> T = {
    if v < lo { return lo; }
    if hi < v { return hi; }
    return v;
}

// 复杂约束用 requires 子句
mid3: <T> (a: T, b: T, c: T) -> T
    requires Ordered<T>
= {
    lo: T := a;
    if b < lo { lo = b; }
    if c < lo { lo = c; }
    return lo;
}

// 泛型 + 匿名函数实参(DESIGN §4.6):F 由 lambda 类型推导
transform: <T, F> (v: vector<T>, f: F) -> vector<T> = {
    out: vector<T> := {};
    for x in v {
        out.push_back(f(x));
    }
    return out;
}

main: () -> int = {
    // UFCS(DESIGN §4.7):n.to_string() ≡ to_string(n),桥到 std::to_string
    n: int := 42;
    s: string := n.to_string();
    std::println("n to_string = {0}", s);

    a: int := clamp(5, 1, 10);
    b: int := clamp(-3, 0, 10);
    d: double := clamp(2.5, 0.0, 2.0);
    std::println("clamp 5 = {0}", a);
    std::println("clamp -3 = {0}", b);
    std::println("clamp 2.5 = {0}", d);

    m: int := mid3(7, 3, 9);
    std::println("mid3 = {0}", m);

    v: vector<int> := {1, 2, 3, 4};
    sq: vector<int> := transform(v, (x: int) -> int = x * x);
    total: int := 0;
    for y in sq {
        total += y;
    }
    std::println("squares sum = {0}", total);
    return 0;
}
