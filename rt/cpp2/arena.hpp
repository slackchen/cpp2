// cpp2/arena.hpp — 区域内存(M6,DESIGN §7.6 / IMPL §5)
// create<T>(...) 登记析构(创建序);reset() 逆序析构后整块归还。
// v1 按对象分配 + 析构登记(正确性优先);段式池为后续优化(见偏差表)。
#pragma once

#include <functional>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace cpp2 {

template <class T> class arena_ptr;         // 前置:arena::create 返回它

class arena {
public:
    arena() = default;
    arena(arena const&) = delete;
    arena& operator=(arena const&) = delete;

    ~arena() { reset(); }

    template <class T, class... Args>
    arena_ptr<T> create(Args&&... args)
    {
        void* mem = ::operator new(sizeof(T));
        T* p = new (mem) T(std::forward<Args>(args)...);
        destroy_.push_back([p] { p->~T(); });
        blocks_.push_back(mem);
        return arena_ptr<T>(p, this);
    }

    // 单参推导重载:make_in 包装经它获得 T 推导(否则 T 仅在返回类型,不可推导)
    template <class T>
    auto create(T&& v) -> arena_ptr<std::remove_reference_t<T>>
    {
        using U = std::remove_reference_t<T>;
        using B = std::remove_const_t<U>;
        return create<B>(std::forward<T>(v));
    }

    // 逆序析构全部对象并归还内存;之后 arena 可复用
    void reset()
    {
        for (size_t i = destroy_.size(); i-- > 0;) destroy_[i]();
        destroy_.clear();
        for (void* m : blocks_) ::operator delete(m);
        blocks_.clear();
    }

    size_t live() const { return blocks_.size(); }

private:
    std::vector<std::function<void()>> destroy_;
    std::vector<void*> blocks_;
};

// arena_ptr<T>:指向 arena 内对象;不得逃逸出 arena 生存期(M5-L6,
// sema 强制:不可 return / 不可流入非 arena_ptr 存储 / 不支持算术)
template <class T>
class arena_ptr {
public:
    arena_ptr() = default;
    arena_ptr(T* p, arena* a) : p_(p), a_(a) {}

    T*   get()       const { return p_; }
    T&   operator*() const { return *p_; }
    T*   operator->()const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
    arena* a_ = nullptr;
};

} // namespace cpp2
