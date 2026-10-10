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
        fields(r, d.node->fields, d.fields, d.pub, "struct");
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
        for (const Attr& a : n->attrs) {
            bool& flag = a.name == "det" ? f.det : f.test;
            if (a.name != "det" && a.name != "test") {
                diags_.error(Code::E0319, a.span, "unknown attribute '@" + a.name + "'");
            } else if (flag) {
                diags_.error(Code::E0319, a.span, "attribute '@" + a.name + "' is given twice");
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
