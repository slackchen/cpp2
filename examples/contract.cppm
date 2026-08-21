// contract.cppm — 契约:pre / post / old() / result(DESIGN §6.5)
// 契约违反 = bug → trap,不可捕获(与错误值通道严格分离)。
module contract;
import std;

Account: type = {
    balance: i64 = 0;
}

// pre:入口断言;post:出口断言;old() 在入口求值,result 绑定返回值
withdraw: (inout acct: Account, amount: i64) -> i64
    pre:  amount > 0 && amount <= acct.balance
    post: acct.balance == old(acct.balance) - amount && result == amount
= {
    acct.balance -= amount;
    return amount;
}

// 纯函数契约:post 直接给出可验证的数学性质
int_sqrt: (n: int) -> int
    pre:  n >= 0
    post: result * result <= n && (result + 1) * (result + 1) > n
= {
    r: int := 0;
    while (r + 1) * (r + 1) <= n {
        r += 1;
    }
    return r;
}

Counter: type = {
    value: int = 0;

    // 方法契约:mutates 签名 + pre/post 同函数
    bump: (step: int) mutates
        pre:  step > 0
        post: value == old(value) + step
    = {
        value += step;
    }
}

main: () -> int = {
    a: Account := Account{.balance = 100};
    w: i64 := withdraw(a, 30);
    std::print("w = {0}, balance = {1}\n", w, a.balance);   // 30, 70

    std::print("int_sqrt(26) = {0}\n", int_sqrt(26));       // 5

    c: Counter := Counter{};
    c.bump(5);
    std::print("value = {0}\n", c.value);                   // 5
    return 0;
}
