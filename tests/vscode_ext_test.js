// Unit test for extension.js pure helpers (no VSCode needed - stubbed).
const Module = require("module");
const origLoad = Module._load;
Module._load = function (request, parent, isMain) {
    if (request === "vscode") {
        return {
            languages: { createDiagnosticCollection: () => ({}) },
            window: { activeTextEditor: null },
            workspace: {
                getConfiguration: () => ({ get: () => null }),
                onDidSaveTextDocument: () => ({ dispose() {} }),
                onDidChangeTextDocument: () => ({ dispose() {} }),
                onDidOpenTextDocument: () => ({ dispose() {} }),
                onDidCloseTextDocument: () => ({ dispose() {} }),
                textDocuments: [],
                findFiles: () => Promise.resolve([]),
            },
            commands: { registerCommand: () => ({ dispose() {} }) },
            CompletionItem: function (label, kind) { this.label = label; this.kind = kind; },
            CompletionItemKind: {
                Keyword: 14, TypeParameter: 25, Snippet: 15, Function: 3, Field: 4, Method: 2,
                Class: 7, Enum: 12, EnumMember: 19, Constant: 20, Variable: 6, Module: 9,
            },
            SnippetString: function (v) { this.value = v; },
            MarkdownString: function (v) { this.value = v; },
            Position: function (line, character) { this.line = line; this.character = character; },
            Range: function (start, end) { this.start = start; this.end = end; },
            DocumentSymbol: function () {}, CodeLens: function () {},
            Diagnostic: function () {}, DiagnosticSeverity: {},
            Uri: { file: (f) => ({ fsPath: f }) },
        };
    }
    return origLoad.apply(this, arguments);
};

const { __internals } = require("../editors/vscode/extension.js");
const {
    CompletionProvider,
    parseTypes, parseVars, parseEnums, parseVariants, parseFunctions, resolveMembers, pointeeOf,
    resolveExprType, methodReturnType, membersForType, exprBeforeDot,
    collectModuleNames, elementTypeOf, inStringOrComment,
} = __internals;
const fs = require("fs");

let failed = 0;
function check(name, cond) {
    if (cond) console.log("PASS " + name);
    else { console.log("FAIL " + name); failed++; }
}

// ── provider 分发端到端:模拟 TextDocument,走真实 provideCompletionItems ──
// VSCode lineAt().text 不含 \r:统一归一化 CRLF → LF,与行尾无关
function makeDoc(text0) {
    const text = text0.replace(/\r\n/g, "\n");
    const lines = text.split("\n");
    const lineStarts = [];
    let acc = 0;
    for (const l of lines) { lineStarts.push(acc); acc += l.length + 1; }
    const offsetAt = pos => lineStarts[pos.line] + pos.character;
    return {
        languageId: "cpp2",
        uri: { toString: () => "file:///t.cpp2", fsPath: "t.cpp2" },
        version: 1,
        getText(range) {
            if (!range) return text;
            return text.slice(offsetAt(range.start), offsetAt(range.end));
        },
        positionAt(off) {
            let line = 0;
            while (line + 1 < lineStarts.length && lineStarts[line + 1] <= off) line++;
            return { line, character: off - lineStarts[line] };
        },
        offsetAt,
    };
}

async function complete(text, lineText) {
    const doc = makeDoc(text);
    const line = text.replace(/\r\n/g, "\n").split("\n").indexOf(lineText);
    if (line < 0) throw new Error("line not found: " + lineText);
    const provider = new CompletionProvider();
    const col = lineText.length;
    return provider.provideCompletionItems(doc, { line, character: col });
}
const labels = items => items.map(i => i.label);

// ── real example file ──
const src = fs.readFileSync("examples/types.cpp2", "utf8");
const types = parseTypes(src);

check("Animal parsed", types.has("Animal"));
check("Dog base = Animal", types.get("Dog").base === "Animal");
check("Dog field tricks", types.get("Dog").fields.some(f => f.name === "tricks" && f.type === "int"));
check("Animal field name: string", types.get("Animal").fields.some(f => f.name === "name" && f.type === "string"));
check("Dog method rename", types.get("Dog").methods.includes("rename"));
check("Animal method speak", types.get("Animal").methods.includes("speak"));
check("destructor excluded", !types.get("Dog").methods.includes("destructor"));
check("method sig ret: speak -> string", types.get("Animal").methodSigs.speak.ret === "string");
check("method sig mutates: rename", types.get("Dog").methodSigs.rename.mutates === true);
check("method sig params: rename", types.get("Dog").methodSigs.rename.params === "n: string");

const vars = parseVars(src);
check("var a: Animal", vars.get("a") === "Animal");
check("var d: Dog (inferred factory)", vars.get("d") === "Dog");

const dogMembers = resolveMembers(types, "Dog").map(x => x.name);
check("inherited name", dogMembers.includes("name"));
check("inherited speak", dogMembers.includes("speak"));
check("own tricks", dogMembers.includes("tricks"));
check("own rename", dogMembers.includes("rename"));

// ── smart pointer unwrap ──
check("pointeeOf unique<Dog>", pointeeOf("unique<Dog>") === "Dog");
const ptrMembers = resolveMembers(types, "unique<Dog>").map(x => x.name);
check("unique<Dog> members via deref", ptrMembers.includes("tricks"));

// ── local decls inside function bodies ──
const snippet = `
main: () -> int = {
    p: Point := Point{.x = 3};
    q: const Point := Point{};
    w: unique<Widget> := make_unique(Widget{});
    return 0;
}`;
const sv = parseVars(snippet);
check("local p: Point", sv.get("p") === "Point");
check("const-qualified q: Point", sv.get("q") === "Point");
check("smart ptr w: unique<Widget>", sv.get("w") === "unique<Widget>");

// ── enums + :: scope completion ──
const colors = fs.readFileSync("examples/colors.cpp2", "utf8");
const shapes = fs.readFileSync("examples/shapes.cpp2", "utf8");
const ce = parseEnums(colors);
const se = parseEnums(shapes);
check("Color enum parsed", Array.isArray(ce.get("Color")));
check("Color members", JSON.stringify(ce.get("Color")) === JSON.stringify(["red", "green", "blue"]));
check("Signal enum (shapes)", JSON.stringify(se.get("Signal")) === JSON.stringify(["red", "green", "yellow"]));
check("no enum named Shape", !se.has("Shape"));

// underlying-type syntax: Flags: enum: u8 = { a, b }
const fe = parseEnums("Flags: enum: u8 = { a, b }");
check("enum with underlying type", JSON.stringify(fe.get("Flags")) === JSON.stringify(["a", "b"]));

// ── export prefix visible to index ──
const expTypes = parseTypes("export Config: type = {\n    name: string = \"\";\n}\n");
check("export Config: type parsed", expTypes.has("Config"));
check("export enum parsed", parseEnums("export Color: enum = { red }").has("Color"));
check("variant parsed", parseVariants("Shape: variant = { Point, Circle }").includes("Shape"));

// ── parseFunctions: 顶层函数签名/返回类型 ──
const showcase = fs.readFileSync("examples/showcase.cpp2", "utf8");
const funcs = parseFunctions(showcase);
check("parse_digit ret int", funcs.get("parse_digit") && funcs.get("parse_digit").ret === "int");
check("parse_digit throws", funcs.get("parse_digit").throws === true);
check("withdraw_style ret i64", funcs.get("withdraw_style").ret === "i64");
check("generic clamp ret T", funcs.get("clamp").ret === "T");
check("add_into no ret (proc)", funcs.get("add_into").ret === "");
check("methods not polluted into funcs", !funcs.has("speak") && !funcs.has("rename"));

// ── resolveExprType: 字面量 / 调用 / 方法链 / 解引用 / 聚合 ──
const sctx = {
    types: parseTypes(showcase),
    vars: new Map([["s", "string"], ["v", "vector<int>"], ["ages", "map<string, int>"],
                   ["d", "Point"], ["p", "unique<Point>"], ["maybe", "int?"]]),
    funcs,
};
check("literal string", resolveExprType("\"abc\"", sctx) === "string");
check("literal char", resolveExprType("'x'", sctx) === "char");
check("literal int", resolveExprType("42", sctx) === "int");
check("literal double", resolveExprType("3.14", sctx) === "double");
check("var lookup", resolveExprType("s", sctx) === "string");
check("find → u64? (T? 取代 npos)", resolveExprType("s.find(\"cpp2\")", sctx) === "u64?");
check("vector.first → int?", resolveExprType("v.first()", sctx) === "int?");
check("map.get → int?", resolveExprType("ages.get(\"grace\")", sctx) === "int?");
check("method ret via methodSigs (norm2 → int)", resolveExprType("d.norm2()", sctx) === "int");
check("deref *p → Point", resolveExprType("*p", sctx) === "Point");
check("deref *opt → int", resolveExprType("*s.find(\"x\")", sctx) === "u64");
check("aggregate Point{...}", resolveExprType("Point{.x = 3, .y = 4}", sctx) === "Point");
check("make_unique → unique<Point>", resolveExprType("make_unique(Point{.x = 1})", sctx) === "unique<Point>");
check("user fn call ret (parse_digit → int)", resolveExprType("parse_digit('7')", sctx) === "int");
check("ufcs to_string(42) → string", resolveExprType("to_string(42)", sctx) === "string");
check("field chain d.x → int", resolveExprType("d.x", sctx) === "int");

// ── membersForType: M9 std 双轨 API ──
const stringMembers = membersForType("string", sctx);
const stringNames = stringMembers.map(x => x.name);
check("string: cpp2 风格层 len/at/find", ["len", "at", "find"].every(n => stringNames.includes(n)));
check("string: std 兼容层 size/push_back", ["size", "push_back", "c_str"].every(n => stringNames.includes(n)));
check("string: cpp2 layer tagged", stringMembers.find(x => x.name === "len").layer === "cpp2");
check("string: std layer tagged", stringMembers.find(x => x.name === "size").layer === "std");

const vecMembers = membersForType("vector<int>", sctx);
const vecGet = n => vecMembers.find(x => x.name === n);
check("vector: push/pop/at/first/last", ["push", "pop", "at", "first", "last"].every(n => !!vecGet(n)));
check("vector: T 替换 push(v: int)", vecGet("push").detail === "(v: int) -> void");
check("vector: pop → int?", vecGet("pop").detail === "() -> int?");
check("list<int> = vector 别名", membersForType("list<int>", sctx).map(x => x.name).includes("push"));

const mapMembers = membersForType("map<string, int>", sctx);
const mapGet = n => mapMembers.find(x => x.name === n);
check("map: insert/get/at/contains/remove", ["insert", "get", "at", "contains", "remove"].every(n => !!mapGet(n)));
check("map: get(k: string) -> int?", mapGet("get").detail === "(k: string) -> int?");
check("map: 无 operator[](缺失键走 get/at)", !mapGet("operator[]"));

const pairMembers = membersForType("pair<string, int>", sctx);
check("pair: first/second(类型替换)",
      pairMembers.find(x => x.name === "first").detail === "string"
      && pairMembers.find(x => x.name === "second").detail === "int");

check("optional int?: has_value/value_or",
      membersForType("int?", sctx).map(x => x.name).includes("has_value"));
check("unique<Point> 自动解引用出用户成员",
      membersForType("unique<Point>", sctx).map(x => x.name).includes("norm2"));
check("error 值: message()",
      membersForType("cpp2error", sctx).map(x => x.name).includes("message"));
check("user type members with sig detail",
      membersForType("Dog", { types: parseTypes(src) }).find(x => x.name === "rename").detail === "(n: string) mutates");

// ── exprBeforeDot: 光标前接收者表达式回扫 ──
check("bare ident", JSON.stringify(exprBeforeDot("s.le")) === JSON.stringify({ expr: "s", member: "le" }));
check("chain call", exprBeforeDot("s.find(\"cpp2\").").expr === "s.find(\"cpp2\")");
check("field chain", exprBeforeDot("d.norm2().").expr === "d.norm2()");
check("index receiver", exprBeforeDot("v[2].").expr === "v[2]");
check("deref receiver", exprBeforeDot("*p.").expr === "*p");
check("string literal receiver", exprBeforeDot("\"abc\".").expr === "\"abc\"");
check("no dot → null", exprBeforeDot("hello") === null);

// ── elementTypeOf(for-in)+ parseVars 推断 ──
check("for-in string → char", elementTypeOf("string") === "char");
check("for-in vector<int> → int", elementTypeOf("vector<int>") === "int");
check("for-in map → pair", elementTypeOf("map<string, int>") === "pair<string, int>");
const stdown = fs.readFileSync("tests/cases/stdown.cpp2", "utf8");
const stdTypes = parseTypes(stdown);
const stdFuncs = parseFunctions(stdown);
const sv2 = parseVars(stdown, { types: stdTypes, funcs: stdFuncs });
check("stdown: v: vector<int>", sv2.get("v") === "vector<int>");
check("stdown: ages: map<string, int>", sv2.get("ages") === "map<string, int>");
check("stdown: p := s.find(...) → u64?", sv2.get("p") === "u64?");
check("stdown: top := v.pop() → int?", sv2.get("top") === "int?");
check("stdown: g := ages.get(...) → int?", sv2.get("g") === "int?");
check("stdown: for c in line → char", sv2.get("c") === "char");
check("stdown: for x in v → int", sv2.get("x") === "int");
check("stdown: for kv in ages → pair", sv2.get("kv") === "pair<string, int>");

// ── M10 固定长度数组 T[N] ──
check("for-in int[3] → int", elementTypeOf("int[3]") === "int");
check("for-in string[2] → string", elementTypeOf("string[2]") === "string");
const arrMembers = membersForType("int[3]", sctx);
const arrGet = n => arrMembers.find(x => x.name === n);
check("array: cpp2 层 len/at/first/last", ["len", "at", "first", "last"].every(n => !!arrGet(n)));
check("array: std 层 size/empty/fill", ["size", "empty", "fill", "begin"].every(n => !!arrGet(n)));
check("array: at 替换 T → (i: u64) -> int", arrGet("at").detail === "(i: u64) -> int");
check("array: string[2] 的 at → string", membersForType("string[2]", sctx).find(x => x.name === "at").detail === "(i: u64) -> string");
const arrsrc = "main: () -> int = {\n    a: int[3] = {1, 2, 3};\n    d: double[2] = {1.5, 2.5};\n}";
const av = parseVars(arrsrc);
check("array: a: int[3] 全类型记录", av.get("a") === "int[3]");
check("array: d: double[2]", av.get("d") === "double[2]");
check("array: a[1] → int(下标接收者推断)", resolveExprType("a[1]", { types: new Map(), vars: av, funcs: new Map() }) === "int");

// ── import 模块候选 / 字符串守卫 ──
const mods = collectModuleNames(["module app.config;\n", "module stdown;\n", "import std;\n"]);
check("collectModuleNames std+工作区", mods.includes("std") && mods.includes("app.config") && mods.includes("stdown"));
check("inStringOrComment: 行注释", inStringOrComment("  // hello") === true);
check("inStringOrComment: 未闭合字符串", inStringOrComment("s := \"abc") === true);
check("inStringOrComment: 正常代码", inStringOrComment("v.push(1") === false);

// ── provider 上下文分发(stdown.cpp2 为真实语料)──
(async () => {
    const stdownText = fs.readFileSync("tests/cases/stdown.cpp2", "utf8");

    // `.` → 接收者成员:string 双轨
    const doc1 = stdownText.replace('    std::println("S1', '    s.\n    std::println("S1');
    items = await complete(doc1, "    s.");
    check("dispatch: s. → string 双轨成员", ["len", "at", "find", "size", "push_back"].every(n => labels(items).includes(n)));
    const lenItem = items.find(i => i.label === "len");
    check("dispatch: len 排序先于 std 兼容层", lenItem.sortText < items.find(i => i.label === "size").sortText);
    check("dispatch: len detail 带 cpp2 标签", /cpp2 风格/.test(lenItem.detail));

    // `.` → map 成员(泛型实参替换进签名)
    const doc2 = stdownText.replace('    ages.insert("ada", 36);', "    ages.\n    ages.insert(\"ada\", 36);");
    items = await complete(doc2, "    ages.");
    check("dispatch: ages. → map 成员 get/insert/contains/remove",
          ["get", "insert", "contains", "remove", "at"].every(n => labels(items).includes(n)));
    check("dispatch: map.get detail 替换 K/V",
          items.find(i => i.label === "get").detail.includes("(k: string) -> int?"));

    // `.` → 链式返回值推断:v.first() → int? → has_value
    const doc3 = stdownText.replace("    std::println(\"V1:", "    v.first().\n    std::println(\"V1:");
    items = await complete(doc3, "    v.first().");
    check("dispatch: v.first(). → optional 成员", labels(items).includes("has_value"));

    // `.` → 用户类型成员(含方法签名)
    const showcaseText = fs.readFileSync("examples/showcase.cpp2", "utf8");
    const doc4 = showcaseText.replace("    d.translate(1, 1);", "    d.\n    d.translate(1, 1);");
    items = await complete(doc4, "    d.");
    check("dispatch: d. → Point 用户成员", labels(items).includes("norm2") && labels(items).includes("translate"));
    check("dispatch: norm2 detail 带签名", items.find(i => i.label === "norm2").detail.includes("() -> int"));

    // `.` → unique 自动解引用
    const doc5 = showcaseText.replace('    std::println("K:', '    p.\n    std::println("K:');
    items = await complete(doc5, "    p.");
    check("dispatch: p(unique<Point>). → 解引用成员", labels(items).includes("norm2"));

    // `.` → M10 数组成员(examples/arrays.cpp2 为真实语料)
    const arrText = fs.readFileSync("examples/arrays.cpp2", "utf8");
    const docA = arrText.replace('    std::print("a = ', '    a.\n    std::print("a = ');
    items = await complete(docA, "    a.");
    check("dispatch: a. → array 成员 len/at/first", ["len", "at", "first", "last"].every(n => labels(items).includes(n)));
    check("dispatch: array.at detail 替换 T", items.find(i => i.label === "at").detail.includes("(i: u64) -> int"));

    // `::` → std 函数带签名
    const doc6 = stdownText.replace('    std::println("S1:', '    std::\n    std::println("S1:');
    items = await complete(doc6, "    std::");
    check("dispatch: std:: → println/to_string/make_unique",
          ["println", "print", "to_string", "make_unique"].every(n => labels(items).includes(n)));
    check("dispatch: println detail 带签名", items.find(i => i.label === "println").detail.includes("(fmt: string_view"));

    // import → 模块候选(工作区扫描为空 → 至少 std)
    const doc7 = stdownText.replace("import std;", "import std;\nimport ");
    items = await complete(doc7, "import ");
    check("dispatch: import → std 模块候选", labels(items).includes("std"));

    // @ → 属性
    items = await complete("@unsafe {\n}\n@", "@");
    check("dispatch: @ → unsafe/unchecked", ["unsafe", "unchecked"].every(n => labels(items).includes(n)));

    // -> → 返回类型位(内建类型 + 工作区枚举;无参数模式)
    items = await complete("Color: enum = { red, green }\nf: () -> \n{\n}", "f: () -> ");
    check("dispatch: -> → 类型位含 map/vector + 工作区 enum",
          ["map", "vector", "string", "int", "Color"].every(n => labels(items).includes(n)));
    check("dispatch: -> 类型位无参数模式", !labels(items).includes("inout"));

    // 参数位 → 参数模式 + 类型
    items = await complete("Color: enum = { red, green }\nf: (x: \n{\n}", "f: (x: ");
    check("dispatch: 参数位 → inout/out/move 模式 + 工作区 enum",
          ["inout", "out", "move", "copy", "Color"].every(n => labels(items).includes(n)));

    // 默认位 → 关键词/片段/std 函数/本地符号
    items = await complete(stdownText, "        joined2 += std::to_string(kv.second);");
    check("dispatch: 默认 → 关键词+类型+片段+std 函数",
          ["match", "throws", "map", "vecd", "std::println"].every(n => labels(items).includes(n)));
    check("dispatch: 默认 → 本地函数/变量候选", labels(items).includes("word_count") && labels(items).includes("ages"));
    check("dispatch: 本地函数候选带签名", items.find(i => i.label === "word_count").detail.includes("-> int"));

    // match 臂 `.x` → 枚举成员
    const doc8 = showcaseText.replace(".red   => std::println(\"F: stop\");", ".red   => std::println(\"F: stop\");\n        .");
    items = await complete(doc8, "        .");
    check("dispatch: match 臂 . → 枚举成员", labels(items).includes("red") && labels(items).includes("green"));

    // 字符串/注释内 → 不出候选
    items = await complete('main: () -> int = {\n    s := "abc.\n}', '    s := "abc.');
    check("dispatch: 字符串内不出候选", items.length === 0);

    console.log(failed === 0 ? "\nALL PASS" : `\n${failed} FAILURES`);
    process.exit(failed === 0 ? 0 : 1);
})().catch(e => { console.error(e); process.exit(1); });
