// C++2 language extension: diagnostics / outline / completions / member completion.
// No dependencies - child-process based, regex parsing of cpp2 tool output.
//
// 补全为上下文感知(按接收者类型 / 语法位出候选,同成熟语言):
//   - `.`        → 接收者成员:M9 自研 std(string/vector/map 双轨 API, cpp2 风格层优先)、
//                  `T?`(optional)、pair(map for-in 元素)、unique/shared 自动解引用、
//                  用户类型字段/方法(含签名与返回类型)、链式调用返回值推断(s.find("x").)
//   - `::`       → std::(函数带签名/类型/常量)或 enum 成员
//   - `import `  → std + 工作区 .cpp2 文件里声明的 module 名
//   - `@`        → unsafe / unchecked
//   - match 臂 `.x` → 全部枚举成员
//   - `->` / `name: ` → 类型位(builtin + 工作区类型/枚举/variant;参数位附参数模式)
//   - 其余       → 关键词 / 内建类型 / 片段 / std 函数 / 本文件符号(函数带签名)
const vscode = require("vscode");
const cp = require("child_process");
const path = require("path");
const fs = require("fs");

const diagCollection = vscode.languages.createDiagnosticCollection("cpp2");
let checkTimer = null;

function toolPath() {
    const cfg = vscode.workspace.getConfiguration("cpp2");
    return cfg.get("toolPath", "cpp2");
}

function runTool(args, cwd) {
    return new Promise((resolve) => {
        cp.execFile(toolPath(), args, { cwd, maxBuffer: 16 * 1024 * 1024 },
            (err, stdout, stderr) => resolve({ code: err ? err.code : 0, stdout, stderr }));
    });
}

// ── Diagnostics: parse `file:line:col: severity: msg` from cpp2 check ──
const DIAG_RE = /^(.+?):(\d+):(\d+):\s*(error|warning):\s*(.*)$/gm;

async function checkDocument(doc) {
    if (!doc || doc.languageId !== "cpp2") return;
    const folder = vscode.workspace.getWorkspaceFolder(doc.uri);
    const cwd = folder ? folder.uri.fsPath : path.dirname(doc.uri.fsPath);

    const res = await runTool(["check", doc.uri.fsPath], cwd);
    const byFile = new Map();
    let m;
    DIAG_RE.lastIndex = 0;
    while ((m = DIAG_RE.exec(res.stderr)) !== null) {
        const [, file, line, col, sev, msg] = m;
        const key = path.resolve(cwd, file.trim());
        if (!byFile.has(key)) byFile.set(key, []);
        byFile.get(key).push(new vscode.Diagnostic(
            new vscode.Range(+line - 1, +col - 1, +line - 1, +col),
            msg, sev === "error" ? vscode.DiagnosticSeverity.Error
                                 : vscode.DiagnosticSeverity.Warning));
    }
    for (const [file, diags] of byFile) {
        diagCollection.set(vscode.Uri.file(file), diags);
    }
    for (const d of vscode.workspace.textDocuments) {
        if (d.languageId === "cpp2" && !byFile.has(d.uri.fsPath)) {
            diagCollection.delete(d.uri);
        }
    }
}

function scheduleCheck(doc) {
    const cfg = vscode.workspace.getConfiguration("cpp2");
    if (!cfg.get("checkOnChange", true)) return;
    clearTimeout(checkTimer);
    checkTimer = setTimeout(() => checkDocument(doc), 500);
}

// ══ Index: types (fields/methods/base) + variables + functions ═══════════════
// Pure functions (unit-testable via node; see tests in tests/vscode_ext_test.js)

// Name: type = { ... } -> { base, fields:[{name,type}], methods:[name], methodSigs }
function parseTypes(text) {
    const types = new Map();
    const re = /^([ \t]*)(?:export[ \t]+)?([A-Za-z_]\w*)[ \t]*:[ \t]*type[ \t]*(?::[ \t]*([A-Za-z_][\w.:]*)[ \t]*)?=[ \t]*\{/gm;
    let m;
    while ((m = re.exec(text)) !== null) {
        const start = m.index + m[0].length;
        let depth = 1, i = start;
        while (i < text.length && depth > 0) {
            const c = text[i];
            if (c === "/" && text[i + 1] === "/") {
                const e = text.indexOf("\n", i);
                i = e < 0 ? text.length : e;
                continue;
            }
            if (c === "/" && text[i + 1] === "*") {
                const e = text.indexOf("*/", i + 2);
                i = e < 0 ? text.length : e + 2;
                continue;
            }
            if (c === '"' || c === "'") {
                const q = c;
                i++;
                while (i < text.length && text[i] !== q) {
                    if (text[i] === "\\") i++;
                    i++;
                }
                i++;
                continue;
            }
            if (c === "{") depth++;
            else if (c === "}") depth--;
            i++;
        }
        const body = text.slice(start, Math.max(start, i - 1));
        const t = { base: m[3] ? m[3].split(":").pop() : null, fields: [], methods: [], methodSigs: {} };
        const fre = /^[ \t]+([A-Za-z_]\w*)[ \t]*:[ \t]*([^=({\n]+?)[ \t]*=[ \t]/gm;
        let fm;
        while ((fm = fre.exec(body)) !== null) {
            if (fm[1] === "invariant" || fm[1] === "pre" || fm[1] === "post") continue;
            t.fields.push({ name: fm[1], type: fm[2].trim() });
        }
        const mre = /^[ \t]+([A-Za-z_]\w*)[ \t]*:[ \t]*\(([^()]*)\)([^{\n]*)/gm;
        while ((fm = mre.exec(body)) !== null) {
            if (fm[1] === "destructor") continue;
            const rest = (fm[3] || "").split("=")[0];
            const rm = /->\s*([\s\S]+?)(?:\s+(?:mutates|throws)|\s*$)/.exec(rest);
            t.methods.push(fm[1]);
            t.methodSigs[fm[1]] = {
                params: fm[2].trim(),
                ret: rm ? rm[1].trim() : "",
                mutates: /\bmutates\b/.test(rest),
                throws: /\bthrows\b/.test(rest),
            };
        }
        types.set(m[2], t);
    }
    return types;
}

// Name: enum [: u8] = { a, b, c } -> Map enumName -> [members]
function parseEnums(text) {
    const enums = new Map();
    const re = /^([ \t]*)(?:export[ \t]+)?([A-Za-z_]\w*)[ \t]*:[ \t]*enum(?:[ \t]*:[ \t]*[^={\n]+)?[ \t]*=[ \t]*\{([^}]*)\}/gm;
    let m;
    while ((m = re.exec(text)) !== null) {
        const members = m[3].split(",").map(s => s.trim()).filter(Boolean);
        enums.set(m[2], members);
    }
    return enums;
}

// Name: variant = { ... } -> [names]
function parseVariants(text) {
    const out = [];
    const re = /^[ \t]*(?:export[ \t]+)?([A-Za-z_]\w*)[ \t]*:[ \t]*variant\b/gm;
    let m;
    while ((m = re.exec(text)) !== null) out.push(m[1]);
    return out;
}

// 顶层函数声明(列 0 起,含 export):name: <T: C> (params) -> ret throws = / {
// 方法(缩进)由 parseTypes.methodSigs 承载;返回类型供 `f(...).` 链式推断。
function parseFunctions(text) {
    const funcs = new Map();
    const re = /^(?:export[ \t]+)?([A-Za-z_]\w*)[ \t]*:[ \t]*(?:<[^<>()\n]*>[ \t]*)?\(([^()]*)\)([^{=\n]*)/gm;
    let m;
    while ((m = re.exec(text)) !== null) {
        const rest = m[3] || "";
        const rm = /->\s*([\s\S]+?)(?:\s+(?:mutates|throws)|\s*$)/.exec(rest);
        funcs.set(m[1], {
            params: m[2].trim(),
            ret: rm ? rm[1].trim() : "",
            throws: /\bthrows\b/.test(rest),
        });
    }
    return funcs;
}

// unwrap unique<T>/shared<T>/weak<T>
function pointeeOf(typeText) {
    const m = /^(?:unique|shared|weak)\s*<\s*([^>]+)\s*>$/.exec(typeText.trim());
    return m ? m[1].trim() : null;
}

// "map<string, int>" 内层实参按顶层逗号拆分
function splitGenericArgs(text) {
    const out = [];
    let depth = 0, cur = "";
    for (const ch of text) {
        if (ch === "<") depth++;
        else if (ch === ">") depth--;
        if (ch === "," && depth === 0) { out.push(cur.trim()); cur = ""; continue; }
        cur += ch;
    }
    if (cur.trim()) out.push(cur.trim());
    return out;
}

// "vector<int>" -> { base: "vector", params: { T: "int" } }
function genericParamsOf(t) {
    const m = /^(\w+)<([\s\S]*)>$/.exec(t);
    if (!m) return { base: t, params: {} };
    const gens = { vector: ["T"], list: ["T"], map: ["K", "V"], pair: ["K", "V"], optional: ["T"] }[m[1]];
    const params = {};
    if (gens) {
        const args = splitGenericArgs(m[2]);
        gens.forEach((g, i) => { params[g] = (args[i] || "").trim() || g; });
    }
    return { base: m[1], params };
}

function subst(sig, params) {
    return sig.replace(/\b(T|K|V)\b/g, (w) => params[w] || w);
}

// 元数据 → "(x: int) mutates" 风格签名串
function sigDetail(sig) {
    return "(" + sig.params + ")"
        + (sig.ret ? " -> " + sig.ret : "")
        + (sig.mutates ? " mutates" : "")
        + (sig.throws ? " throws" : "");
}

// 集合元素类型(for x in coll):string→char / vector<T>→T / map<K,V>→pair<K,V> / T[N]→T
function elementTypeOf(t) {
    if (!t) return null;
    t = t.trim().replace(/^const\s+/, "").replace(/^list</, "vector<");
    if (t === "string") return "char";
    const am = /^(.+?)\[(\d+)\]$/.exec(t);         // T[N](M10 固定长度数组)
    if (am) return am[1].trim();
    const { base, params } = genericParamsOf(t);
    if (base === "vector" && params.T) return params.T;
    if (base === "map" && params.K && params.V) return "pair<" + params.K + ", " + params.V + ">";
    return null;
}

// 变量/参数/for-in 循环变量/`:=` 右值推断 -> Map varName -> type text
// ctx = { types, funcs }(可省;省略时仅显式标注与聚合构造)
function parseVars(text, ctx) {
    const vars = new Map();
    let m;
    // x: Type := / x: Type = / x: const Type =(M10:T[N] 后缀,?/* 修饰元素)
    const re1 = /\b([A-Za-z_]\w*)[ \t]*:[ \t]*((?:const[ \t]+)?[A-Za-z_][\w.]*(?:<[^=;{}\n]*>)?[?]?(?:\[\d+\])?(?:[ \t]+const)?)[ \t]*:?=/g;
    while ((m = re1.exec(text)) !== null) {
        const ty = m[2].replace(/\bconst\b/g, "").trim();
        if (ty) vars.set(m[1], ty);
    }
    // 函数参数(签名处类型后随 `,`/`)`,无 `=`):f: (x: T, out q: U) -> R
    const re0 = /([A-Za-z_]\w*)[ \t]*:[ \t]*((?:const[ \t]+)?[A-Za-z_][\w.]*(?:<[^(){}\n=]*>)?[?]?(?:\[\d+\])?(?:[ \t]+const)?)[ \t]*[,)]/g;
    while ((m = re0.exec(text)) !== null) {
        const ty = m[2].replace(/\bconst\b/g, "").trim();
        if (ty && !vars.has(m[1])) vars.set(m[1], ty);
    }
    // x := Point{...} (inferred aggregate)
    const re2 = /\b([A-Za-z_]\w*)[ \t]*:=[ \t]*([A-Za-z_]\w*)[ \t]*\{/g;
    while ((m = re2.exec(text)) !== null) vars.set(m[1], m[2]);
    if (ctx) {
        // x := <expr> — 借助类型解析推断(make_unique / 调用返回值 / 方法链)
        // 右值不排除 `=`(聚合指定初始化器 Point{.x = 1} 合法)
        const re3 = /\b([A-Za-z_]\w*)[ \t]*:=[ \t]*([^;\n]+)/g;
        while ((m = re3.exec(text)) !== null) {
            if (vars.has(m[1])) continue;
            const t = resolveExprType(m[2].trim(), { types: ctx.types, vars, funcs: ctx.funcs });
            if (t) vars.set(m[1], t);
        }
        // for x in coll — 集合元素类型
        const re4 = /\bfor[ \t]+([A-Za-z_]\w*)[ \t]+in[ \t]+([A-Za-z_][\w.]*)/g;
        while ((m = re4.exec(text)) !== null) {
            if (vars.has(m[1])) continue;
            const coll = vars.get(m[2]) || (ctx.types.has(m[2]) ? m[2] : null);
            const et = elementTypeOf(coll);
            if (et) vars.set(m[1], et);
        }
    }
    return vars;
}

// ══ std 元数据:M9 自研 std 面(rt/cpp2/std)+ 常用包装类型 ═══════════════════
// 成员四元组:[name, sig, layer, doc]。layer "cpp2" = cpp2 风格层(推荐,排序在前);
// "std" = std 兼容层(既有语料零迁移);"field" = 数据成员。sig 中 T/K/V 按接收者泛型实参替换。
const STD_MEMBERS = {
    string: {
        header: "cpp2::string(rt/cpp2/std/string.hpp)",
        members: [
            ["len", "() -> u64", "cpp2", "长度"],
            ["at", "(i: u64) -> char", "cpp2", "受检下标:越界 trap(@unchecked 块内退出)"],
            ["find", "(sub: string_view) -> u64?", "cpp2", "查找子串;缺失返回 none(T? 取代 npos)"],
            ["substr", "(pos: u64, n: u64 = npos) -> string", "cpp2", "子串"],
            ["starts_with", "(p: string_view) -> bool", "cpp2", "前缀判断"],
            ["ends_with", "(p: string_view) -> bool", "cpp2", "后缀判断"],
            ["to_string", "() -> string", "cpp2", "内部 std::string(互操作)"],
            ["size", "() -> u64", "std", "std 兼容层"],
            ["length", "() -> u64", "std", "std 兼容层"],
            ["empty", "() -> bool", "std", "std 兼容层"],
            ["data", "() -> char const*", "std", "std 兼容层"],
            ["c_str", "() -> char const*", "std", "std 兼容层"],
            ["push_back", "(c: char) -> void", "std", "std 兼容层"],
            ["resize", "(n: u64, fill: char) -> void", "std", "std 兼容层"],
            ["begin", "() -> iterator", "std", "std 兼容层(for-in 走语言层)"],
            ["end", "() -> iterator", "std", "std 兼容层"],
        ],
    },
    vector: {
        header: "cpp2::vector<T>(rt/cpp2/std/vector.hpp);v[i] 语言层受检,越界 trap",
        members: [
            ["len", "() -> u64", "cpp2", "元素个数"],
            ["push", "(v: T) -> void", "cpp2", "尾插"],
            ["pop", "() -> T?", "cpp2", "弹出尾元素;空容器返回 none(值语义取代 UB)"],
            ["at", "(i: u64) -> T", "cpp2", "受检下标:越界 trap"],
            ["first", "() -> T?", "cpp2", "首元素;空容器返回 none"],
            ["last", "() -> T?", "cpp2", "尾元素;空容器返回 none"],
            ["size", "() -> u64", "std", "std 兼容层"],
            ["empty", "() -> bool", "std", "std 兼容层"],
            ["clear", "() -> void", "std", "std 兼容层"],
            ["push_back", "(v: T) -> void", "std", "std 兼容层"],
            ["resize", "(n: u64, fill: T) -> void", "std", "std 兼容层"],
            ["begin", "() -> iterator", "std", "std 兼容层(for-in 走语言层)"],
            ["end", "() -> iterator", "std", "std 兼容层"],
        ],
    },
    map: {
        header: "cpp2::map<K, V>(rt/cpp2/std/map.hpp,有序);无 m[k] 下标——缺失键走 get(值)/at(trap)",
        members: [
            ["len", "() -> u64", "cpp2", "键值对个数"],
            ["insert", "(k: K, v: V) -> bool", "cpp2", "插入;键已存在返回 false"],
            ["get", "(k: K) -> V?", "cpp2", "取值;缺失键返回 none(缺失是值,不静默插入)"],
            ["at", "(k: K) -> V", "cpp2", "受检索取;缺失 trap"],
            ["contains", "(k: K) -> bool", "cpp2", "键存在判断"],
            ["remove", "(k: K) -> bool", "cpp2", "删除;键存在返回 true"],
            ["size", "() -> u64", "std", "std 兼容层"],
            ["empty", "() -> bool", "std", "std 兼容层"],
            ["clear", "() -> void", "std", "std 兼容层"],
            ["begin", "() -> iterator", "std", "std 兼容层(for-in 产出 pair<K,V>:first/second)"],
            ["end", "() -> iterator", "std", "std 兼容层"],
        ],
    },
    array: {
        header: "T[N](rt/cpp2/std/array.hpp,M10 固定长度数组);a[i] 语言层受检,越界 trap;len 为编译期常量",
        members: [
            ["len", "() -> u64", "cpp2", "元素个数(编译期常量 N)"],
            ["at", "(i: u64) -> T", "cpp2", "受检下标:越界 trap"],
            ["first", "() -> T?", "cpp2", "首元素"],
            ["last", "() -> T?", "cpp2", "尾元素"],
            ["size", "() -> u64", "std", "std 兼容层"],
            ["empty", "() -> bool", "std", "std 兼容层"],
            ["fill", "(v: T) -> void", "std", "std 兼容层"],
            ["begin", "() -> iterator", "std", "std 兼容层(for-in 走语言层)"],
            ["end", "() -> iterator", "std", "std 兼容层"],
        ],
    },
    pair: {
        header: "pair<K, V>(map for-in 元素)",
        members: [
            ["first", "K", "field", "键(K)"],
            ["second", "V", "field", "值(V)"],
        ],
    },
    optional: {
        header: "T?(optional);惯用 if-let / or / '?' / '!' / match ok-err",
        members: [
            ["has_value", "() -> bool", "cpp2", "是否存在值"],
            ["value", "() -> T", "std", "取值(std::optional)"],
            ["value_or", "(d: T) -> T", "std", "缺省取值(or 运算符的成员写法)"],
        ],
    },
    string_view: {
        header: "std::string_view(互操作)",
        members: [
            ["size", "() -> u64", "std", "std 兼容层"],
            ["length", "() -> u64", "std", "std 兼容层"],
            ["empty", "() -> bool", "std", "std 兼容层"],
            ["data", "() -> char const*", "std", "std 兼容层"],
            ["substr", "(pos: u64, n: u64) -> string_view", "std", "std 兼容层"],
            ["starts_with", "(p: string_view) -> bool", "std", "std 兼容层"],
            ["ends_with", "(p: string_view) -> bool", "std", "std 兼容层"],
        ],
    },
    weak: {
        header: "weak<T>(std::weak_ptr)",
        members: [
            ["lock", "() -> T?", "std", "提升强引用;失效返回 none"],
        ],
    },
    cpp2error: {
        header: "错误值(expected 的错误侧):if-let else 的 it / match err e 绑定",
        members: [
            ["message", "() -> string", "cpp2", "错误消息"],
        ],
    },
};

// std:: 直写(互操作 escape hatch)常用函数
const STD_FUNCTIONS = [
    ["print", "(fmt: string_view, ...args) -> void", "格式化输出(无换行)"],
    ["println", "(fmt: string_view, ...args) -> void", "格式化输出(换行)"],
    ["to_string", "(v) -> string", "转字符串;UFCS:n.to_string()"],
    ["sqrt", "(x: double) -> double", "平方根"],
    ["pow", "(x: double, e: double) -> double", "幂"],
    ["abs", "(x) -> auto", "绝对值"],
    ["make_unique", "(args...) -> unique<T>", "堆构造 unique<T>(语言类型写作 unique<T>)"],
    ["make_shared", "(args...) -> shared<T>", "堆构造 shared<T>(语言类型写作 shared<T>)"],
    ["move", "(v: T) -> T", "显式移动:consume(move buf),buf 之后不可再用"],
    ["forward", "(v: T) -> T", "泛型完美转发"],
    ["holds_alternative", "(v) -> bool", "variant 当前候选判断"],
    ["get", "(v) -> T&", "variant 取候选"],
];
const STD_STD_TYPES = ["string", "string_view", "vector", "optional", "expected",
                       "variant", "unique_ptr", "shared_ptr"];
const STD_STD_CONSTS = ["nullopt"];

// ══ 接收者类型解析 ═══════════════════════════════════════════════════════════

// 接收者. 方法返回类型(泛型实参替换;用户方法查 methodSigs;标量 to_string UFCS 桥)
function methodReturnType(t, name, ctx) {
    t = (t || "").trim().replace(/^const\s+/, "").replace(/^list</, "vector<");
    if (!t) return null;
    if (t.endsWith("?")) {
        for (const [n, sig, layer] of STD_MEMBERS.optional.members) {
            if (n === name && layer !== "field") {
                const parts = sig.split("->");
                return parts.length === 2 ? subst(parts[1].trim(), { T: t.slice(0, -1).trim() }) : null;
            }
        }
        return null;
    }
    let m = /^(unique|shared)<([\s\S]+)>$/.exec(t);
    if (m) return methodReturnType(m[2].trim(), name, ctx);       // 自动解引用
    m = /^(.+?)\[(\d+)\]$/.exec(t);                // T[N](M10)
    if (m) {
        for (const [n, sig, layer] of STD_MEMBERS.array.members) {
            if (n === name && layer !== "field") {
                const parts = sig.split("->");
                return parts.length === 2 ? subst(parts[1].trim(), { T: m[1].trim(), N: m[2] }) : null;
            }
        }
        return null;
    }
    m = /^(\w+)(?:<([\s\S]*)>)?$/.exec(t);
    if (m && STD_MEMBERS[m[1]]) {
        const { base, params } = genericParamsOf(t);
        for (const [n, sig, layer] of STD_MEMBERS[base].members) {
            if (n === name && layer !== "field") {
                const parts = sig.split("->");
                return parts.length === 2 ? subst(parts[1].trim(), params) : null;
            }
        }
        return null;
    }
    if (ctx && ctx.types && ctx.types.has(t)) {
        let cur = t, guard = 0;
        while (cur && ctx.types.has(cur) && guard++ < 16) {
            const sig = ctx.types.get(cur).methodSigs[name];
            if (sig) return sig.ret || null;
            cur = ctx.types.get(cur).base;
        }
        return null;
    }
    if (name === "to_string") return "string";     // UFCS 桥:std::to_string(x)
    return null;
}

// 接收者. 字段类型(用户类型沿基类链)
function fieldTypeOf(t, field, ctx) {
    t = (t || "").trim().replace(/^const\s+/, "").replace(/^list</, "vector<");
    let m = /^(unique|shared)<([\s\S]+)>$/.exec(t);
    if (m) return fieldTypeOf(m[2].trim(), field, ctx);
    if (ctx && ctx.types && ctx.types.has(t)) {
        let cur = t, guard = 0;
        while (cur && ctx.types.has(cur) && guard++ < 16) {
            for (const f of ctx.types.get(cur).fields) {
                if (f.name === field) return f.type;
            }
            cur = ctx.types.get(cur).base;
        }
    }
    return null;
}

// 表达式类型推断(正则级,够用即止):字面量 / 调用 / 方法链 / 下标 / 解引用 / 聚合
function resolveExprType(expr, ctx, depth = 0) {
    if (!expr || depth > 8) return null;
    let e = expr.trim();
    while (e.length > 1 && e[0] === "(" && e[e.length - 1] === ")") {   // 去外层括号
        let d = 0, paired = true;
        for (let i = 0; i < e.length; i++) {
            if (e[i] === "(") d++;
            else if (e[i] === ")") { d--; if (d === 0 && i !== e.length - 1) { paired = false; break; } }
        }
        if (!paired) break;
        e = e.slice(1, -1).trim();
    }
    if (/^"[^"]*"?$/.test(e)) return "string";
    if (/^'(?:\\.|[^'])'?$/.test(e)) return "char";
    if (/^-?\d+\.\d*(?:[eE][+-]?\d+)?$/.test(e) || /^-?\d+[eE][+-]?\d+$/.test(e)) return "double";
    if (/^-?\d+'?\d*$/.test(e)) return "int";
    if (e === "true" || e === "false") return "bool";
    if (e === "it") return "cpp2error";            // if-let else 分支绑定的错误值
    let m = /^\*\s*([\s\S]+)$/.exec(e);            // *p:解引用(optional/智能指针)
    if (m) {
        const t = resolveExprType(m[1], ctx, depth + 1);
        if (!t) return null;
        return pointeeOf(t) || (t.endsWith("?") ? t.slice(0, -1) : null);
    }
    m = /^([\s\S]+?)\s*\[[^\[\]]*\]$/.exec(e);     // 下标:a[i] → 元素类型(数组/vector/string)
    if (m) {
        const t = resolveExprType(m[1], ctx, depth + 1);
        return t ? elementTypeOf(t) : null;
    }
    m = /^([\s\S]+?)\s*\(([^()]*)\)$/.exec(e);     // 调用:方法链 / 自由函数 / UFCS
    if (m) {
        const args = m[2].trim();
        const callee = m[1].trim();
        const cm = /^([\s\S]*)\.\s*(\w+)$/.exec(callee);
        if (cm) {
            const recv = resolveExprType(cm[1], ctx, depth + 1);
            return recv ? methodReturnType(recv, cm[2], ctx) : null;
        }
        if (callee === "make_unique" || callee === "make_shared") {
            const am = /^\s*([A-Za-z_]\w*)/.exec(args);
            return am ? (callee === "make_unique" ? "unique<" : "shared<") + am[1] + ">" : null;
        }
        if (callee === "std::to_string") return "string";
        if (callee === "move" || callee === "copy") return resolveExprType(args, ctx, depth + 1);
        const f = ctx.funcs && ctx.funcs.get(callee);
        if (f && f.ret) return f.ret;
        // UFCS:f(x, ...) ≡ x.f(...)
        const first = args ? args.split(",")[0].trim() : "";
        if (first) {
            const t0 = resolveExprType(first, ctx, depth + 1);
            if (t0) {
                const mr = methodReturnType(t0, callee, ctx);
                if (mr) return mr;
            }
        }
        return null;
    }
    m = /^([A-Za-z_]\w*)\s*\{/.exec(e);            // 聚合构造 Type{...}
    if (m) return m[1];
    m = /^([A-Za-z_]\w*(?:\s*\.\s*\w+)*)$/.exec(e); // 名字链:x / x.field
    if (m) {
        const parts = e.split(".").map(s => s.trim());
        let t = null;
        for (const p of parts) {
            if (t === null) {
                t = ctx.vars.get(p)
                 || (ctx.types && ctx.types.has(p) ? p : null);
                if (!t) return null;
            } else {
                t = fieldTypeOf(t, p, ctx) || methodReturnType(t, p, ctx);
                if (!t) return null;
            }
        }
        return t;
    }
    return null;
}

// members of receiver type, walking base chain; dedup by name
function resolveMembers(types, typeName) {
    const out = [];
    const seen = new Set();
    let t = typeName && pointeeOf(typeName) || typeName;
    let guard = 0;
    while (t && types.has(t) && guard++ < 16) {
        const def = types.get(t);
        for (const f of def.fields) {
            if (!seen.has(f.name)) {
                seen.add(f.name);
                out.push({ name: f.name, kind: "field", detail: f.type });
            }
        }
        for (const name of def.methods) {
            if (!seen.has(name)) {
                seen.add(name);
                out.push({ name, kind: "method", detail: "() (method)" });
            }
        }
        t = def.base;
    }
    return out;
}

// 成员候选元数据:[{name, kind, detail, doc, layer, header}]
function membersForType(typeText, ctx) {
    const out = [];
    const seen = new Set();
    let t = (typeText || "").trim().replace(/^const\s+/, "").replace(/^list</, "vector<");
    if (!t) return out;

    const pushStd = (key, params) => {
        const def = STD_MEMBERS[key];
        if (!def) return;
        for (const [name, sig, layer, doc] of def.members) {
            if (seen.has(name)) continue;
            seen.add(name);
            out.push({
                name,
                kind: layer === "field" ? "field" : "method",
                detail: sig ? subst(sig, params) : "",
                doc, header: def.header,
                layer: layer === "field" ? "cpp2" : layer,
            });
        }
    };

    if (t.endsWith("?")) { pushStd("optional", { T: t.slice(0, -1).trim() }); return out; }
    let m = /^(unique|shared|weak)<([\s\S]+)>$/.exec(t);
    if (m) {
        if (m[1] === "weak") { pushStd("weak", { T: m[2].trim() }); return out; }
        return membersForType(m[2], ctx);          // unique/shared 自动解引用
    }
    m = /^(.+?)\[(\d+)\]$/.exec(t);                // T[N](M10)
    if (m) { pushStd("array", { T: m[1].trim(), N: m[2] }); return out; }
    m = /^(\w+)(?:<([\s\S]*)>)?$/.exec(t);
    if (m && STD_MEMBERS[m[1]]) {
        const { base, params } = genericParamsOf(t);
        pushStd(base, params);
        return out;
    }
    if (ctx && ctx.types && ctx.types.has(t)) {    // 用户类型沿基类链
        let cur = t, guard = 0;
        while (cur && ctx.types.has(cur) && guard++ < 16) {
            const def = ctx.types.get(cur);
            for (const f of def.fields) {
                if (!seen.has(f.name)) {
                    seen.add(f.name);
                    out.push({ name: f.name, kind: "field", detail: f.type,
                               doc: null, layer: "user", header: cur });
                }
            }
            for (const name of def.methods) {
                if (seen.has(name)) continue;
                seen.add(name);
                const sig = def.methodSigs[name];
                out.push({ name, kind: "method",
                           detail: sig ? sigDetail(sig) : "() (method)",
                           doc: null, layer: "user", header: cur });
            }
            cur = def.base;
        }
        return out;
    }
    return out;
}

// ══ 补全上下文工具 ═══════════════════════════════════════════════════════════

// 光标前缀里最后一个 `.` 前的接收者表达式(支持链式调用/下标/字面量/解引用)
function exprBeforeDot(prefix) {
    let i = prefix.length;
    while (i > 0 && /[\s\w]/.test(prefix[i - 1])) i--;
    const member = prefix.slice(i);
    if (i === 0 || prefix[i - 1] !== ".") return null;
    i--;                                           // 消费 `.`
    while (i > 0 && /\s/.test(prefix[i - 1])) i--;
    const end = i;
    if (i > 0 && (prefix[i - 1] === ")" || prefix[i - 1] === "]")) {
        const open = prefix[i - 1] === ")" ? "(" : "[";
        const close = prefix[i - 1] === ")" ? ")" : "]";
        let depth = 0;
        while (i > 0) {
            const c = prefix[--i];
            if (c === close) depth++;
            else if (c === open && --depth === 0) break;
        }
        while (i > 0 && /[\w.]/.test(prefix[i - 1])) i--;
    } else if (i > 0 && (prefix[i - 1] === '"' || prefix[i - 1] === "'")) {
        const q = prefix[--i];
        while (i > 0) { const c = prefix[--i]; if (c === q) break; }
    } else {
        while (i > 0 && /[\w.]/.test(prefix[i - 1])) i--;
        while (i > 0 && prefix[i - 1] === "*") i--;    // *p. 解引用接收者
    }
    const expr = prefix.slice(i, end).trim();
    if (!expr) return null;
    return { expr, member };
}

// 行前缀处于字符串/行注释中 → 不出候选
function inStringOrComment(prefix) {
    const noEscape = prefix.replace(/\\./g, "__");
    if (noEscape.replace(/"(?:[^"]*)"/g, '""').includes("//")) return true;
    let q = 0;
    for (const ch of noEscape) if (ch === '"') q++;
    return q % 2 === 1;
}

// 前缀中未闭合的圆括号深度(参数位判定;忽略字符串)
function parenDepth(prefix) {
    let d = 0;
    const s = prefix.replace(/\\./g, "__").replace(/"(?:[^"]*)"/g, '""');
    for (const ch of s) {
        if (ch === "(") d++;
        else if (ch === ")") d--;
    }
    return d;
}

// 工作区 module 名收集(单文件 = 单模块单元)
function collectModuleNames(texts) {
    const names = new Set(["std"]);
    for (const text of texts) {
        const m = /^[ \t]*module[ \t]+([A-Za-z_][\w.]*)/m.exec(text);
        if (m) names.add(m[1]);
    }
    return [...names].sort();
}

// ══ Completions ══════════════════════════════════════════════════════════════
const KEYWORDS = [
    "module", "import", "export", "type", "enum", "variant", "concept",
    "if", "else", "for", "while", "in", "return", "break", "continue",
    "match", "throws", "mutates", "const", "true", "false", "virtual",
    "none", "some", "self", "it",
    "inout", "out", "move", "copy", "forward", "as", "requires",
    "pre", "post", "invariant", "destructor",
];
const KEYWORD_DETAILS = {
    "as": "显式类型转换(n as i32;收窄溢出仍 trap)",
    "it": "if-let else 分支绑定的错误值(it.message())",
    "requires": "泛型约束子句:requires Ordered<T>",
    "pre": "前置契约:pre: amount > 0",
    "post": "后置契约:post: result == …(old() 入口求值)",
    "invariant": "类型不变量:公开方法出入口检查",
    "mutates": "修改 self 的方法必须标注(与 inout 对齐)",
    "throws": "函数走错误通道:返回 expected,'?' / '!' / or / if-let / match 处理",
    "none": "optional 空值(x: int? = none)",
    "some": "optional 有值包装",
    "self": "方法内显式接收者;lambda 捕获写 [self]",
    "virtual": "虚分发声明",
    "destructor": "析构器:destructor: () mutates = { ... }(作用域结束确定性调用)",
    "move": "移动语义:consume(move buf),调用侧显式写",
    "inout": "参数模式:读写,可修改调用者对象",
    "out": "参数模式:输出,被调用方必须赋值",
    "copy": "参数模式:明确独立副本",
    "forward": "参数模式:泛型完美转发",
};
const TYPES = [
    "int", "double", "float", "bool", "char", "void",
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
    "string", "string_view", "vector", "list", "map", "byte",
    "unique", "shared", "weak", "arena", "arena_ptr", "auto",
];
const TYPE_DETAILS = {
    "string": "cpp2::string(rt/cpp2/std):len / at / find→u64? / substr / starts_with",
    "string_view": "std::string_view(互操作)",
    "vector": "cpp2::vector<T>:push / pop→T? / at / first / last",
    "list": "vector 的别名(cpp2::vector<T>)",
    "map": "cpp2::map<K,V>(有序):get→V? / at / contains / insert / remove;无 m[k]",
    "unique": "std::unique_ptr<T>;. 自动解引用 + 空检",
    "shared": "std::shared_ptr<T>;. 自动解引用",
    "weak": "std::weak_ptr<T>;.lock() → T?",
    "arena": "区域分配器(内存层)",
    "arena_ptr": "arena 内指针(逃逸检查)",
    "auto": "类型推断(x := …)",
    "byte": "std::byte",
};
const PARAM_MODES = [
    ["in", "只读(默认,可省略)"],
    ["inout", "读写:可修改调用者的对象"],
    ["out", "输出:被调用方必须赋值"],
    ["move", "移动:调用侧必须显式写 move"],
    ["copy", "拷贝:明确要一份独立副本"],
    ["forward", "泛型完美转发"],
];
const ATTRS = [
    ["unsafe", "逃生舱块:指针运算 / 无检查下标 / legacy 调用;白纸黑字可审计"],
    ["unchecked", "块级退出下标与算术检查"],
];
const BUILTINS = [
    ["err", 'err("message")', "构造错误值(throws 函数内 return err(…))"],
    ["old", "old(expr)", "契约 post:在入口求值并缓存"],
    ["result", "result", "契约 post:绑定返回值"],
];
const SNIPPETS = [
    ["fn", "function", "${name}: (${params}) -> ${ret} = {\n\t$0\n}"],
    ["gfn", "generic function", "${name}: <T${2:: Concept}> (${args}) -> T\n\trequires Ordered<T>\n= {\n\t$0\n}"],
    ["struct", "type definition", "${Name}: type = {\n\t${field}: int = 0;$0\n}"],
    ["enum", "enum definition", "${Name}: enum = { ${a}, ${b}$0 }"],
    ["variant", "variant definition", "${Name}: variant = { ${int}, string }"],
    ["concept", "concept definition", "${Name}: concept = {\n\toperator<: (that: self) -> bool;\n}"],
    ["matchv", "variant match", "match ${v} {\n\t${Type}(x)\t=> $0;\n\t_\t\t=> {};\n}"],
    ["matche", "enum match", "match ${e} {\n\t.${red}\t=> $0;\n\t_\t\t=> {};\n}"],
    ["matcherr", "expected match (ok/err)", "match ${expr} {\n\tok v\t=> $0;\n\terr e\t=> e.message();\n}"],
    ["iflet", "optional if-let", "if ${x} := ${opt} {\n\t$0\n} else {\n\t\n}"],
    ["errfn", "throws function", "${name}: (${args}) -> ${ret} throws = {\n\treturn err(\"message\");\n}"],
    ["main", "main function", "main: () -> int = {\n\t$0\n\treturn 0;\n}"],
    ["forin", "for-in loop", "for ${x} in ${xs} {\n\t$0\n}"],
    ["vecd", "vector declaration (M9 std)", "${name}: vector<${int}> := { ${1}, ${2}$0 };"],
    ["arrd", "array declaration (M10 固定长度 T[N])", "${name}: ${int}[${3}] = { ${1}, ${2}, ${3}$0 };"],
    ["mapd", "map declaration (M9 std,有序)", "${name}: map<${string}, ${int}> := {};\n${name}.insert(\"${key}\", ${value});$0"],
    ["opt", "optional(T?) declaration", "${name}: ${T}? = none;"],
];

// std:: 直写 / std 兼容层条目的展示标签
const LAYER_TAG = {
    cpp2: "  · cpp2 风格(rt/cpp2/std)",
    std: "  · std 兼容层",
    user: "  · 方法",
};
const FIELD_TAG = { user: "  · 字段", cpp2: "  · 成员", std: "  · 成员" };

class CompletionProvider {
    constructor() {
        this.cache = new Map();      // uri -> {version, types, vars, enums, variants, funcs}
        this.modules = { names: null, at: 0 };
    }

    index(doc) {
        const hit = this.cache.get(doc.uri.toString());
        if (hit && hit.version === doc.version) return hit;
        const text = doc.getText();
        const types = parseTypes(text);
        const entry = {
            version: doc.version,
            types,
            enums: parseEnums(text),
            variants: parseVariants(text),
            funcs: parseFunctions(text),
        };
        entry.vars = parseVars(text, { types, funcs: entry.funcs });
        this.cache.set(doc.uri.toString(), entry);
        return entry;
    }

    async scanModules() {
        if (this.modules.names && Date.now() - this.modules.at < 60000) return this.modules.names;
        let texts = [];
        try {
            const uris = await vscode.workspace.findFiles("**/*.cpp2", "**/node_modules/**", 500);
            for (const u of uris) {
                try { texts.push(fs.readFileSync(u.fsPath, "utf8")); } catch (_) { /* 跳过 */ }
            }
        } catch (_) { /* 无工作区 */ }
        this.modules = { names: collectModuleNames(texts), at: Date.now() };
        return this.modules.names;
    }

    // `Receiver::` — enum 成员 / std 命名空间
    scopeItems(doc, receiver) {
        const items = [];
        if (receiver === "std") {
            for (const [name, sig, docText] of STD_FUNCTIONS) {
                const c = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
                c.detail = "std::" + name + sig;
                c.documentation = new vscode.MarkdownString(
                    "**std::" + name + sig + "**\n\n" + docText + "\n\n_std:: 直写 = 互操作 escape hatch(DESIGN §9.1)_");
                items.push(c);
            }
            for (const t of STD_STD_TYPES) {
                const c = new vscode.CompletionItem(t, vscode.CompletionItemKind.Class);
                c.detail = "std 类型(互操作 escape hatch)";
                items.push(c);
            }
            for (const t of STD_STD_CONSTS) {
                const c = new vscode.CompletionItem(t, vscode.CompletionItemKind.Constant);
                c.detail = "std 常量";
                items.push(c);
            }
            return items;
        }
        const members = this.index(doc).enums.get(receiver);
        if (members) {
            for (const mem of members) {
                const c = new vscode.CompletionItem(mem,
                    vscode.CompletionItemKind.EnumMember);
                c.detail = receiver + "::" + mem;
                items.push(c);
            }
        }
        return items;
    }

    // 类型位(-> 返回类型 / name: 字段·变量·参数)候选
    typeItems(idx, inParams) {
        const items = [];
        const push = (label, kind, detail) => {
            const c = new vscode.CompletionItem(label, kind);
            if (detail) c.detail = detail;
            items.push(c);
        };
        for (const t of TYPES) push(t, vscode.CompletionItemKind.TypeParameter, TYPE_DETAILS[t]);
        for (const [name, def] of idx.types) {
            push(name, vscode.CompletionItemKind.Class, "type" + (def.base ? " : " + def.base : ""));
        }
        for (const [name] of idx.enums) push(name, vscode.CompletionItemKind.Enum, "enum");
        for (const v of idx.variants) push(v, vscode.CompletionItemKind.Enum, "variant");
        if (inParams) {
            for (const [k, d] of PARAM_MODES) push(k, vscode.CompletionItemKind.Keyword, "参数模式:" + d);
        }
        push("const", vscode.CompletionItemKind.Keyword, "不可变绑定");
        return items;
    }

    // `接收者.` — 按接收者类型出成员;接收者类型来自表达式解析
    memberItems(doc, pos, receiverExpr) {
        const idx = this.index(doc);
        const ctx = { types: idx.types, vars: idx.vars, funcs: idx.funcs };
        const t = resolveExprType(receiverExpr, ctx);
        const items = [];
        for (const mem of membersForType(t, ctx)) {
            const kind = mem.kind === "method" ? vscode.CompletionItemKind.Method
                                               : vscode.CompletionItemKind.Field;
            const c = new vscode.CompletionItem(mem.name, kind);
            if (mem.kind === "method") {
                c.detail = mem.name + (mem.detail || "()") + (LAYER_TAG[mem.layer] || "");
            } else {
                c.detail = (mem.detail ? mem.detail + " " : "") + (FIELD_TAG[mem.layer] || "");
            }
            const md = [];
            if (mem.header) md.push("**" + mem.header + "**");
            if (mem.doc) md.push(mem.doc);
            if (mem.layer === "std") md.push("_std 兼容层:既有语料零迁移;新代码建议 cpp2 风格层_");
            if (md.length) c.documentation = new vscode.MarkdownString(md.join("\n\n"));
            c.sortText = (mem.layer === "user" ? "0" : mem.layer === "cpp2" ? "1" : "2") + mem.name;
            items.push(c);
        }
        // UFCS 桥:标量(或未知接收者).to_string() ≡ std::to_string(x)
        const SCALARS = new Set(["int", "double", "float", "bool", "char",
                                 "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"]);
        if (!t || SCALARS.has(t)) {
            const bridge = new vscode.CompletionItem("to_string", vscode.CompletionItemKind.Function);
            bridge.detail = "UFCS bridge → std::to_string(x) -> string";
            items.push(bridge);
        }
        return items;
    }

    // 默认位:关键词 / 内建类型 / 工作区符号 / 片段 / std 函数 / 内建函数
    defaultItems(idx) {
        const items = [];
        for (const k of KEYWORDS) {
            const c = new vscode.CompletionItem(k, vscode.CompletionItemKind.Keyword);
            if (KEYWORD_DETAILS[k]) c.detail = KEYWORD_DETAILS[k];
            items.push(c);
        }
        for (const t of TYPES) {
            const c = new vscode.CompletionItem(t, vscode.CompletionItemKind.TypeParameter);
            if (TYPE_DETAILS[t]) c.detail = TYPE_DETAILS[t];
            items.push(c);
        }
        for (const [name, def] of idx.types) {
            const c = new vscode.CompletionItem(name, vscode.CompletionItemKind.Class);
            c.detail = "type" + (def.base ? " : " + def.base : "");
            items.push(c);
        }
        for (const [name] of idx.enums) {
            const c = new vscode.CompletionItem(name, vscode.CompletionItemKind.Enum);
            c.detail = "enum";
            items.push(c);
        }
        for (const v of idx.variants) {
            const c = new vscode.CompletionItem(v, vscode.CompletionItemKind.Enum);
            c.detail = "variant";
            items.push(c);
        }
        for (const [name, f] of idx.funcs) {
            const c = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
            c.detail = "(" + f.params + ")" + (f.ret ? " -> " + f.ret : "")
                     + (f.throws ? " throws" : "");
            items.push(c);
        }
        for (const [name, ty] of idx.vars) {
            const c = new vscode.CompletionItem(name, vscode.CompletionItemKind.Variable);
            c.detail = ": " + ty;
            items.push(c);
        }
        for (const [name, use, docText] of BUILTINS) {
            const c = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
            c.detail = "内建:" + use;
            c.documentation = new vscode.MarkdownString(docText);
            items.push(c);
        }
        for (const [label, desc, body] of SNIPPETS) {
            const s = new vscode.CompletionItem(label, vscode.CompletionItemKind.Snippet);
            s.detail = "snippet: " + desc;
            s.insertText = new vscode.SnippetString(body);
            s.documentation = new vscode.MarkdownString("```cpp2\n"
                + body.replace(/\$\{?\d+:?([^}]*)\}?/g, "$1") + "\n```");
            items.push(s);
        }
        for (const [name, sig, docText] of STD_FUNCTIONS) {
            const c = new vscode.CompletionItem("std::" + name, vscode.CompletionItemKind.Function);
            c.insertText = "std::" + name;
            c.detail = "std::" + name + sig;
            c.documentation = new vscode.MarkdownString(docText);
            items.push(c);
        }
        return items;
    }

    async provideCompletionItems(doc, pos) {
        const linePrefix = doc.getText(new vscode.Range(new vscode.Position(pos.line, 0), pos));
        if (inStringOrComment(linePrefix)) return [];

        const idx = this.index(doc);
        let m;

        // import → 模块候选(std + 工作区 module 名)
        const im = /^[\t ]*import\s+([\w.]*)$/.exec(linePrefix);
        if (im) {
            const names = await this.scanModules();
            return names
                .filter(n => !im[1] || n.startsWith(im[1]))
                .map(n => {
                    const c = new vscode.CompletionItem(n, vscode.CompletionItemKind.Module);
                    c.detail = n === "std" ? "标准库面(M9:print/println/to_string/…)" : "工作区模块";
                    return c;
                });
        }
        // @ → 属性
        if (/^[\t ]*@/.test(linePrefix) && (m = /@\s*(\w*)$/.exec(linePrefix))) {
            return ATTRS.map(([name, docText]) => {
                const c = new vscode.CompletionItem(name, vscode.CompletionItemKind.Keyword);
                c.detail = "@" + name;
                c.documentation = new vscode.MarkdownString(docText);
                return c;
            });
        }
        // Receiver:: → enum 成员 / std::
        if ((m = /([A-Za-z_]\w*)\s*::\s*(\w*)$/.exec(linePrefix))) {
            return this.scopeItems(doc, m[1]);
        }
        // match 臂 `.x` → 枚举成员
        if (/^[\t ]*\.\s*\w*$/.test(linePrefix) && idx.enums.size > 0) {
            const items = [];
            for (const [ename, members] of idx.enums) {
                for (const mem of members) {
                    const c = new vscode.CompletionItem(mem, vscode.CompletionItemKind.EnumMember);
                    c.detail = ename + "::" + mem;
                    c.insertText = mem;
                    items.push(c);
                }
            }
            return items;
        }
        // 接收者. → 成员(光标前 500 字符回扫表达式,支持链式调用)
        const tailStart = Math.max(0, doc.offsetAt(pos) - 500);
        const tail = doc.getText(new vscode.Range(doc.positionAt(tailStart), pos));
        const eb = exprBeforeDot(tail);
        if (eb) return this.memberItems(doc, pos, eb.expr);

        // -> 返回类型位
        if (/->\s*[\w<>?, \t]*$/.test(linePrefix)) return this.typeItems(idx, false);
        // name: 类型位(变量/字段/参数;参数位附参数模式)
        if (/([A-Za-z_]\w*)\s*:\s*[\w<>?, \t]*$/.test(linePrefix)) {
            return this.typeItems(idx, parenDepth(tail) > 0);
        }
        return this.defaultItems(idx);
    }
}

// ── Outline: golden rule `name: kind = ...` ──
class SymbolProvider {
    provideDocumentSymbols(doc) {
        const symbols = [];
        const re = /^(\s*)([A-Za-z_]\w*)\s*:\s*(type|enum|variant|concept)\b|^\s*([A-Za-z_]\w*)\s*:\s*(?=<[^;{]*>?\s*\(|\()|^([A-Za-z_]\w*)\s*:(?![=])(?!type\b|enum\b|variant\b|concept\b)/gm;
        let m;
        while ((m = re.exec(doc.getText())) !== null) {
            const indent = m[1] ? m[1].length : 0;
            const name = m[2] || m[4] || m[5];
            const start = doc.positionAt(m.index);
            const end = doc.positionAt(m.index + name.length);
            let kind;
            if (m[3]) {
                kind = { type: vscode.SymbolKind.Struct, enum: vscode.SymbolKind.Enum,
                         variant: vscode.SymbolKind.Struct, concept: vscode.SymbolKind.Interface }[m[3]];
            } else if (m[4]) {
                kind = indent > 0 ? vscode.SymbolKind.Method : vscode.SymbolKind.Function;
            } else {
                kind = vscode.SymbolKind.Variable;
            }
            symbols.push(new vscode.DocumentSymbol(name, "", kind, start, end));
        }
        return symbols;
    }
}

// ── CodeLens: Run on main ──
class LensProvider {
    provideCodeLenses(doc) {
        const lenses = [];
        const re = /^main\s*:/gm;
        let m;
        while ((m = re.exec(doc.getText())) !== null) {
            const pos = doc.positionAt(m.index);
            lenses.push(new vscode.CodeLens(new vscode.Range(pos, pos), {
                title: "\u25B6 Run",
                command: "cpp2.run",
                arguments: [doc.uri],
            }));
        }
        return lenses;
    }
}

// ── Commands ──
function workspaceOf(uri) {
    const f = vscode.workspace.getWorkspaceFolder(uri);
    return f ? f.uri.fsPath : path.dirname(uri.fsPath);
}

// 终端环境:注入 CPP2_RT(自动探测工作区 rt/)与 CPP2_LDFLAGS(设置项)
function termEnv(uri) {
    const env = Object.assign({}, process.env);
    try {
        const wsRoot = workspaceOf(uri);
        if (fs.existsSync(path.join(wsRoot, "rt", "cpp2", "support.hpp")))
            env.CPP2_RT = path.join(wsRoot, "rt");
        const cfg = vscode.workspace.getConfiguration("cpp2");
        const ld = cfg.get("ldflags", "");
        if (ld) env.CPP2_LDFLAGS = ld;
    } catch (_) { /* 尽力注入 */ }
    return env;
}

async function runInTerminal(title, args, cwd, uri) {
    const tp = toolPath();
    if (!fs.existsSync(tp)) {
        vscode.window.showErrorMessage(
            `cpp2 tool not found: ${tp} — 请检查设置 cpp2.toolPath`);
        return;
    }
    const t = vscode.window.createTerminal({
        name: title, cwd,
        env: termEnv(uri || (vscode.window.activeTextEditor
            && vscode.window.activeTextEditor.document.uri)),
    });
    t.show();
    // 终端 shell 就绪前 sendText 会丢字:延迟发送
    setTimeout(() => {
        t.sendText([tp, ...args].map(a => a.includes(" ") ? `"${a}"` : a).join(" "));
    }, 600);
}

async function cmdRun(uri) {
    uri = uri || (vscode.window.activeTextEditor && vscode.window.activeTextEditor.document.uri);
    if (!uri) return;
    // 默认 native 后端(零 g++;Unsupported 自动回退 headers)
    await runInTerminal("C++2 Run", ["run", uri.fsPath, "--backend=native"], path.dirname(uri.fsPath), uri);
}
async function cmdBuild(uri) {
    uri = uri || (vscode.window.activeTextEditor && vscode.window.activeTextEditor.document.uri);
    if (!uri) return;
    // 默认 native;构建产物与 headers 隔离(.cpp2build/native)
    await runInTerminal("C++2 Build", ["build", uri.fsPath, "--backend=native"], path.dirname(uri.fsPath), uri);
}
async function cmdCheck() {
    const ed = vscode.window.activeTextEditor;
    if (ed) await checkDocument(ed.document);
}

function activate(context) {
    context.subscriptions.push(diagCollection);

    context.subscriptions.push(vscode.commands.registerCommand("cpp2.run", cmdRun));
    context.subscriptions.push(vscode.commands.registerCommand("cpp2.build", cmdBuild));
    context.subscriptions.push(vscode.commands.registerCommand("cpp2.check", cmdCheck));

    const completions = new CompletionProvider();
    context.subscriptions.push(vscode.languages.registerDocumentSymbolProvider(
        { language: "cpp2" }, new SymbolProvider()));
    context.subscriptions.push(vscode.languages.registerCompletionItemProvider(
        { language: "cpp2" }, completions, ":", ".", "@"));
    context.subscriptions.push(vscode.languages.registerCodeLensProvider(
        { language: "cpp2" }, new LensProvider()));

    context.subscriptions.push(vscode.workspace.onDidSaveTextDocument(doc => {
        if (vscode.workspace.getConfiguration("cpp2").get("checkOnSave", true)) {
            checkDocument(doc);
        }
    }));
    context.subscriptions.push(vscode.workspace.onDidChangeTextDocument(e => scheduleCheck(e.document)));
    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(doc => checkDocument(doc)));
    context.subscriptions.push(vscode.workspace.onDidCloseTextDocument(doc => diagCollection.delete(doc.uri)));

    if (vscode.window.activeTextEditor) {
        checkDocument(vscode.window.activeTextEditor.document);
    }
}

function deactivate() {
    clearTimeout(checkTimer);
}

module.exports = {
    activate, deactivate,
    __internals: {
        CompletionProvider,
        parseTypes, parseVars, parseEnums, parseVariants, parseFunctions,
        resolveMembers, pointeeOf, splitGenericArgs, genericParamsOf, subst,
        resolveExprType, methodReturnType, fieldTypeOf, membersForType,
        exprBeforeDot, inStringOrComment, parenDepth, collectModuleNames,
        elementTypeOf, sigDetail, STD_MEMBERS,
    },
};
