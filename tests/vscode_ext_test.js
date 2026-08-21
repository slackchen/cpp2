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
            },
            commands: { registerCommand: () => ({ dispose() {} }) },
            CompletionItem: function (label, kind) { this.label = label; this.kind = kind; },
            CompletionItemKind: { Keyword: 14, TypeParameter: 25, Snippet: 15, Function: 3, Field: 4, Method: 2 },
            SnippetString: function (v) { this.value = v; },
            MarkdownString: function (v) { this.value = v; },
            Position: function () {}, Range: function () {},
            DocumentSymbol: function () {}, CodeLens: function () {},
            Diagnostic: function () {}, DiagnosticSeverity: {},
            Uri: { file: (f) => ({ fsPath: f }) },
        };
    }
    return origLoad.apply(this, arguments);
};

const { __internals } = require("../editors/vscode/extension.js");
const { parseTypes, parseVars, parseEnums, resolveMembers, pointeeOf } = __internals;
const fs = require("fs");

let failed = 0;
function check(name, cond) {
    if (cond) console.log("PASS " + name);
    else { console.log("FAIL " + name); failed++; }
}

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

console.log(failed === 0 ? "\nALL PASS" : `\n${failed} FAILURES`);
process.exit(failed === 0 ? 0 : 1);
