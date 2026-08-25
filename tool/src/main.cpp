// cpp2 命令行分发(模块化拆分后仅保留 usage + 分发)
#include "app.hpp"

#include <iostream>
#include <string>
#include <vector>



void usage()
{
    std::cerr
        << "usage:\n"
        << "  cpp2 run <root.cpp2> [--backend=native] [--release]   # 摊平转译 + 编译 + 执行\n"
        << "  cpp2 check <root.cpp2> [--quick]                      # 快速语义检查,不生成代码\n"
        << "  cpp2 build [root.cpp2] [--backend=headers|cxx20-modules]\n"
        << "      [--max-jobs=N] [--release]                        # 并行增量构建(headers 默认)\n"
        << "  cpp2 transpile <root.cpp2> [-o out.cpp] [--release]   # 摊平转译查看生成码\n"
        << "  cpp2 export-headers <root.cpp2> [-o dir]              # 生成 Cpp1 消费者 .h/.cpp\n"
        << "  cpp2 audit <root.cpp2>                                # 安全审计:检查点 + 退出点\n"
        << "  cpp2 fuzz <corpus...> [--seed N --iters N]            # 前端模糊测试\n"
        << "  cpp2 version\n";
}

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    if (cmd == "transpile")       return cpp2::app::cmd_transpile(args);
    if (cmd == "check")           return cpp2::app::cmd_check(args);
    if (cmd == "run")             return cpp2::app::cmd_run(args);
    if (cmd == "build")           return cpp2::app::cmd_build(args);
    if (cmd == "export-headers")  return cpp2::app::cmd_export_headers(args);
    if (cmd == "audit")           return cpp2::app::cmd_audit(args);
    if (cmd == "fuzz")            return cpp2::app::cmd_fuzz(args);
    if (cmd == "version")         { std::cout << "cpp2 0.1.0-m7b (virtual dispatch, error categories, pipes, shifts, captures, if-bodies)" << "\n"; return 0; }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { usage(); return 0; }

    std::cerr << "error: unknown command '" << cmd << "'\n";
    usage();
    return 1;
}
