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
    bool pub = (s.fn && s.fn->pub) || (s.st && s.st->pub) || (s.en && s.en->pub) || (s.cn && s.cn->pub);
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
            if (s && (s->kind == SymKind::Struct || s->kind == SymKind::Enum)) {
                if (!n->args.empty()) {
                    diags_.error(Code::E0314, node->span, "type '" + n->name + "' takes no type arguments");
                }
                out = s->kind == SymKind::Struct ? tt.struct_type(s->st) : tt.enum_type(s->en);
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
        case TypeKind::Error: break;
    }
    node->resolved = out;
    return out;
}

}  // namespace dp::sema
