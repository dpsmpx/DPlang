// Объявления модулей: области имён, типы в сигнатурах, правила сводок
// (docs/spec/core.md, разделы 3 и 4).
#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "sema/builtins.h"
#include "sema/resolve.h"
#include "sema/sema.h"
#include "syntax/ast_dump.h"

namespace dp::sema {

namespace {

using namespace ast;

class Collector {
public:
    Collector(Program& prog, Diagnostics& diags) : prog_(prog), diags_(diags) {}

    void run() {
        for (auto& m : prog_.modules) declare(*m);
        for (auto& m : prog_.modules) signatures(*m);
        for (StructDef& d : prog_.structs) {
            if (d.node && d.c) c_struct(d);
        }
        prog_.c_layout_known = true;
        for (const Program::PendingCFn& p : prog_.pending_cfn) check_cfn(p.node, p.type, p.module, diags_);
        prog_.pending_cfn.clear();
        for (FnDef& f : prog_.fns) {
            if (f.node && f.is_extern) c_signature(f);
            if (f.node && f.exported) export_signature(f);
        }
        export_symbols();
        linear_placement();
        self_containment();
    }

private:
    Program& prog_;
    Diagnostics& diags_;

    void duplicate(Code code, Span span, const std::string& what, Span first) {
        Diagnostic& d = diags_.error(code, span, what);
        d.notes.push_back(Note{first, true, "first defined here"});
    }

    // Проход 1: имена объявлений модуля и импортированных модулей.
    void declare(ModuleInfo& m) {
        for (ModuleInfo* dep : m.imports) {
            Symbol s;
            s.kind = SymKind::Module;
            s.mod = dep;
            for (Item* it : m.ast->items) {
                if (it->kind != ItemKind::Import || it->name != dep->name) continue;
                s.span = it->name_span;
                if (!import_det(*static_cast<const ImportItem*>(it))) continue;
                if (dep->stdlib) {  // стандартная библиотека v0.1 — только os: ввод-вывод
                    diags_.error(Code::E0425, it->span, "module '" + dep->name +
                                                            "' does input or output and cannot be imported with '@det'");
                } else {
                    dep->det = true;
                }
            }
            m.scope[dep->name] = s;
        }
        for (Item* it : m.ast->items) {
            if (it->name.empty() || it->kind == ItemKind::Import || it->kind == ItemKind::Error) continue;
            Symbol s;
            s.span = it->name_span;
            switch (it->kind) {
                case ItemKind::Fn: {
                    FnDef& f = prog_.fns.emplace_back();
                    f.name = it->name;
                    f.module = &m;
                    f.pub = it->pub;
                    f.span = it->name_span;
                    f.node = static_cast<FnItem*>(it);
                    s.kind = SymKind::Fn;
                    s.fn = &f;
                    break;
                }
                case ItemKind::Struct: {
                    StructDef& d = prog_.structs.emplace_back();
                    d.name = it->name;
                    d.module = &m;
                    d.pub = it->pub;
                    d.span = it->name_span;
                    d.node = static_cast<StructItem*>(it);
                    s.kind = SymKind::Struct;
                    s.st = &d;
                    break;
                }
                case ItemKind::Enum: {
                    EnumDef& d = prog_.enums.emplace_back();
                    d.name = it->name;
                    d.module = &m;
                    d.pub = it->pub;
                    d.span = it->name_span;
                    d.node = static_cast<EnumItem*>(it);
                    s.kind = SymKind::Enum;
                    s.en = &d;
                    break;
                }
                case ItemKind::ExternType: {
                    HandleDef& d = prog_.handles.emplace_back();
                    d.name = it->name;
                    d.module = &m;
                    d.pub = it->pub;
                    d.span = it->name_span;
                    s.kind = SymKind::Handle;
                    s.hd = &d;
                    break;
                }
                case ItemKind::Const: {
                    ConstDef& d = prog_.consts.emplace_back();
                    d.name = it->name;
                    d.module = &m;
                    d.pub = it->pub;
                    d.span = it->name_span;
                    d.node = static_cast<ConstItem*>(it);
                    s.kind = SymKind::Const;
                    s.cn = &d;
                    break;
                }
                default: continue;
            }
            auto found = m.scope.find(it->name);
            if (found != m.scope.end()) {
                if (found->second.kind == SymKind::Module) {
                    duplicate(Code::E0305, it->name_span,
                              "'" + it->name + "' is also the name of an imported module", found->second.span);
                } else {
                    duplicate(Code::E0304, it->name_span, "'" + it->name + "' is defined twice", found->second.span);
                }
                continue;
            }
            m.scope[it->name] = s;
        }
    }

    // Проход 2: типы полей, вариантов и сигнатур.
    void signatures(ModuleInfo& m) {
        Resolver r(prog_, m, diags_);
        for (Item* it : m.ast->items) {
            auto found = m.scope.find(it->name);
            if (found == m.scope.end()) continue;
            Symbol& s = found->second;
            switch (it->kind) {
                case ItemKind::Struct:
                    if (s.st && s.st->node == it) struct_sig(r, *s.st);
                    break;
                case ItemKind::Enum:
                    if (s.en && s.en->node == it) enum_sig(r, *s.en);
                    break;
                case ItemKind::Fn:
                    if (s.fn && s.fn->node == it) fn_sig(r, *s.fn);
                    break;
                case ItemKind::Const:
                    if (s.cn && s.cn->node == it) {
                        s.cn->type = r.type(s.cn->node->type);
                        no_slice(s.cn->node->type, s.cn->type, "a constant");
                        if (s.cn->pub) public_type(s.cn->node->type, s.cn->type, "constant");
                    }
                    break;
                default: break;
            }
        }
    }

    // Объявления, которые значение типа t хранит прямо внутри себя: через
    // ?T и E!T, но не через List, Map и срез — те хранят элементы отдельно.
    void direct(const sema::Type* t, std::vector<const void*>& out) {
        if (!t) return;
        switch (t->kind) {
            case TK::Optional: direct(t->elem, out); break;
            case TK::Result:
                direct(t->elem, out);
                direct(t->err, out);
                break;
            case TK::Struct: out.push_back(t->sdef); break;
            case TK::Enum: out.push_back(t->edef); break;
            default: break;
        }
    }

    // E0423: структура или перечисление содержит себя, если лежит на цикле
    // графа «хранит прямо внутри» (type.recursive). Компоненты сильной
    // связности — алгоритм Тарьяна с явным стеком: O(объявлений + рёбер),
    // без рекурсии по длине цепочки и без повторных обходов на «ромбах».
    void self_containment() {
        std::vector<const void*> nodes;
        std::map<const void*, size_t> id;
        for (StructDef& d : prog_.structs) {
            if (d.node) id.emplace(&d, nodes.size()), nodes.push_back(&d);
        }
        size_t structs = nodes.size();
        for (EnumDef& d : prog_.enums) {
            if (d.node) id.emplace(&d, nodes.size()), nodes.push_back(&d);
        }
        std::vector<std::vector<size_t>> edges(nodes.size());
        for (size_t v = 0; v < nodes.size(); v++) {
            std::vector<const void*> out;
            if (v < structs) {
                for (const FieldDef& f : static_cast<const StructDef*>(nodes[v])->fields) direct(f.type, out);
            } else {
                for (const VariantDef& vd : static_cast<const EnumDef*>(nodes[v])->variants) {
                    for (const sema::Type* x : vd.types) direct(x, out);
                    for (const FieldDef& f : vd.fields) direct(f.type, out);
                }
            }
            for (const void* d : out) {
                auto it = id.find(d);
                if (it != id.end()) edges[v].push_back(it->second);
            }
        }
        const size_t none = SIZE_MAX;
        std::vector<size_t> index(nodes.size(), none), low(nodes.size(), 0), stack;
        std::vector<char> on_stack(nodes.size(), 0), cyclic(nodes.size(), 0);
        size_t counter = 0;
        struct Frame {
            size_t v, edge;
        };
        for (size_t root = 0; root < nodes.size(); root++) {
            if (index[root] != none) continue;
            std::vector<Frame> calls{{root, 0}};
            index[root] = low[root] = counter++;
            stack.push_back(root);
            on_stack[root] = 1;
            while (!calls.empty()) {
                size_t v = calls.back().v;
                if (calls.back().edge < edges[v].size()) {
                    size_t w = edges[v][calls.back().edge++];
                    if (w == v) cyclic[v] = 1;  // хранит себя прямо
                    if (index[w] == none) {
                        index[w] = low[w] = counter++;
                        stack.push_back(w);
                        on_stack[w] = 1;
                        calls.push_back({w, 0});
                    } else if (on_stack[w]) {
                        low[v] = std::min(low[v], index[w]);
                    }
                    continue;
                }
                if (low[v] == index[v]) {  // корень компоненты
                    size_t first = stack.size();
                    while (stack[first - 1] != v) first--;
                    bool loop = stack.size() - (first - 1) > 1;
                    for (size_t k = first - 1; k < stack.size(); k++) {
                        on_stack[stack[k]] = 0;
                        if (loop) cyclic[stack[k]] = 1;
                    }
                    stack.resize(first - 1);
                }
                calls.pop_back();
                if (!calls.empty()) low[calls.back().v] = std::min(low[calls.back().v], low[v]);
            }
        }
        for (size_t v = 0; v < nodes.size(); v++) {
            if (!cyclic[v]) continue;
            if (v < structs) {
                auto* d = static_cast<const StructDef*>(nodes[v]);
                diags_.error(Code::E0423, d->span, "struct '" + d->name + "' contains itself; keep the nested values in a List");
            } else {
                auto* d = static_cast<const EnumDef*>(nodes[v]);
                diags_.error(Code::E0423, d->span, "enum '" + d->name + "' contains itself; keep the nested values in a List");
            }
        }
    }

    // Открытое объявление не упоминает закрытых типов: иначе сводка
    // ссылалась бы на то, чего другие модули не видят.
    void public_type(const ast::Type* node, const sema::Type* t, const std::string& what) {
        if (!t || !node) return;
        switch (t->kind) {
            case TK::List: case TK::Slice: case TK::Optional:
                public_type(node, t->elem, what);
                break;
            case TK::Result:
                public_type(node, t->elem, what);
                public_type(node, t->err, what);
                break;
            case TK::Map:
                public_type(node, t->key, what);
                public_type(node, t->elem, what);
                break;
            case TK::CFn:
                for (const auto& p : t->sig->params) public_type(node, p.second, what);
                public_type(node, t->sig->ret, what);
                break;
            case TK::Struct:
                if (!t->sdef->pub) {
                    diags_.error(Code::E0309, node->span,
                                 "public " + what + " uses private type '" + t->sdef->name + "'");
                }
                break;
            case TK::Enum:
                if (!t->edef->pub) {
                    diags_.error(Code::E0309, node->span,
                                 "public " + what + " uses private type '" + t->edef->name + "'");
                }
                break;
            case TK::Handle:
                if (!t->hdef->pub) {
                    diags_.error(Code::E0309, node->span,
                                 "public " + what + " uses private type '" + t->hdef->name + "'");
                }
                break;
            default: break;
        }
    }

    // Срез — только тип параметра-значения (Р44).
    void no_slice(const ast::Type* node, const sema::Type* t, const std::string& where) {
        if (node && contains_slice(t)) {
            diags_.error(Code::E0510, node->span, "a slice cannot be " + where + "; use List[T]");
        }
    }

    void fields(Resolver& r, const std::vector<FieldDecl>& decls, std::vector<FieldDef>& out, bool pub,
                const std::string& what) {
        for (const FieldDecl& fd : decls) {
            bool dup = false;
            for (const FieldDef& prev : out) {
                if (prev.name == fd.name) {
                    duplicate(Code::E0318, fd.name_span, "field '" + fd.name + "' is defined twice", prev.span);
                    dup = true;
                }
            }
            const sema::Type* t = r.type(fd.type);
            if (pub) public_type(fd.type, t, what);
            no_slice(fd.type, t, "a field");
            if (dup) continue;
            out.push_back(FieldDef{fd.name, fd.name_span, t, fd.value});
        }
    }

    void struct_sig(Resolver& r, StructDef& d) {
        for (const Attr& a : d.node->attrs) {
            bool* flag = a.name == "c" ? &d.c : a.name == "linear" ? &d.linear : nullptr;
            if (!flag) {
                diags_.error(Code::E0319, a.span, "unknown attribute '@" + a.name + "' for a struct; use '@c' or '@linear'");
            } else if (*flag) {
                diags_.error(Code::E0319, a.span, "attribute '@" + a.name + "' is given twice");
            } else if (a.has_arg) {
                diags_.error(Code::E0319, a.span, "attribute '@" + a.name + "' takes no argument");
            }
            if (flag) *flag = true;
        }
        fields(r, d.node->fields, d.fields, d.pub, "struct");
    }

    // Где лежат линейные значения (Р60, E0514): поле — только у `@linear`,
    // не в List, Map, срезе и данных варианта. Проверка — когда `@linear`
    // всех структур известен.
    void linear_placement() {
        auto container = [&](const ast::Type* node, const sema::Type* t, const ModuleInfo* m) {
            if (node && linear_in_container(t)) {
                diags_.error(Code::E0514, node->span,
                             "'" + type_name(t, m) + "' holds a '@linear' value in a List, a Map or a slice; "
                             "keep resources in variables or fields of a '@linear' struct");
            }
        };
        for (StructDef& d : prog_.structs) {
            if (!d.node) continue;
            for (size_t i = 0; i < d.fields.size() && i < d.node->fields.size(); i++) {
                const ast::Type* node = d.node->fields[i].type;
                if (linear_type(d.fields[i].type) && !d.linear) {
                    diags_.error(Code::E0514, node->span,
                                 "field '" + d.fields[i].name + "' holds a '@linear' value; mark struct '" + d.name +
                                     "' '@linear' too");
                }
                container(node, d.fields[i].type, d.module);
            }
        }
        for (EnumDef& d : prog_.enums) {
            if (!d.node) continue;
            for (size_t v = 0; v < d.variants.size() && v < d.node->variants.size(); v++) {
                const VariantDecl& vd = d.node->variants[v];
                auto payload = [&](const ast::Type* node, const sema::Type* t) {
                    if (linear_type(t)) {
                        diags_.error(Code::E0514, node->span,
                                     "variant '" + vd.name + "' cannot hold a '@linear' value; use a '@linear' struct");
                    }
                    container(node, t, d.module);
                };
                for (size_t k = 0; k < d.variants[v].types.size() && k < vd.types.size(); k++) {
                    payload(vd.types[k], d.variants[v].types[k]);
                }
                for (size_t k = 0; k < d.variants[v].fields.size() && k < vd.fields.size(); k++) {
                    payload(vd.fields[k].type, d.variants[v].fields[k].type);
                }
            }
        }
        for (FnDef& f : prog_.fns) {
            if (!f.node) continue;
            for (size_t i = 0; i < f.params.size() && i < f.node->params.size(); i++) {
                container(f.node->params[i].type, f.params[i].type, f.module);
            }
            container(f.node->ret, f.ret, f.module);
        }
    }

    // `@c` (Р59): поля — типы с видом в C; проверка — когда поля всех структур известны.
    void c_struct(const StructDef& d) {
        for (size_t i = 0; i < d.fields.size() && i < d.node->fields.size(); i++) {
            const FieldDef& f = d.fields[i];
            if (f.type && f.type->kind != TK::Error && !c_value_type(f.type)) {
                diags_.error(Code::E0326, d.node->fields[i].type->span,
                             "field '" + f.name + "' of '@c' struct '" + d.name + "' has type '" + type_name(f.type, d.module) +
                                 "', which has no C layout; use integers, floats, bool, 'extern type' or '@c' structs");
            }
        }
    }

    // Тип параметра функции C (Р59): значение с видом в C; str — строка
    // с нулём в конце; []T — указатель на элементы; &T, &mut T — указатель;
    // &mut List[T] — указатель на элементы для записи; ?H — дескриптор или NULL.
    bool c_param(const sema::Type* t, ParamMode mode) {
        if (!t || t->kind == TK::Error) return true;
        if (mode == ParamMode::Value) {
            return export_param(t, mode) || (t->kind == TK::Slice && c_value_type(t->elem));
        }
        return c_value_type(t) || (mode == ParamMode::Mut && t->kind == TK::List && c_value_type(t->elem));
    }

    // Типы функции C — когда известны `@c` всех структур.
    void c_signature(const FnDef& f) {
        const FnItem* n = f.node;
        for (size_t i = 0; i < f.params.size() && i < n->params.size(); i++) {
            const ParamDef& p = f.params[i];
            if (!c_param(p.type, p.mode)) {
                diags_.error(Code::E0326, n->params[i].type->span,
                             "parameter '" + p.name + "' of C function '" + f.name + "' has type '" +
                                 type_name(p.type, f.module) + "', which cannot cross to C (spec: ffi.types)");
            }
        }
        if (!c_result(f.ret)) {
            diags_.error(Code::E0326, n->ret ? n->ret->span : f.span,
                         "C function '" + f.name + "' returns '" + type_name(f.ret, f.module) +
                             "', which cannot cross from C (spec: ffi.types)");
        }
    }

    bool c_result(const sema::Type* t) {
        if (!t || t->kind == TK::Error) return true;
        return export_result(t) || t->kind == TK::Str || (t->kind == TK::Optional && t->elem->kind == TK::Str);
    }

    // `@export` (Р61): сигнатура — для вызова из C, как у `extern fn(...)`;
    // строку и ссылку C отдаёт только на время вызова, `E!T` в C нет.
    void export_signature(const FnDef& f) {
        const FnItem* n = f.node;
        for (size_t i = 0; i < f.params.size() && i < n->params.size(); i++) {
            const ParamDef& p = f.params[i];
            if (!export_param(p.type, p.mode)) {
                diags_.error(Code::E0326, n->params[i].type->span,
                             "parameter '" + p.name + "' of exported function '" + f.name + "' has type '" +
                                 std::string(p.mode == ParamMode::Ref ? "&" : p.mode == ParamMode::Mut ? "&mut " : "") +
                                 type_name(p.type, f.module) + "', which C cannot pass (spec: ffi.export)");
            }
        }
        if (!export_result(f.ret)) {
            std::string hint = f.ret->kind == TK::Result ? "; C has no 'E!T': return a code" : "";
            diags_.error(Code::E0326, n->ret ? n->ret->span : f.span,
                         "exported function '" + f.name + "' returns '" + type_name(f.ret, f.module) +
                             "', which C cannot take (spec: ffi.export)" + hint);
        }
    }

    // Символы C программы (Р61): у `@export` — свои, не повторяются, не
    // совпадают с `extern fn`, словами C (их не объявить в заголовке),
    // `main` и символами runtime (`dp_…`).
    void export_symbols() {
        static const char* const kKeywords[] = {
            "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
            "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
            "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
            "volatile", "while", "alignas", "alignof", "bool", "true", "false", "nullptr", "static_assert",
            "thread_local", "typeof", "constexpr"};
        std::map<std::string, const FnDef*> taken;
        for (FnDef& f : prog_.fns) {
            if (f.node && f.is_extern) taken.emplace(f.name, &f);
        }
        for (FnDef& f : prog_.fns) {
            if (!f.node || !f.exported) continue;
            const std::string& s = f.export_name;
            bool keyword = false;
            for (const char* k : kKeywords) keyword = keyword || s == k;
            if (keyword || s == "main" || s.rfind("dp_", 0) == 0) {
                diags_.error(Code::E0327, f.export_span,
                             "C symbol '" + s + "' is reserved" +
                                 (keyword ? ": it is a C keyword" : s == "main" ? ": it is the C entry point" : ": names starting with 'dp_' belong to the dpl runtime") +
                                 "; choose another with @export(\"name\")");
                continue;
            }
            auto [it, fresh] = taken.emplace(s, &f);
            if (!fresh) {
                const FnDef& other = *it->second;
                Diagnostic& d = diags_.error(Code::E0327, f.export_span,
                                             "C symbol '" + s + "' is already " +
                                                 (other.is_extern ? "declared by 'extern fn " + other.name + "'"
                                                                  : "exported by '" + other.module->name + "." + other.name + "'"));
                d.notes.push_back(Note{other.exported ? other.export_span : other.span, true, "first here"});
            }
        }
    }

    void enum_sig(Resolver& r, EnumDef& d) {
        for (const VariantDecl& vd : d.node->variants) {
            int prev = d.find(vd.name);
            if (prev >= 0) {
                duplicate(Code::E0318, vd.name_span, "variant '" + vd.name + "' is defined twice",
                          d.variants[static_cast<size_t>(prev)].span);
                continue;
            }
            VariantDef v;
            v.name = vd.name;
            v.span = vd.name_span;
            v.form = vd.form;
            for (ast::Type* t : vd.types) {
                const sema::Type* rt = r.type(t);
                if (d.pub) public_type(t, rt, "enum");
                no_slice(t, rt, "a variant value");
                v.types.push_back(rt);
            }
            fields(r, vd.fields, v.fields, d.pub, "enum");
            d.variants.push_back(std::move(v));
        }
    }

    static bool c_identifier(const std::string& s) {
        if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
        for (char c : s) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) return false;
        }
        return true;
    }

    // `@det import m` (Р55): у импорта — только `@det`, один раз.
    bool import_det(const ImportItem& im) {
        bool det = false;
        for (const Attr& a : im.attrs) {
            if (a.name == "test") {
                diags_.error(Code::E0205, a.span, "'@test' marks a function, not an import");
            } else if (a.name != "det") {
                diags_.error(Code::E0319, a.span, "unknown attribute '@" + a.name + "'");
            } else if (det) {
                diags_.error(Code::E0319, a.span, "attribute '@det' is given twice");
            }
            det = det || a.name == "det";
        }
        return det;
    }

    void fn_sig(Resolver& r, FnDef& f) {
        const FnItem* n = f.node;
        f.is_extern = n->is_extern;
        bool linked = false;
        for (const Attr& a : n->attrs) {
            if (a.name == "export") {  // `@export`, `@export("name")` — функцию зовёт C (Р61)
                if (n->is_extern) {
                    diags_.error(Code::E0319, a.span, "an 'extern fn' is already C; '@export' marks a dpl function for C");
                } else if (f.exported) {
                    diags_.error(Code::E0319, a.span, "attribute '@export' is given twice");
                } else if (a.has_arg && !c_identifier(a.arg)) {
                    diags_.error(Code::E0319, a.span, "'@export' needs a C name: a letter or '_', then letters, digits or '_'");
                }
                if (!n->is_extern && !f.exported) {
                    f.exported = true;
                    f.export_name = a.has_arg ? a.arg : f.name;
                    f.export_span = a.span;
                }
                continue;
            }
            if (a.name == "link") {  // `@link("lib")` — только у `extern fn` (Р59)
                if (!n->is_extern) {
                    diags_.error(Code::E0319, a.span, "'@link' names the C library of an 'extern fn'");
                } else if (!a.has_arg || a.arg.empty()) {
                    diags_.error(Code::E0319, a.span, "'@link' needs a library name: @link(\"SDL3\")");
                } else if (linked) {
                    diags_.error(Code::E0319, a.span, "attribute '@link' is given twice");
                }
                linked = true;
                f.link = a.arg;
                continue;
            }
            bool& flag = a.name == "det" ? f.det : f.test;
            if (a.name != "det" && a.name != "test") {
                diags_.error(Code::E0319, a.span, "unknown attribute '@" + a.name + "'");
            } else if (flag) {
                diags_.error(Code::E0319, a.span, "attribute '@" + a.name + "' is given twice");
            } else if (n->is_extern) {
                diags_.error(Code::E0319, a.span, "an 'extern fn' cannot be '@" + a.name + "': C code is outside dpl");
            } else if (a.has_arg) {
                diags_.error(Code::E0319, a.span, "attribute '@" + a.name + "' takes no argument");
            }
            if (a.name == "det" || a.name == "test") flag = true;
        }
        for (const Param& p : n->params) {
            for (const ParamDef& prev : f.params) {
                if (prev.name == p.name) {
                    duplicate(Code::E0318, p.name_span, "parameter '" + p.name + "' is defined twice", prev.span);
                }
            }
            const sema::Type* t = r.type(p.type);
            if (f.pub) public_type(p.type, t, "function");
            // Срез — сам параметр-значение; внутри других типов и за ссылкой его нет.
            if (p.mode != ParamMode::Value || (t->kind == TK::Slice ? contains_slice(t->elem) : contains_slice(t))) {
                no_slice(p.type, t, p.mode != ParamMode::Value ? "behind a reference" : "nested in another type");
            }
            f.params.push_back(ParamDef{p.name, p.mode, t, p.name_span});
        }
        if (n->ret) {
            f.ret = r.type(n->ret, /*allow_void=*/false);
            if (f.pub) public_type(n->ret, f.ret, "function");
            no_slice(n->ret, f.ret, "a result");
            auto* res = as<ResultType>(n->ret);
            f.infer_error = res && !res->error && !f.pub;
            if (f.pub && res && !res->error) {
                diags_.error(Code::E0308, n->ret->span,
                             "public function '" + f.name + "' must name its error type, for example 'MyError" +
                                 type_text(n->ret) + "'");
            }
        } else {
            f.ret = prog_.types.prim(TK::Void);
        }
        if (f.test && f.exported) {
            diags_.error(Code::E0319, f.export_span, "a test cannot be '@export': 'dpl test' calls it");
        }
        if (f.test) {
            bool ok_ret = f.ret->kind == TK::Void || f.ret->kind == TK::Error ||
                          (f.ret->kind == TK::Result && f.ret->elem->kind == TK::Void);
            if (!f.params.empty() || !ok_ret) {
                diags_.error(Code::E0323, f.span,
                             "test '" + f.name + "' must take no parameters and return nothing or 'E!void'");
            }
        }
    }
};

}  // namespace

void collect(Program& prog, Diagnostics& diags) { Collector(prog, diags).run(); }

}  // namespace dp::sema
