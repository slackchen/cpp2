// C++2 模糊测试实现(M4):确定性变异 + 前端卫生检查
#include "fuzz.hpp"

#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace cpp2::fuzz {

namespace {

constexpr size_t kMaxInput = 64 * 1024;

bool write_file(fs::path const& p, std::string const& content)
{
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << content;
    return static_cast<bool>(f);
}

std::optional<std::string> read_file(fs::path const& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

char random_syntax_char(unsigned v)
{
    // 覆盖语言的有意义字符:标识符、数字、运算符、引号、括号
    static char const chars[] =
        "abcxyzM_0123456789 \t\n:;,.<>()[]{}+-*/%=!?&|~^@'\"#";
    return chars[v % (sizeof chars - 1)];
}

// 对 seed 文本施加 1..8 次随机变异
std::string mutate(std::string base, std::mt19937& rng, std::vector<std::string> const& corpus)
{
    auto below = [&rng](size_t n) -> size_t {
        return std::uniform_int_distribution<size_t>{0, n ? n - 1 : 0}(rng);
    };

    int n = std::uniform_int_distribution<int>{1, 8}(rng);
    for (; n-- > 0;) {
        if (base.empty()) base = "x";
        size_t p = below(base.size());
        switch (std::uniform_int_distribution<int>{0, 5}(rng)) {
        case 0:                                            // 翻转一个字节
            base[p] = random_syntax_char(rng());
            break;
        case 1:                                            // 插入随机字符
            base.insert(base.begin() + p, random_syntax_char(rng()));
            break;
        case 2:                                            // 删除一段(≤16 字节)
            base.erase(p, std::min<size_t>(below(16) + 1, base.size() - p));
            break;
        case 3: {                                          // 复制一段并接在尾部
            size_t l = std::min<size_t>(below(32) + 1, base.size() - p);
            base += base.substr(p, l);
            break;
        }
        case 4: {                                          // 从其他语料拼接一段
            if (corpus.empty()) break;
            std::string const& other = corpus[below(corpus.size())];
            if (other.empty()) break;
            size_t op = below(other.size());
            base += other.substr(op, below(64) + 1);
            break;
        }
        default:                                           // 截断
            base.resize(p + 1);
            break;
        }
        if (base.size() > kMaxInput) {
            base.resize(kMaxInput);
            break;
        }
    }
    return base;
}

} // namespace

int run_one(std::string const& src, std::string const& name)
{
    try {
        auto toks = lex::lex(src);
        auto ast = parse::parse(std::move(toks), name);
        (void)sema::check(ast, {});
        return 0;
    } catch (lex::LexError const&) {
        return 0;                       // 预期诊断:未闭合字符串/非法字符等
    } catch (parse::ParseError const&) {
        return 0;                       // 预期诊断:语法错误
    } catch (std::bad_alloc const&) {
        return 0;                       // 超大输入:内存上限,非崩溃
    } catch (std::exception const& e) {
        std::fprintf(stderr, "fuzz: unexpected exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "fuzz: unexpected non-standard exception\n");
        return 1;
    }
}

Outcome run(std::vector<std::string> const& corpus_files, unsigned seed, int iters,
            std::string const& crash_dir)
{
    Outcome out;

    std::vector<std::string> corpus;
    for (auto const& f : corpus_files) {
        auto txt = read_file(f);
        if (txt) corpus.push_back(std::move(*txt));
    }
    if (corpus.empty()) return out;

    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> pick(0, corpus.size() - 1);

    for (int i = 0; i < iters; ++i) {
        std::string input = mutate(corpus[pick(rng)], rng, corpus);
        ++out.iterations;
        if (run_one(input, "<fuzz>") != 0) {
            ++out.crashes;
            fs::path crash = fs::path(crash_dir)
                / ("crash-" + std::to_string(out.crashes) + ".cpp2");
            fs::create_directories(crash_dir);
            if (write_file(crash, input)) out.crash_files.push_back(crash.string());
            std::fprintf(stderr, "fuzz: CRASH input saved to %s\n", crash.string().c_str());
        }
    }
    return out;
}

} // namespace cpp2::fuzz
