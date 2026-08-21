// errors.cppm — 错误通道:? 传播 / ! 断言 / or 默认 / match ok-err / if-let(DESIGN §8)
// 错误是值(throws + expected),bug 是 trap(! 断言失败、契约违反)。
module errors;
import std;

// throws:可能失败的函数;失败即返回错误值,不抛异常
parse_int: (s: string) -> int throws = {
    if s.empty() { return err("empty input"); }
    n: int := 0;
    for c in s {
        if c < '0' || c > '9' {
            return err("not a digit in '" + s + "'");
        }
        n = n * 10 + ((c as int) - ('0' as int));
    }
    return n;
}

// ? :失败机械向上传播(本函数因此也在错误通道上,DESIGN §8.2)
double_it: (s: string) -> int throws = {
    n: int := parse_int(s)?;
    return n * 2;
}

main: () -> int = {
    // or:失败取默认值(DESIGN §8.3)
    a: int := parse_int("21") or 0;
    std::print("a = {0}\n", a);                    // 21

    // !:确信必成功;失败即 bug → trap
    b: int := parse_int("7")!;
    std::print("b = {0}\n", b);                    // 7

    // match ok/err:失败路径强制显式
    match double_it("20") {
        ok  n => std::print("double = {0}\n", n);  // 40
        err e => std::print("failed: {0}\n", e.message());
    }
    match double_it("2x") {
        ok  n => std::print("double = {0}\n", n);
        err e => std::print("failed: {0}\n", e.message());
    }

    // if-let 成功路径 + else 绑定错误(DESIGN §8.3)
    if v := parse_int("99") {
        std::print("v = {0}\n", v);                // 99
    } else e := it {
        std::print("failed: {0}\n", e.message());
    }
    return 0;
}
