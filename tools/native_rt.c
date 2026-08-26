// tools/native_rt.c — native 后端运行时垫片(asm 路径用;PE 直出不需要本文件)
// 契约符号 = std 内建层(builtin)在 C 侧的实现,与 rt/cpp2/support.hpp 对齐:
//   cpp2_write(s, n) / sys_exit(code)
// 跨平台:POSIX 用 unistd write/_exit;Windows 用 io.h _write/ExitProcess。
#if defined(_WIN32)
    #include <io.h>
    #include <windows.h>
    static long long rt_write(const char* s, int n)
    {
        unsigned long written = 0;
        void* h = GetStdHandle((unsigned)-11);
        WriteFile(h, s, (unsigned long)n, &written, NULL);
        return (long long)written;
    }
    static void rt_exit(int c) { ExitProcess((unsigned)c); }
#else
    #include <unistd.h>
    static long long rt_write(const char* s, int n) { return write(1, s, n); }
    static void rt_exit(int c) { _exit(c); }
#endif

void cpp2_write(const char* s, int n) { rt_write(s, n); }
void sys_exit(int code) { rt_exit(code); }

// print_int 不在此提供:用户程序(std::println 或自带定义)负责
void cpp2_native_div0(void)
{
    cpp2_write("cpp2 trap: division by zero\n", 29);
    sys_exit(101);
}
