// Глубина дерева модуля без рекурсии (parser.h, deeper_than).
//
// Парсер ограничивает свою рекурсию, но цепочки операторов и постфиксов
// (`1+1+…`, `x.f().g()…`) он строит циклом, а дерево у них глубокое.
// Проходы после разбора рекурсивны, поэтому глубину дерева проверяет
// этот обход с явным стеком.
#include <vector>

#include "syntax/parser.h"

namespace dp {

using namespace ast;

namespace {

enum class K : uint8_t { Expr, Stmt, Block, Pat, Type };

struct Entry {
    K k;
    const Node* n;
    uint32_t d;
};

class Walk {
public:
    const Node* run(const Module& m, uint32_t limit) {
        for (const Item* it : m.items) item(it);
        while (!st_.empty()) {
            Entry e = st_.back();
            st_.pop_back();
            if (e.d > limit) return e.n;
            uint32_t d = e.d + 1;
            switch (e.k) {
                case K::Expr: expr(static_cast<const Expr*>(e.n), d); break;
                case K::Stmt: stmt(static_cast<const Stmt*>(e.n), d); break;
                case K::Block:
                    for (const Stmt* s : static_cast<const Block*>(e.n)->stmts) push(K::Stmt, s, d);
                    break;
                case K::Pat: pat(static_cast<const Pat*>(e.n), d); break;
                case K::Type: type(static_cast<const Type*>(e.n), d); break;
            }
        }
        return nullptr;
    }

private:
    std::vector<Entry> st_;

    void push(K k, const Node* n, uint32_t d) {
        if (n) st_.push_back(Entry{k, n, d});
    }

    void item(const Item* it) {
        if (auto* f = as<const FnItem>(it)) {
            for (const Param& p : f->params) push(K::Type, p.type, 1);
            push(K::Type, f->ret, 1);
            push(K::Block, f->body, 1);
        } else if (auto* s = as<const StructItem>(it)) {
            for (const FieldDecl& fd : s->fields) {
                push(K::Type, fd.type, 1);
                push(K::Expr, fd.value, 1);
            }
        } else if (auto* e = as<const EnumItem>(it)) {
            for (const VariantDecl& v : e->variants) {
                for (const Type* t : v.types) push(K::Type, t, 1);
                for (const FieldDecl& fd : v.fields) {
                    push(K::Type, fd.type, 1);
                    push(K::Expr, fd.value, 1);
                }
            }
        } else if (auto* c = as<const ConstItem>(it)) {
            push(K::Type, c->type, 1);
            push(K::Expr, c->value, 1);
        }
    }

    void stmt(const Stmt* s, uint32_t d) {
        switch (s->kind) {
            case StmtKind::Let: {
                auto* l = static_cast<const LetStmt*>(s);
                push(K::Type, l->type, d);
                push(K::Expr, l->value, d);
                break;
            }
            case StmtKind::Assign: {
                auto* a = static_cast<const AssignStmt*>(s);
                push(K::Expr, a->target, d);
                push(K::Expr, a->value, d);
                break;
            }
            case StmtKind::Expr: push(K::Expr, static_cast<const ExprStmt*>(s)->expr, d); break;
            case StmtKind::If: {
                auto* i = static_cast<const IfStmt*>(s);
                push(K::Expr, i->cond, d);
                push(K::Expr, i->let_value, d);
                push(K::Block, i->then, d);
                push(K::Stmt, i->els, d);
                break;
            }
            case StmtKind::While: {
                auto* w = static_cast<const WhileStmt*>(s);
                push(K::Expr, w->cond, d);
                push(K::Block, w->body, d);
                break;
            }
            case StmtKind::For: {
                auto* f = static_cast<const ForStmt*>(s);
                push(K::Expr, f->iter, d);
                push(K::Expr, f->hi, d);
                push(K::Block, f->body, d);
                break;
            }
            case StmtKind::Match: {
                auto* m = static_cast<const MatchStmt*>(s);
                push(K::Expr, m->subject, d);
                for (const MatchArm& a : m->arms) {
                    push(K::Pat, a.pat, d);
                    push(K::Stmt, a.body, d);
                }
                break;
            }
            case StmtKind::Return: push(K::Expr, static_cast<const ReturnStmt*>(s)->value, d); break;
            case StmtKind::Defer: push(K::Stmt, static_cast<const DeferStmt*>(s)->body, d); break;
            case StmtKind::Fail: push(K::Expr, static_cast<const FailStmt*>(s)->value, d); break;
            case StmtKind::Block: push(K::Block, static_cast<const BlockStmt*>(s)->block, d); break;
            case StmtKind::Break: case StmtKind::Continue: case StmtKind::Error: break;
        }
    }

    void expr(const Expr* e, uint32_t d) {
        switch (e->kind) {
            case ExprKind::Field: push(K::Expr, static_cast<const FieldExpr*>(e)->object, d); break;
            case ExprKind::Index: {
                auto* x = static_cast<const IndexExpr*>(e);
                push(K::Expr, x->object, d);
                push(K::Expr, x->index, d);
                break;
            }
            case ExprKind::Slice: {
                auto* x = static_cast<const SliceExpr*>(e);
                push(K::Expr, x->object, d);
                push(K::Expr, x->lo, d);
                push(K::Expr, x->hi, d);
                break;
            }
            case ExprKind::Call: {
                auto* x = static_cast<const CallExpr*>(e);
                push(K::Expr, x->callee, d);
                for (const Expr* a : x->args) push(K::Expr, a, d);
                break;
            }
            case ExprKind::Unary: push(K::Expr, static_cast<const UnaryExpr*>(e)->operand, d); break;
            case ExprKind::Binary: {
                auto* x = static_cast<const BinaryExpr*>(e);
                push(K::Expr, x->lhs, d);
                push(K::Expr, x->rhs, d);
                break;
            }
            case ExprKind::Try: push(K::Expr, static_cast<const TryExpr*>(e)->operand, d); break;
            case ExprKind::Paren: push(K::Expr, static_cast<const ParenExpr*>(e)->inner, d); break;
            case ExprKind::Ref: push(K::Expr, static_cast<const RefExpr*>(e)->operand, d); break;
            case ExprKind::StructLit: {
                auto* x = static_cast<const StructLitExpr*>(e);
                push(K::Expr, x->path, d);
                for (const FieldInit& f : x->fields) push(K::Expr, f.value, d);
                break;
            }
            case ExprKind::VariantLit: {
                auto* x = static_cast<const VariantLitExpr*>(e);
                for (const Expr* a : x->args) push(K::Expr, a, d);
                for (const FieldInit& f : x->fields) push(K::Expr, f.value, d);
                break;
            }
            case ExprKind::ListLit:
                for (const Expr* a : static_cast<const ListLitExpr*>(e)->items) push(K::Expr, a, d);
                break;
            case ExprKind::Int: case ExprKind::Float: case ExprKind::Str: case ExprKind::Byte:
            case ExprKind::Bool: case ExprKind::None: case ExprKind::Name: case ExprKind::Error:
                break;
        }
    }

    void pat(const Pat* p, uint32_t d) {
        switch (p->kind) {
            case PatKind::Lit: push(K::Expr, static_cast<const LitPat*>(p)->lit, d); break;
            case PatKind::Wrap: push(K::Pat, static_cast<const WrapPat*>(p)->inner, d); break;
            case PatKind::Variant: {
                auto* v = static_cast<const VariantPat*>(p);
                for (const Pat* a : v->args) push(K::Pat, a, d);
                for (const FieldPat& f : v->fields) push(K::Pat, f.pat, d);
                break;
            }
            case PatKind::Or:
                for (const Pat* a : static_cast<const OrPat*>(p)->alts) push(K::Pat, a, d);
                break;
            case PatKind::Wild: case PatKind::Bind: case PatKind::None: case PatKind::Error: break;
        }
    }

    void type(const Type* t, uint32_t d) {
        switch (t->kind) {
            case TypeKind::Name:
                for (const Type* a : static_cast<const NameType*>(t)->args) push(K::Type, a, d);
                break;
            case TypeKind::Slice: push(K::Type, static_cast<const SliceType*>(t)->elem, d); break;
            case TypeKind::Optional: push(K::Type, static_cast<const OptionalType*>(t)->inner, d); break;
            case TypeKind::Result: {
                auto* r = static_cast<const ResultType*>(t);
                push(K::Type, r->error, d);
                push(K::Type, r->value, d);
                break;
            }
            case TypeKind::Array: push(K::Type, static_cast<const ArrayType*>(t)->elem, d); break;
            case TypeKind::Ref: push(K::Type, static_cast<const RefType*>(t)->inner, d); break;
            case TypeKind::Fn: {
                auto* f = static_cast<const FnType*>(t);
                for (const FnType::Param& p : f->params) push(K::Type, p.type, d);
                push(K::Type, f->ret, d);
                break;
            }
            case TypeKind::Error: break;
        }
    }
};

}  // namespace

const Node* deeper_than(const Module& m, uint32_t limit) { return Walk().run(m, limit); }

}  // namespace dp
