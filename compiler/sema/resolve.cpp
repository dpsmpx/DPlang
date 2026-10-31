#include "sema/resolve.h"

#include "sema/builtins.h"

namespace dp::sema {

using namespace ast;

const Symbol* Resolver::lookup(const std::string& name) const {
    auto it = m_.scope.find(name);
    return it == m_.scope.end() ? nullptr : &it->second;
}

const Symbol* Resolver::member(const ModuleInfo& mod, const std::string& name, Span span) {
    auto it = mod.scope.find(name);
    if (it == mod.scope.end() || it->second.kind == SymKind::Module) {
        diags_.error(Code::E0311, span, "module '" + mod.name + "' has no item '" + name + "'");
        return nullptr;
    }
    const Symbol& s = it->second;
    bool pub = (s.fn && s.fn->pub) || (s.st && s.st->pub) || (s.en && s.en->pub) || (s.cn && s.cn->pub) ||
               (s.hd && s.hd->pub);
    if (!pub) {
        Diagnostic& d = diags_.error(Code::E0306, span, "'" + name + "' is private to module '" + mod.name + "'");
        d.notes.push_back(Note{s.span, true, "declared here without 'pub'"});
        return nullptr;
    }
    return &s;
}

const sema::Type* Resolver::type(ast::Type* node, bool allow_void) {
    TypeTable& tt = prog_.types;
    const sema::Type* out = tt.error();
    if (!node) return out;
    switch (node->kind) {
        case TypeKind::Name: {
            auto* n = static_cast<NameType*>(node);
            const Symbol* s = nullptr;
            if (!n->module.empty()) {
                const Symbol* ms = lookup(n->module);
                if (!ms || ms->kind != SymKind::Module) {
                    diags_.error(Code::E0313, node->span, "cannot find type '" + n->module + "." + n->name +
                                                              "': '" + n->module + "' is not an imported module");
                    break;
                }
                s = member(*ms->mod, n->name, node->span);
                if (!s) break;
            } else {
                s = lookup(n->name);
            }
            if (s && (s->kind == SymKind::Struct || s->kind == SymKind::Enum || s->kind == SymKind::Handle)) {
                if (!n->args.empty()) {
                    diags_.error(Code::E0314, node->span, "type '" + n->name + "' takes no type arguments");
                }
                out = s->kind == SymKind::Struct ? tt.struct_type(s->st)
                      : s->kind == SymKind::Enum ? tt.enum_type(s->en)
                                                 : tt.handle(s->hd);
                break;
            }
            int b = n->module.empty() ? find_builtin_type(n->name) : -1;
            if (s || b < 0) {
                diags_.error(Code::E0313, node->span, "cannot find type '" + n->name + "'");
                break;
            }
            TK k = kBuiltinTypes[b].kind;
            if (k == TK::List) {
                if (n->args.size() != 1) {
                    diags_.error(Code::E0314, node->span, "'List' takes exactly one type argument: List[T]");
                    break;
                }
                out = tt.list(type(n->args[0]));
                break;
            }
            if (k == TK::Atomic) {  // Р70: целое или bool
                if (n->args.size() != 1) {
                    diags_.error(Code::E0314, node->span, "'Atomic' takes exactly one type argument: Atomic[T]");
                    break;
                }
                const sema::Type* elem = type(n->args[0]);
                if (elem->kind == TK::Error) break;
                if (!is_integer(elem) && elem->kind != TK::Bool) {
                    diags_.error(Code::E0314, n->args[0]->span,
                                 "'Atomic' holds an integer or a bool, found '" + type_name(elem) + "'");
                    break;
                }
                out = tt.atomic(elem);
                break;
            }
            if (k == TK::Mutex || k == TK::Channel) {  // Р70
                if (n->args.size() != 1) {
                    diags_.error(Code::E0314, node->span, "'" + n->name + "' takes exactly one type argument: " + n->name +
                                                              "[T]");
                    break;
                }
                const sema::Type* elem = type(n->args[0]);
                if (elem->kind == TK::Error) break;
                out = k == TK::Mutex ? tt.mutex(elem) : tt.channel(elem);
                break;
            }
            if (k == TK::Pool || k == TK::PoolHandle) {  // Р67
                if (n->args.size() != 1) {
                    diags_.error(Code::E0314, node->span, "'" + n->name + "' takes exactly one type argument: " + n->name +
                                                              "[T]");
                    break;
                }
                const sema::Type* elem = type(n->args[0]);
                if (elem->kind == TK::Error) break;
                out = k == TK::Pool ? tt.pool(elem) : tt.pool_handle(elem);
                break;
            }
            if (k == TK::Map) {
                if (n->args.size() != 2) {
                    diags_.error(Code::E0314, node->span, "'Map' takes exactly two type arguments: Map[K, V]");
                    break;
                }
                const sema::Type* key = type(n->args[0]);
                const sema::Type* value = type(n->args[1]);
                if (key->kind == TK::Error || value->kind == TK::Error) break;
                if (!map_key_type(key)) {
                    diags_.error(Code::E0424, n->args[0]->span,
                                 "a Map key must be an integer, a bool or a str, found '" + type_name(key) + "'");
                    break;
                }
                out = tt.map(key, value);
                break;
            }
            if (!n->args.empty()) {
                diags_.error(Code::E0314, node->span, "type '" + n->name + "' takes no type arguments");
                break;
            }
            if (k == TK::Void && !allow_void) {
                diags_.error(Code::E0315, node->span, "'void' is only allowed as the value of a result: '!void'");
                break;
            }
            out = tt.prim(k);
            break;
        }
        case TypeKind::Slice: out = tt.slice(type(static_cast<SliceType*>(node)->elem)); break;
        case TypeKind::Optional: out = tt.optional(type(static_cast<OptionalType*>(node)->inner)); break;
        case TypeKind::Result: {
            auto* r = static_cast<ResultType*>(node);
            const sema::Type* value = type(r->value, true);
            const sema::Type* err = nullptr;
            if (r->error) {
                err = type(r->error);
                if (err->kind != TK::Enum && err->kind != TK::Error) {
                    diags_.error(Code::E0316, r->error->span,
                                 "error type '" + type_name(err) + "' is not an enum");
                    err = tt.error();
                }
            }
            out = tt.result(err, value);
            break;
        }
        case TypeKind::Array: {  // `[N]T` (Р62)
            auto* a = static_cast<ArrayType*>(node);
            const sema::Type* elem = type(a->elem);
            if (elem->kind == TK::Error) break;
            if (a->len == 0 || a->len > (uint64_t{1} << 24)) {
                diags_.error(Code::E0328, node->span, "the length of '[N]T' must be from 1 to 16777216");
                break;
            }
            out = tt.array(elem, a->len);
            break;
        }
        case TypeKind::Ref: {  // `&T` поля `@c` (Р62)
            const sema::Type* inner = type(static_cast<RefType*>(node)->inner);
            if (inner->kind != TK::Error) out = tt.ref(inner);
            break;
        }
        case TypeKind::Fn: {  // `extern fn(...)` (Р61)
            auto* f = static_cast<FnType*>(node);
            CFnSig sig;
            bool bad = false;
            for (const FnType::Param& p : f->params) {
                const sema::Type* t = type(p.type);
                bad = bad || t->kind == TK::Error;
                sig.params.emplace_back(p.mode, t);
            }
            sig.ret = f->ret ? type(f->ret) : tt.prim(TK::Void);
            if (bad || sig.ret->kind == TK::Error) break;
            out = tt.cfn(sig);
            // Виды в C известны, когда известны `@c` всех структур (collect).
            if (prog_.c_layout_known) check_cfn(f, out);
            else prog_.pending_cfn.push_back({f, out, &m_});
            break;
        }
        case TypeKind::Error: break;
    }
    node->resolved = out;
    return out;
}

void check_cfn(const ast::FnType* node, const sema::Type* t, const ModuleInfo* m, Diagnostics& diags) {
    for (size_t i = 0; i < t->sig->params.size() && i < node->params.size(); i++) {
        const auto& [mode, p] = t->sig->params[i];
        if (!export_param(p, mode)) {
            diags.error(Code::E0326, node->params[i].type->span,
                        "a parameter of '" + type_name(t, m) + "' cannot be '" + type_name(p, m) +
                            "': it has no C form (spec: ffi.fn)");
        }
    }
    if (!export_result(t->sig->ret)) {
        diags.error(Code::E0326, node->ret ? node->ret->span : node->span,
                    "'" + type_name(t, m) + "' cannot return '" + type_name(t->sig->ret, m) +
                        "': it has no C form (spec: ffi.fn)");
    }
}

void Resolver::check_cfn(const ast::FnType* node, const sema::Type* t) { sema::check_cfn(node, t, &m_, diags_); }

}  // namespace dp::sema
