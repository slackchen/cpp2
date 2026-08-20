// optout — @unsafe/@unchecked 块形式(DESIGN §6.2/§6.6)+ 检查逃逸与恢复
module optout;
import std;

main: () -> int = {
    v: list<int> := [1, 2, 3];
    sum: int := 0;

    @unchecked for i in 0..3 {          // 语句形式:仅此循环退出检查
        sum += v[i];                    // 无 index/checked 注入
    }

    @unsafe {                           // 块形式
        raw: int := v[2];
        sum += raw;
    }

    @unchecked {                        // 块形式
        sum += v[1];
    }

    std::print("sum = ");
    std::print(sum);                    // 6 + 3 + 2 = 11
    std::print("\n");

    std::print(v[0]);                   // 块外恢复检查:合法访问仍受保护
    std::print("\n");
    return 0;
}
