// C++2 language extension: diagnostics / outline / completions / member completion.
// No dependencies - child-process based, regex parsing of cpp2 tool output.
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

// ══ Index: types (fields/methods/base) + variables ─═════════════════
// Pure functions (unit-testable via node; see tests in editors/vscode/test.js)

// Name: type = { ... } -> { base, fields:[{name,type}], methods:[name] }
function parseTypes(text) {
    const types = new Map();
    const re = /^([ \t]*)([A-Za-z_]\w*)[ \t]*:[ \t]*type[ \t]*(?::[ \t]*([A-Za-z_][\w.:]*)[ \t]*)?=[ \t]*\{/gm;
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
        const t = { base: m[3] ? m[3].split(":").pop() : null, fields: [], methods: [] };
        const fre = /^[ \t]+([A-Za-z_]\w*)[ \t]*:[ \t]*([^=({\n]+?)[ \t]*=[ \t]/gm;
        let fm;
        while ((fm = fre.exec(body)) !== null) {
            t.fields.push({ name: fm[1], type: fm[2].trim() });
        }
        const mre = /^[ \t]+([A-Za-z_]\w*)[ \t]*:[ \t]*\(/gm;
        while ((fm = mre.exec(body)) !== null) {
            if (fm[1] !== "destructor") t.methods.push(fm[1]);
        }
        types.set(m[2], t);
    }
    return types;
}

// Name: enum [: u8] = { a, b, c } -> Map enumName -> [members]
function parseEnums(text) {
    const enums = new Map();
    const re = /^([ \t]*)([A-Za-z_]\w*)[ \t]*:[ \t]*enum(?:[ \t]*:[ \t]*[^={\n]+)?[ \t]*=[ \t]*\{([^}]*)\}/gm;
    let m;
    while ((m = re.exec(text)) !== null) {
        const members = m[3].split(",").map(s => s.trim()).filter(Boolean);
        enums.set(m[2], members);
    }
    return enums;
}

// variable/parameter declarations -> Map varName -> type text (last wins)
function parseVars(text) {
    const vars = new Map();
    let m;
    // x: Type := / x: Type = / x: const Type =
    const re1 = /\b([A-Za-z_]\w*)[ \t]*:[ \t]*((?:const[ \t]+)?[A-Za-z_][\w.]*(?:<[^=;{}\n]*>)?[?]?(?:[ \t]+const)?)[ \t]*:?=/g;
    while ((m = re1.exec(text)) !== null) {
        const ty = m[2].replace(/\bconst\b/g, "").trim();
        if (ty) vars.set(m[1], ty);
    }
    // x := Point{...} (inferred aggregate)
    const re2 = /\b([A-Za-z_]\w*)[ \t]*:=[ \t]*([A-Za-z_]\w*)[ \t]*\{/g;
    while ((m = re2.exec(text)) !== null) vars.set(m[1], m[2]);
    return vars;
}

// unwrap unique<T>/shared<T>/weak<T>
function pointeeOf(typeText) {
    const m = /^(?:unique|shared|weak)\s*<\s*([^>]+)\s*>$/.exec(typeText.trim());
    return m ? m[1].trim() : null;
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

// ══ Completions ═════════════════════════════════════════════════════
const KEYWORDS = [
    "module", "import", "export", "type", "enum", "variant", "concept",
    "if", "else", "for", "while", "in", "return", "break", "continue",
    "match", "throws", "mutates", "const", "none", "some", "self",
    "inout", "out", "move", "copy", "forward", "destructor",
];
const TYPES = [
    "int", "double", "float", "bool", "char", "void",
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
    "string", "string_view", "list", "vector", "byte",
    "unique", "shared", "weak", "auto",
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
    ["iflet", "optional if-let", "if ${x} := ${opt} {\n\t$0\n} else {\n\t\n}"],
    ["errfn", "throws function", "${name}: (${args}) -> ${ret} throws = {\n\treturn err(\"message\");\n}"],
    ["main", "main function", "main: () -> int = {\n\t$0\n\treturn 0;\n}"],
    ["forin", "for-in loop", "for ${x} in ${xs} {\n\t$0\n}"],
];

class CompletionProvider {
    constructor() {
        this.cache = new Map();      // uri -> {version, types, vars}
    }

    index(doc) {
        const hit = this.cache.get(doc.uri.toString());
        if (hit && hit.version === doc.version) return hit;
        const entry = {
            version: doc.version,
            types: parseTypes(doc.getText()),
            vars: parseVars(doc.getText()),
            enums: parseEnums(doc.getText()),
        };
        this.cache.set(doc.uri.toString(), entry);
        return entry;
    }

    // `Receiver::` — enum members / std namespace
    scopeItems(doc, receiver) {
        const items = [];
        if (receiver === "std") {
            for (const f of ["println", "print", "to_string", "sqrt", "pow", "abs",
                             "make_unique", "make_shared"]) {
                const c = new vscode.CompletionItem(f, vscode.CompletionItemKind.Function);
                c.detail = "std";
                items.push(c);
            }
            for (const t of ["string", "string_view", "vector", "optional", "expected",
                             "variant", "unique_ptr", "shared_ptr"]) {
                const c = new vscode.CompletionItem(t, vscode.CompletionItemKind.TypeParameter);
                c.detail = "std type";
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

    memberItems(doc, pos, receiver) {
        const idx = this.index(doc);
        const varType = idx.vars.get(receiver);
        const items = [];
        if (varType) {
            for (const mem of resolveMembers(idx.types, varType)) {
                const c = new vscode.CompletionItem(mem.name,
                    mem.kind === "method" ? vscode.CompletionItemKind.Method
                                          : vscode.CompletionItemKind.Field);
                c.detail = mem.kind === "method" ? mem.detail
                                                 : mem.kind + ": " + mem.detail;
                items.push(c);
            }
        }
        // std bridges: scalar.to_string() (UFCS)
        const bridge = new vscode.CompletionItem("to_string", vscode.CompletionItemKind.Function);
        bridge.detail = "UFCS bridge";
        items.push(bridge);
        return items;
    }

    provideCompletionItems(doc, pos) {
        const prefix = doc.getText(new vscode.Range(new vscode.Position(pos.line, 0), pos));
        const dcolM = /([A-Za-z_]\w*)\s*::\s*(\w*)$/.exec(prefix);
        if (dcolM) return this.scopeItems(doc, dcolM[1]);
        const dotM = /([A-Za-z_]\w*)\s*\.\s*(\w*)$/.exec(prefix);
        if (dotM) return this.memberItems(doc, pos, dotM[1]);

        const items = [];
        for (const k of KEYWORDS) {
            items.push(new vscode.CompletionItem(k, vscode.CompletionItemKind.Keyword));
        }
        for (const t of TYPES) {
            items.push(new vscode.CompletionItem(t, vscode.CompletionItemKind.TypeParameter));
        }
        for (const [label, desc, body] of SNIPPETS) {
            const s = new vscode.CompletionItem(label, vscode.CompletionItemKind.Snippet);
            s.detail = "snippet: " + desc;
            s.insertText = new vscode.SnippetString(body);
            s.documentation = new vscode.MarkdownString("```cpp2\n"
                + body.replace(/\$\{?\d+:?([^}]*)\}?/g, "$1") + "\n```");
            items.push(s);
        }
        for (const f of ["println", "print", "to_string", "sqrt", "pow", "abs"]) {
            const c = new vscode.CompletionItem("std::" + f, vscode.CompletionItemKind.Function);
            c.insertText = "std::" + f;
            items.push(c);
        }
        return items;
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
        { language: "cpp2" }, completions, ":", "."));
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

module.exports = { activate, deactivate, __internals: { parseTypes, parseVars, parseEnums, resolveMembers, pointeeOf } };
