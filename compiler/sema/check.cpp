// Проверка тел: имена (docs/spec/core.md, раздел 3). Типы — шаг 5,
// в этом же обходе.
#include <vector>

#include "sema/builtins.h"
#include "sema/resolve.h"
#include "sema/sema.h"

namespace dp::sema {

namespace {

using namespace ast;

Res* res_of(Expr* e) {
    if (auto* n = as<NameExpr>(e)) return &n->res;
    if (auto* f = as<FieldExpr>(e)) return &f->res;
    return nullptr;
}

class BodyChecker {
public:
    BodyChecker(Program& prog, ModuleInfo& m, Diagnostics& diags)
        : prog_(prog), m_(m), diags_(diags), r_(prog, m, diags) {}

    void run() {
        for (Item* it : m_.ast->items) {
            switch (it->kind) {
                case ItemKind::Fn: fn(static_cast<FnItem*>(it)); break;
                case ItemKind::Const: expr(static_cast<ConstItem*>(it)->value); break;
                case ItemKind::Struct:
                    for (FieldDecl& f : static_cast<StructItem*>(it)->fields) expr(f.value);
                    break;
                default: break;
            }
        }
    }

private:
    Program& prog_;
    ModuleInfo& m_;
    Diagnostics& diags_;
    Resolver r_;
    std::vector<std::vector<Local*>> scopes_;
    uint32_t next_local_ = 0;
    int or_depth_ = 0;  // внутри `p | q` имена связывать нельзя

    // --- области

    Local* find_local(const std::string& name) const {
        for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s) {
            for (auto l = s->rbegin(); l != s->rend(); ++l) {
                if ((*l)->name == name) return *l;
            }
        }
        return nullptr;
    }

    Local* declare(const std::string& name, Span span, bool mutable_, bool is_ref) {
        if (name == "_" || name.empty()) return nullptr;
        if (or_depth_ > 0) {
            diags_.error(Code::E0321, span, "patterns joined by '|' cannot bind names");
            return nullptr;
        }
        if (Local* prev = find_local(name)) {
            Diagnostic& d = diags_.error(Code::E0310, span, "'" + name + "' is already defined in this function");
            d.notes.push_back(Note{prev->span, true, "previous definition here"});
            return nullptr;
        }
        Local& l = prog_.locals.emplace_back();
        l.name = name;
        l.span = span;
        l.mutable_ = mutable_;
        l.is_ref = is_ref;
        l.id = next_local_++;
        scopes_.back().push_back(&l);
        return &l;
    }

    struct Scope {
        BodyChecker& c;
        explicit Scope(BodyChecker& checker) : c(checker) { c.scopes_.emplace_back(); }
        ~Scope() { c.scopes_.pop_back(); }
    };

    // --- объявления

    void fn(FnItem* f) {
        next_local_ = 0;
        Scope s(*this);
        for (Param& p : f->params) {
            if (find_local(p.name)) continue;  // повтор параметра — уже E0318 при сборе объявлений
            p.local = declare(p.name, p.name_span, p.mode == ParamMode::Mut, p.mode != ParamMode::Value);
        }
        block(f->body);
    }

    void block(Block* b) {
        if (!b) return;
        Scope s(*this);
        for (Stmt* st : b->stmts) stmt(st);
    }

    // --- инструкции

    void stmt(Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Let: {
                auto* l = static_cast<LetStmt*>(s);
                if (l->type) r_.type(l->type);
                expr(l->value);
                l->local = declare(l->name, l->name_span, l->is_var, false);
                break;
            }
            case StmtKind::Assign: {
                auto* a = static_cast<AssignStmt*>(s);
                auto* n = as<NameExpr>(a->target);
                if (!(n && n->name == "_" && a->op == AssignOp::Set)) expr(a->target);
                expr(a->value);
                break;
            }
            case StmtKind::Expr: expr(static_cast<ExprStmt*>(s)->expr); break;
            case StmtKind::If: {
                auto* i = static_cast<IfStmt*>(s);
                {
                    Scope sc(*this);
                    if (i->cond) {
                        expr(i->cond);
                    } else {
                        expr(i->let_value);
                        i->let_local = declare(i->let_name, i->let_span, false, false);
                    }
                    block(i->then);
                }
                stmt(i->els);
                break;
            }
            case StmtKind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                expr(w->cond);
                block(w->body);
                break;
            }
            case StmtKind::For: {
                auto* f = static_cast<ForStmt*>(s);
                expr(f->iter);
                expr(f->hi);
                Scope sc(*this);
                f->local = declare(f->var, f->var_span, f->mode == ForMode::Mut,
                                   f->mode == ForMode::Ref || f->mode == ForMode::Mut);
                block(f->body);
                break;
            }
            case StmtKind::Match: {
                auto* m = static_cast<MatchStmt*>(s);
                expr(m->subject);
                for (MatchArm& a : m->arms) {
                    Scope sc(*this);
                    pat(a.pat);
                    stmt(a.body);
                }
                break;
            }
            case StmtKind::Return: expr(static_cast<ReturnStmt*>(s)->value); break;
            case StmtKind::Defer: stmt(static_cast<DeferStmt*>(s)->body); break;
            case StmtKind::Fail: expr(static_cast<FailStmt*>(s)->value); break;
            case StmtKind::Block: block(static_cast<BlockStmt*>(s)->block); break;
            case StmtKind::Break: case StmtKind::Continue: case StmtKind::Error: break;
        }
    }

    // --- выражения

    void name(NameExpr* n) {
        if (n->name == "_") {
            diags_.error(Code::E0307, n->span, "'_' is not a value; it can only discard one: '_ = e'");
            return;
        }
        if (Local* l = find_local(n->name)) {
            n->res.kind = ResKind::Local;
            n->res.local = l;
            return;
        }
        if (const Symbol* s = r_.lookup(n->name)) {
            switch (s->kind) {
                case SymKind::Fn: n->res.kind = ResKind::Fn; n->res.fn = s->fn; break;
                case SymKind::Const: n->res.kind = ResKind::Const; n->res.cn = s->cn; break;
                case SymKind::Struct: n->res.kind = ResKind::Struct; n->res.st = s->st; break;
                case SymKind::Enum: n->res.kind = ResKind::Enum; n->res.en = s->en; break;
                case SymKind::Module: n->res.kind = ResKind::Module; n->res.mod = s->mod; break;
            }
            return;
        }
        int b = find_builtin_fn(n->name);
        if (b >= 0) {
            n->res.kind = ResKind::BuiltinFn;
            n->res.index = static_cast<uint32_t>(b);
            return;
        }
        b = find_builtin_type(n->name);
        if (b >= 0) {
            n->res.kind = ResKind::BuiltinType;
            n->res.index = static_cast<uint32_t>(b);
            return;
        }
        diags_.error(Code::E0307, n->span, "cannot find '" + n->name + "' in this scope");
    }

    void set_symbol(Res& res, const Symbol& s) {
        switch (s.kind) {
            case SymKind::Fn: res.kind = ResKind::Fn; res.fn = s.fn; break;
            case SymKind::Const: res.kind = ResKind::Const; res.cn = s.cn; break;
            case SymKind::Struct: res.kind = ResKind::Struct; res.st = s.st; break;
            case SymKind::Enum: res.kind = ResKind::Enum; res.en = s.en; break;
            case SymKind::Module: res.kind = ResKind::Module; res.mod = s.mod; break;
        }
    }

    void field(FieldExpr* f) {
        expr(f->object);
        Res* obj = res_of(f->object);
        if (!obj) return;
        if (obj->kind == ResKind::Module) {
            if (const Symbol* s = r_.member(*obj->mod, f->name, f->name_span)) set_symbol(f->res, *s);
        } else if (obj->kind == ResKind::Enum) {
            int v = obj->en->find(f->name);
            if (v < 0) {
                diags_.error(Code::E0312, f->name_span, "enum '" + obj->en->name + "' has no variant '" + f->name + "'");
                return;
            }
            f->res.kind = ResKind::Variant;
            f->res.en = obj->en;
            f->res.index = static_cast<uint32_t>(v);
        }
    }

    void field_inits(std::vector<FieldInit>& fields) {
        for (FieldInit& fi : fields) {
            if (fi.value) {
                expr(fi.value);
            } else {
                // Краткая запись `name` — значение имени `name`.
                fi.shorthand = m_.ast->make<NameExpr>();
                fi.shorthand->name = fi.name;
                fi.shorthand->span = fi.name_span;
                name(fi.shorthand);
            }
        }
    }

    void expr(Expr* e) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::Name: name(static_cast<NameExpr*>(e)); break;
            case ExprKind::Field: field(static_cast<FieldExpr*>(e)); break;
            case ExprKind::Index: {
                auto* i = static_cast<IndexExpr*>(e);
                expr(i->object);
                expr(i->index);
                break;
            }
            case ExprKind::Slice: {
                auto* s = static_cast<SliceExpr*>(e);
                expr(s->object);
                expr(s->lo);
                expr(s->hi);
                break;
            }
            case ExprKind::Call: {
                auto* c = static_cast<CallExpr*>(e);
                expr(c->callee);
                for (Expr* a : c->args) expr(a);
                break;
            }
            case ExprKind::Unary: expr(static_cast<UnaryExpr*>(e)->operand); break;
            case ExprKind::Binary: {
                auto* b = static_cast<BinaryExpr*>(e);
                expr(b->lhs);
                expr(b->rhs);
                break;
            }
            case ExprKind::Try: expr(static_cast<TryExpr*>(e)->operand); break;
            case ExprKind::Paren: expr(static_cast<ParenExpr*>(e)->inner); break;
            case ExprKind::Ref: expr(static_cast<RefExpr*>(e)->operand); break;
            case ExprKind::StructLit: {
                auto* s = static_cast<StructLitExpr*>(e);
                expr(s->path);
                if (Res* p = res_of(s->path)) {
                    if (p->kind == ResKind::Struct || p->kind == ResKind::Variant) {
                        s->res = *p;
                    } else if (p->kind != ResKind::None) {
                        diags_.error(Code::E0320, s->path->span, "struct literal needs a struct or an enum variant");
                    }
                }
                field_inits(s->fields);
                break;
            }
            case ExprKind::VariantLit: {
                auto* v = static_cast<VariantLitExpr*>(e);
                for (Expr* a : v->args) expr(a);
                field_inits(v->fields);
                break;
            }
            case ExprKind::ListLit:
                for (Expr* i : static_cast<ListLitExpr*>(e)->items) expr(i);
                break;
            case ExprKind::Int: case ExprKind::Float: case ExprKind::Str: case ExprKind::Byte: case ExprKind::Bool:
            case ExprKind::None: case ExprKind::Error:
                break;
        }
    }

    // --- образцы

    void pat(Pat* p) {
        if (!p) return;
        switch (p->kind) {
            case PatKind::Bind: {
                auto* b = static_cast<BindPat*>(p);
                b->local = declare(b->name, b->span, false, false);
                break;
            }
            case PatKind::Wrap: pat(static_cast<WrapPat*>(p)->inner); break;
            case PatKind::Variant: {
                auto* v = static_cast<VariantPat*>(p);
                if (!v->path.empty()) variant_path(v);
                for (Pat* a : v->args) pat(a);
                for (FieldPat& fp : v->fields) {
                    if (fp.pat) pat(fp.pat);
                    else fp.local = declare(fp.name, fp.name_span, false, false);
                }
                break;
            }
            case PatKind::Or: {
                or_depth_++;
                for (Pat* a : static_cast<OrPat*>(p)->alts) pat(a);
                or_depth_--;
                break;
            }
            case PatKind::Wild: case PatKind::Lit: case PatKind::None: case PatKind::Error: break;
        }
    }

    // `E.V` или `m.E.V` в образце.
    void variant_path(VariantPat* v) {
        const EnumDef* en = nullptr;
        const Symbol* head = r_.lookup(v->path[0]);
        if (v->path.size() == 1 && head && head->kind == SymKind::Enum) {
            en = head->en;
        } else if (v->path.size() == 2 && head && head->kind == SymKind::Module) {
            const Symbol* s = r_.member(*head->mod, v->path[1], v->span);
            if (!s) return;
            if (s->kind == SymKind::Enum) en = s->en;
        }
        if (!en) {
            std::string path;
            for (const std::string& part : v->path) path += (path.empty() ? "" : ".") + part;
            diags_.error(Code::E0307, v->span, "cannot find enum '" + path + "'");
            return;
        }
        int idx = en->find(v->name);
        if (idx < 0) {
            diags_.error(Code::E0312, v->name_span, "enum '" + en->name + "' has no variant '" + v->name + "'");
            return;
        }
        v->res.kind = ResKind::Variant;
        v->res.en = en;
        v->res.index = static_cast<uint32_t>(idx);
    }
};

}  // namespace

void check(Program& prog, Diagnostics& diags) {
    for (auto& m : prog.modules) BodyChecker(prog, *m, diags).run();
}

}  // namespace dp::sema
