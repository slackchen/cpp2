// C++2 模块图实现(M3)
#include "modules.hpp"

#include "lexer.hpp"
#include "parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace cpp2::mods {

namespace {

std::optional<std::string> read_file(fs::path const& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool is_virtual_module(std::vector<std::string> const& parts)
{
    // std 等由工具链桥接提供(stdbridge),不对应磁盘文件
    return !parts.empty() && parts[0] == "std";
}

std::string join_dots(std::vector<std::string> const& parts)
{
    std::string s = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) s += "." + parts[i];
    return s;
}

class Loader {
public:
    explicit Loader(fs::path root) : root_(std::move(root)) {}

    Graph run()
    {
        load_unit(root_, /*imported_by=*/"", /*import_name=*/"");
        visit(root_name_, 0);
        g_.root_name = root_name_;
        return std::move(g_);
    }

private:
    fs::path root_;
    Graph g_;
    std::unordered_set<std::string> done_;
    std::vector<std::string> stack_;          // 环检测

    std::vector<fs::path> search_dirs(fs::path const& importer)
    {
        std::vector<fs::path> dirs;
        dirs.push_back(fs::absolute(importer).parent_path());
        dirs.push_back(fs::absolute(root_).parent_path());
        if (char const* env = std::getenv("CPP2_PATH")) {
            std::string s = env;
            size_t start = 0;
            while (start <= s.size()) {
                size_t end = s.find_first_of(";:", start);
                std::string piece = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!piece.empty()) dirs.push_back(piece);
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        return dirs;
    }

    fs::path resolve(std::string const& name, fs::path const& importer)
    {
        // app.util → app/util.cppm、app.util.cppm、util.cppm(末段)
        std::string tail = name;
        size_t dot = tail.rfind('.');
        if (dot != std::string::npos) tail = tail.substr(dot + 1);

        for (auto const& d : search_dirs(importer)) {
            std::vector<fs::path> cands;
            std::string slashed;
            for (char c : name) slashed += (c == '.') ? '/' : c;
            cands.push_back(d / (slashed + ".cppm"));
            cands.push_back(d / (name + ".cppm"));
            cands.push_back(d / (tail + ".cppm"));
            for (auto const& c : cands)
                if (fs::exists(c)) return fs::weakly_canonical(c);
        }
        return {};
    }

    void load_unit(fs::path const& file, std::string const& imported_by,
                   std::string const& import_name)
    {
        (void)imported_by;                          // 保留参数:诊断上下文扩展用
        auto src = read_file(file);
        if (!src)
            throw GraphError{file.string(), 0,
                "cannot read module file '" + file.string() + "'"};

        ast::Module ast;
        try {
            ast = cpp2::parse::parse(cpp2::lex::lex(*src), file.string());
        } catch (cpp2::parse::ParseError const& e) {
            throw GraphError{file.string(), e.line, e.msg};
        } catch (cpp2::lex::LexError const& e) {
            throw GraphError{file.string(), e.line, e.msg};
        }

        std::string name = ast.name.empty()
            ? file.stem().string()
            : ast.name;
        if (!import_name.empty() && ast.name != import_name) {
            throw GraphError{file.string(), ast.name_line,
                "module file '" + file.string() + "' declares module '"
                + (ast.name.empty() ? "(default)" : ast.name)
                + "' but was imported as '" + import_name + "'"};
        }

        ModuleUnit u;
        u.file = file;
        u.name = name;
        u.source = *src;
        for (auto& im : ast.imports) {
            if (!is_virtual_module(im.module_parts))
                u.imports.push_back(join_dots(im.module_parts));
        }
        u.ast = std::move(ast);

        if (g_.units.empty()) root_name_ = name;   // 第一个加载的是 root
        g_.units.emplace(name, std::move(u));
    }

    std::string root_name_;

    // DFS:加载传递依赖 + 环检测 + 后序拓扑
    void visit(std::string const& name, int depth)
    {
        if (done_.count(name)) return;
        if (std::find(stack_.begin(), stack_.end(), name) != stack_.end()) {
            std::string cycle;
            for (auto& s : stack_) cycle += s + " -> ";
            throw GraphError{g_.units.at(name).file.string(), 0,
                "circular module dependency: " + cycle + name};
        }
        if (depth > 64)
            throw GraphError{g_.units.at(name).file.string(), 0, "module graph too deep"};

        stack_.push_back(name);
        auto imports = g_.units.at(name).imports;      // 拷贝:load 会改 map
        for (auto const& imp : imports) {
            if (g_.units.count(imp)) {
                if (imp == name)
                    throw GraphError{g_.units.at(name).file.string(), 0,
                        "module '" + name + "' imports itself"};
                continue;
            }
            fs::path f = resolve(imp, g_.units.at(name).file);
            if (f.empty())
                throw GraphError{g_.units.at(name).file.string(), 0,
                    "cannot resolve import '" + imp + "' (searched importer dir, "
                    "root dir and CPP2_PATH)"};
            load_unit(f, name, imp);
            visit(imp, depth + 1);
        }
        stack_.pop_back();
        done_.insert(name);
        g_.order.push_back(name);
    }
};

} // namespace

Graph load(fs::path const& root)
{
    return Loader(root).run();
}

// 导出接口规范化文本:接口哈希的数据源(.c2i 种子)
std::string interface_text(ast::Module const& m)
{
    std::ostringstream o;

    auto type_sig = [](ast::TypeUse const& t, auto&& self) -> std::string {
        std::string s;
        for (size_t i = 0; i < t.parts.size(); ++i) {
            if (i) s += "::";
            s += t.parts[i];
        }
        if (!t.args.empty()) {
            s += "<";
            for (size_t i = 0; i < t.args.size(); ++i) {
                if (i) s += ",";
                s += self(t.args[i], self);
            }
            s += ">";
        }
        return s;
    };
    auto ret_sig = [&](std::optional<ast::TypeUse> const& r) {
        return r ? type_sig(*r, type_sig) : "void";
    };

    for (auto const& e : m.enums) {
        if (!e.exported) continue;
        o << "enum " << e.name;
        if (e.underlying) o << ":" << type_sig(*e.underlying, type_sig);
        o << "{";
        for (auto& mem : e.members) o << mem << ",";
        o << "}\n";
    }
    for (auto const& s : m.structs) {
        if (!s.exported) continue;
        o << "struct " << s.name << "{";
        for (auto const& f : s.fields)
            o << f.name << ":" << type_sig(f.type, type_sig) << ";";
        for (auto const& md : s.methods) {
            o << md.name << "(";
            for (size_t i = 0; i < md.params.size(); ++i) {
                if (i) o << ",";
                o << type_sig(md.params[i].type, type_sig);
            }
            o << ")" << ret_sig(md.ret) << (md.mutates ? " mutates" : "")
              << (md.throws ? " throws" : "") << ";";
        }
        o << "}\n";
    }
    for (auto const& g : m.globals) {
        if (!g.exported) continue;
        o << "var " << g.name << ":" << type_sig(g.type, type_sig) << "\n";
    }
    for (auto const& f : m.funcs) {
        if (!f.exported) continue;
        o << "fn " << f.name << "(";
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (i) o << ",";
            o << f.params[i].name << ":" << type_sig(f.params[i].type, type_sig);
        }
        o << ")" << ret_sig(f.ret) << (f.throws ? " throws" : "") << "\n";
    }
    return o.str();
}

} // namespace cpp2::mods
