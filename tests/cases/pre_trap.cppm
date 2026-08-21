// pre_trap.cppm — 契约违反 = bug → trap(带 .cppm 源位置,不可捕获)
module pretrap;
import std;

withdraw: (inout balance: i64, amount: i64)
    pre:  amount > 0 && amount <= balance
= {
    balance -= amount;
}

main: () -> int = {
    balance: i64 := 100;
    withdraw(balance, 200);        // pre 违反:200 <= 100 不成立
    return 0;
}
