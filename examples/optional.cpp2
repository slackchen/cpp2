// M2d 示例:optional(T?)与 if-let / match some-none(DESIGN §6.4)
module optional_demo;
import std;

find_user: (id: int) -> string? = {
    if id == 7 { return "ada"; }
    if id == 8 { return "grace"; }
    return none;
}

greet: (id: int) -> string = match find_user(id) {
    some name => "hello, " + name;
    none      => "nobody";
}

main: () -> int = {
    // if-let:存在分支(绑定解包后的值)
    if u := find_user(7) {
        std::println("found {0}", u);
    } else {
        std::println("not found");
    }

    if _ := find_user(9) {
        std::println("9 exists");
    } else {
        std::println("9 missing");
    }

    std::println("{0}", greet(8));
    std::println("{0}", greet(100));
    return 0;
}
