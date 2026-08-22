// cpp2/gc.hpp — 可选保守式 GC(M6,DESIGN §7.5 / IMPL §5)
// v1 形态(白纸黑字):
//   - 单线程、显式触发(collect)、无终结器
//   - 保守栈扫描:调用帧区间内的字值视为潜在指针 → 只会少收不会错收;
//     死帧残留指针可能延迟回收(保守式的本质代价,非确定性)
//   - 对象为 POD 式生命周期(new 后不调析构)——仅用于无资源语义的数据体
//   - 栈顶界由生成代码在 main 入口注入(gc_stack_mark),覆盖 main 全帧
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

#include <cstdint>                                      // uintptr_t

namespace cpp2 {

namespace gc {

struct block {
    void* mem;
    size_t size;
    bool mark;
};

inline std::vector<block>& blocks()
{
    static std::vector<block> b;
    return b;
}

inline size_t& collections()
{
    static size_t c = 0;
    return c;
}

inline size_t& total_allocations()
{
    static size_t a = 0;
    return a;
}

inline char const*& stack_hi()
{
    // 栈顶界(高地址):main 入口的 gc_stack_mark() 注入
    static char const* hi = nullptr;
    return hi;
}

template <class T, class... Args>
T* make(Args&&... args)
{
    void* mem = ::operator new(sizeof(T));
    T* p = new (mem) T(std::forward<Args>(args)...);
    blocks().push_back({mem, sizeof(T), false});
    ++total_allocations();
    return p;
}

struct stats {
    size_t allocations;                           // 累计分配块数
    size_t live_blocks;                           // 当前池内块数(死块即时移除)
    size_t live_bytes;
    size_t collections;
};

inline stats stat()
{
    size_t bytes = 0;
    for (auto& b : blocks()) bytes += b.size;
    return { total_allocations(), blocks().size(), bytes, collections() };
}

// 候选指针命中池内块 → 标记;块内容入工作列表(传递闭包)
inline void mark_from(void* candidate, std::vector<void*>& worklist)
{
    for (auto& b : blocks()) {
        if (b.mark) continue;
        char* lo = static_cast<char*>(b.mem);
        char* hi = lo + b.size - sizeof(void*);
        if (candidate >= lo && candidate <= hi) {
            b.mark = true;
            worklist.push_back(b.mem);
        }
    }
}

inline void collect()
{
    char anchor;                                       // 当前栈位置(x86 下行)
    char const*& hi_slot = stack_hi();
    char const* top = hi_slot ? hi_slot : &anchor;

    char* lo = &anchor < top ? &anchor : const_cast<char*>(top);
    char* hi = &anchor < top ? const_cast<char*>(top) : &anchor;

    // 1) 保守根集:扫描 main→collect 的栈区间(指针宽度步长;起点对齐到
    //    指针边界——锚变量是 char,不对齐会系统性错过所有对齐槽)
    std::vector<void*> roots;
    {
        uintptr_t hi_al = reinterpret_cast<uintptr_t>(hi)
                        & ~(uintptr_t)(sizeof(void*) - 1);
        for (uintptr_t a = hi_al; a >= reinterpret_cast<uintptr_t>(lo); a -= sizeof(void*)) {
            void* v;
            std::memcpy(&v, reinterpret_cast<void*>(a), sizeof v);
            roots.push_back(v);
            if (a < sizeof(void*)) break;                // 下溢防护
        }
    }

    // 2) 标记 + 块内容传递闭包
    std::vector<void*> worklist(roots.begin(), roots.end());
    while (!worklist.empty()) {
        void* r = worklist.back();
        worklist.pop_back();
        for (auto& b : blocks()) {
            if (b.mark) continue;
            char* blo = static_cast<char*>(b.mem);
            char* bhi = blo + b.size - sizeof(void*);
            if (r >= blo && r <= bhi) {
                b.mark = true;
                for (size_t off = 0; off + sizeof(void*) <= b.size;
                     off += sizeof(void*)) {
                    void* inner;
                    std::memcpy(&inner, static_cast<char*>(b.mem) + off, sizeof inner);
                    worklist.push_back(inner);
                }
            }
        }
    }

    // 3) 清扫:未标记块归还内存
    std::vector<block> survivors;
    survivors.reserve(blocks().size());
    for (auto& b : blocks()) {
        if (b.mark)
            survivors.push_back({b.mem, b.size, false});
        else
            ::operator delete(b.mem);
    }
    blocks().swap(survivors);
    ++collections();
#ifdef CPP2_GC_DEBUG
    std::fprintf(stderr, "[gc] lo=%p hi=%p roots=%zu survivors=%zu\n",
                 (void*)lo, (void*)hi, roots.size(), survivors.size());
#endif
}

// main 入口栈顶界(生成代码在 main 帧内放置锚变量后调用)
inline void gc_set_stack_top(void* p)
{
    stack_hi() = static_cast<char const*>(p);
}

} // namespace gc

// ── 语言面入口(cpp2 命名空间)──────────────────────────────────

using gc::gc_set_stack_top;                 // main 入口锚点(生成代码注入)

// 泛型分配:T 由实参推导;返回裸指针(保守扫描识别原生指针)
template <class T>
auto gc_new(T&& v)
    -> std::remove_const_t<std::remove_reference_t<T>>*
{
    using B = std::remove_const_t<std::remove_reference_t<T>>;
    return gc::make<B>(std::forward<T>(v));
}

inline void gc_collect() { gc::collect(); }

inline std::int64_t gc_live_bytes()
{
    auto s = gc::stat();
    return static_cast<std::int64_t>(s.live_bytes);
}

inline std::int64_t gc_collections()
{
    return static_cast<std::int64_t>(gc::collections());
}

} // namespace cpp2
