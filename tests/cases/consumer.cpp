// Cpp1 消费者:只写普通 C++,include 桥接头(互操作烟雾测试)
#include "app_lib.h"
#include <cstdio>

int main()
{
    std::printf("%d\n", triple(14));               // 42

    Vec v;
    v.items.push_back(1);
    v.items.push_back(2);
    v.items.push_back(3);
    std::printf("%d\n", v.total());                // 6
    return 0;
}
