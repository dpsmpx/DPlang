// Объявления модулей: области имён, типы в сигнатурах, правила сводок
// (docs/spec/core.md, разделы 3 и 4).
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
        for (FnDef& f : prog_.fns) {
            if (f.node && f.is_extern) c_signature(f);
        }
        linear_placement();
        for (StructDef& d : prog_.structs) {
            if (!d.node) continue;
            std::vector<const void*> path;
            if (contains(prog_.types.struct_type(&d), &d, path, true)) {
                diags_.error(Code::E0423, d.span, "struct '" + d.name + "' contains itself; keep the nested values in a List");
            }
        }
        for (EnumDef& d : prog_.enums) {
            if (!d.node) continue;
            std::vector<const void*> path;
            if (contains(prog_.types.enum_type(&d), &d, path, true)) {
                diags_.error(Code::E0423, d.span, "enum '" + d.name + "' contains itself; keep the nested values in a List");
            }
        }
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

    // Значение типа t хранит внутри себя (не через List) значение типа
    // объявления self (E0423). path — объявления на пути, против зацикливания.
    bool contains(const sema::Type* t, const void* self, std::vector<const void*>& path, bool top) {
        if (!t) return false;
        switch (t->kind) {
            case TK::Optional: return contains(t->elem, self, path, false);
            case TK::Result: return contains(t->elem, self, path, false) || contains(t->err, self, path, false);
            case TK::Struct: case TK::Enum: {
                const void* d = t->kind == TK::Struct ? static_cast<const void*>(t->sdef) : t->edef;
                if (!top && d == self) return true;
                for (const void* p : path) {
                    if (p == d) return false;
                }
                path.push_back(d);
                bool found = false;
                if (t->kind == TK::Struct) {
                    for (const FieldDef& f : t->sdef->fields) found = found || contains(f.type, self, path, false);
                } else {
                    for (const VariantDef& v : t->edef->variants) {
                        for (const sema::Type* x : v.types) found = found || contains(x, self, path, false);
                        for (const FieldDef& f : v.fields) found = found || contains(f.type, self, path, false);
                    }
                }
                path.pop_back();
                return found;
            }
            default: return false;  // List, Map и срез хранят элементы отдельно
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
            return c_value_type(t) || t->kind == TK::Str || (t->kind == TK::Slice && c_value_type(t->elem)) ||
                   (t->kind == TK::Optional && t->elem->kind == TK::Handle);
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
        if (!t || t->kind == TK::Error || t->kind == TK::Void) return true;
        if (c_value_type(t) || t->kind == TK::Str) return true;
        return t->kind == TK::Optional && (t->elem->kind == TK::Str || t->elem->kind == TK::Handle);
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
