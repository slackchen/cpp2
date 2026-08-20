// C++2 模糊测试(M4):词法/语法/语义鲁棒性
// 不变量:任意输入下,前端要么正常完成、要么给出 LexError/ParseError 诊断;
// 绝不允许崩溃、非预期异常或失去终止性(IMPL §7.5)。
#pragma once

#include <string>
#include <vector>

namespace cpp2::fuzz {

struct Outcome {
    int iterations = 0;
    int crashes = 0;
    std::vector<std::string> crash_files;   // 复现输入(已写盘)
};

// 单输入卫生检查:lex → parse → sema。
// 返回 0 = 干净(合法或产出预期诊断);非 0 = 崩溃级失败(异常逃逸等)。
int run_one(std::string const& src, std::string const& name);

// corpus 驱动的变异模糊测试;固定 seed 可复现。
// 变异:翻转/插入/删除/复制/拼接/截断;输入上限 64 KB。
Outcome run(std::vector<std::string> const& corpus, unsigned seed, int iters,
            std::string const& crash_dir);

} // namespace cpp2::fuzz
