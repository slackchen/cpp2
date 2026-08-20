// C++2 安全审计(M4):统计检查注入点 + @unsafe/@unchecked 位置
// DESIGN §6.6:安全边界"白纸黑字"——一条命令列出全部退出点。
#pragma once

#include "ast.hpp"
#include "sema.hpp"

#include <string>
#include <vector>

namespace cpp2::audit {

struct OptOut {
    int line = 0;
    bool unsafe = false;                    // true=@unsafe,false=@unchecked
};

struct Report {
    int checked_arith = 0;                  // 溢出/除零检查注入点
    int checked_index = 0;                  // 越界检查注入点
    int checked_deref = 0;                  // 空检查注入点
    int checked_narrow = 0;                 // as 收窄检查注入点
    std::vector<OptOut> opt_outs;

    int checks() const
    { return checked_arith + checked_index + checked_deref + checked_narrow; }
    int unchecked() const;
    int unsafe() const;
};

// 统计一个模块(谓词与 emit 侧注入规则一致,保证"报告 = 生成码事实")
Report report_for(ast::Module& m, sema::Result const& r);

// 人读格式(单模块一段)
std::string format_section(std::string const& module_name, std::string const& file,
                           Report const& rep);

} // namespace cpp2::audit
