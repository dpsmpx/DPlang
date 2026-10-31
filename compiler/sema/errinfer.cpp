// Вывод типа ошибки непубличных функций `!T` (Р41; docs/spec/core.md,
// раздел 7). Источники ошибок функции — её `fail` и `try`; каждый даёт
// множество возможных перечислений, тип ошибки — их общее единственное.
// Функции, вызывающие друг друга через `try`, решаются по готовности:
// функция — после всех, кого она ждёт; цикл `try` не решается.
#include <map>
#include <set>

#include "sema/checker.h"

namespace dp::sema {

using namespace ast;

namespace {

using EnumSet = std::set<const EnumDef*>;

struct Source {
    EnumSet enums;              // возможные перечисления
    const FnDef* callee = nullptr;  // `try f()` у функции с выводимым типом — ждёт её
    bool unknown = false;       // по записи не определить (нужен явный тип)
    Span span;
};

class Collector {
public:
    Collector(Resolver& r) : r_(r) {}
    std::vector<Source> sources;

    void block(const Block* b) {
        if (!b) return;
        for (const Stmt* s : b->stmts) stmt(s);
    }

private:
    Resolver& r_;

    // Перечисления, видимые в модуле, с вариантом name.
    EnumSet enums_with(const std::string& name) {
        EnumSet out;
        for (auto& [n, s] : r_.module().scope) {
            if (s.kind == SymKind::Enum && s.en->find(name) >= 0) out.insert(s.en);
            if (s.kind == SymKind::Module) {
                for (auto& [mn, ms] : s.mod->scope) {
                    if (ms.kind == SymKind::Enum && ms.en->pub && ms.en->find(name) >= 0) out.insert(ms.en);
                }
            }
        }
        return out;
    }

    // Перечисление по пути `E` или `m.E`; nullptr — не перечисление.
    const EnumDef* enum_path(const Expr* e) {
        if (auto* n = as<const NameExpr>(e)) {
            const Symbol* s = r_.lookup(n->name);
            return s && s->kind == SymKind::Enum ? s->en : nullptr;
        }
        if (auto* f = as<const FieldExpr>(e)) {
            auto* m = as<const NameExpr>(f->object);
            if (!m) return nullptr;
            const Symbol* s = r_.lookup(m->name);
            if (!s || s->kind != SymKind::Module) return nullptr;
            auto it = s->mod->scope.find(f->name);
            return it != s->mod->scope.end() && it->second.kind == SymKind::Enum ? it->second.en : nullptr;
        }
        return nullptr;
    }

    const FnDef* fn_path(const Expr* e) {
        if (auto* n = as<const NameExpr>(e)) {
            const Symbol* s = r_.lookup(n->name);
            return s && s->kind == SymKind::Fn ? s->fn : nullptr;
        }
        if (auto* f = as<const FieldExpr>(e)) {
            auto* m = as<const NameExpr>(f->object);
            if (!m) return nullptr;
            const Symbol* s = r_.lookup(m->name);
            if (!s || s->kind != SymKind::Module) return nullptr;
            auto it = s->mod->scope.find(f->name);
            return it != s->mod->scope.end() && it->second.kind == SymKind::Fn ? it->second.fn : nullptr;
        }
        return nullptr;
    }

    void fail_value(const Expr* v, Span span) {
        Source s;
        s.span = span;
        while (auto* p = as<const ParenExpr>(v)) v = p->inner;
        if (auto* vl = as<const VariantLitExpr>(v)) {
            s.enums = enums_with(vl->name);
        } else if (auto* f = as<const FieldExpr>(v)) {
            if (const EnumDef* en = enum_path(f->object)) s.enums.insert(en);
            else s.unknown = true;
        } else if (auto* c = as<const CallExpr>(v)) {  // `fail E.V(x)`
            auto* f2 = as<const FieldExpr>(c->callee);
            const EnumDef* en = f2 ? enum_path(f2->object) : nullptr;
            if (en) s.enums.insert(en);
            else s.unknown = true;
        } else if (auto* sl = as<const StructLitExpr>(v)) {  // `fail E.V { ... }`
            auto* f2 = as<const FieldExpr>(sl->path);
            const EnumDef* en = f2 ? enum_path(f2->object) : nullptr;
            if (en) s.enums.insert(en);
            else s.unknown = true;
        } else {
            s.unknown = true;
        }
        sources.push_back(s);
    }

    void try_value(const TryExpr* t) {
        Source s;
        s.span = t->span;
        const Expr* op = t->operand;
        while (auto* p = as<const ParenExpr>(op)) op = p->inner;
        auto* c = as<const CallExpr>(op);
        const FnDef* f = c ? fn_path(c->callee) : nullptr;
        if (!f) {
            s.unknown = true;
        } else if (f->infer_error) {
            s.callee = f;
        } else if (f->ret && f->ret->kind == TK::Result && f->ret->err && f->ret->err->kind == TK::Enum) {
            s.enums.insert(f->ret->err->edef);
        } else {
            return;  // не результат — проверка типов сообщит сама
        }
        sources.push_back(s);
    }

    void stmt(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Let: expr(static_cast<const LetStmt*>(s)->value); break;
            case StmtKind::Assign:
                expr(static_cast<const AssignStmt*>(s)->target);
                expr(static_cast<const AssignStmt*>(s)->value);
                break;
            case StmtKind::Expr: expr(static_cast<const ExprStmt*>(s)->expr); break;
            case StmtKind::If: {
                auto* i = static_cast<const IfStmt*>(s);
                expr(i->cond);
                expr(i->let_value);
                block(i->then);
                stmt(i->els);
                break;
            }
            case StmtKind::While:
                expr(static_cast<const WhileStmt*>(s)->cond);
                block(static_cast<const WhileStmt*>(s)->body);
                break;
            case StmtKind::For: {
                auto* f = static_cast<const ForStmt*>(s);
                expr(f->iter);
                expr(f->hi);
                block(f->body);
                break;
            }
            case StmtKind::Match: {
                auto* m = static_cast<const MatchStmt*>(s);
                expr(m->subject);
                for (const MatchArm& a : m->arms) stmt(a.body);
                break;
            }
            case StmtKind::Return: expr(static_cast<const ReturnStmt*>(s)->value); break;
            case StmtKind::Defer: stmt(static_cast<const DeferStmt*>(s)->body); break;
            case StmtKind::Lock:
                expr(static_cast<const LockStmt*>(s)->mutex);
                block(static_cast<const LockStmt*>(s)->body);
                break;
            case StmtKind::Fail: {
                auto* f = static_cast<const FailStmt*>(s);
                expr(f->value);
                fail_value(f->value, f->span);
                break;
            }
            case StmtKind::Block: block(static_cast<const BlockStmt*>(s)->block); break;
            default: break;
        }
    }

    void expr(const Expr* e) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::Try: {
                auto* t = static_cast<const TryExpr*>(e);
                try_value(t);
                expr(t->operand);
                break;
            }
            case ExprKind::Field: expr(static_cast<const FieldExpr*>(e)->object); break;
            case ExprKind::Index:
                expr(static_cast<const IndexExpr*>(e)->object);
                expr(static_cast<const IndexExpr*>(e)->index);
                break;
            case ExprKind::Slice: {
                auto* s = static_cast<const SliceExpr*>(e);
                expr(s->object);
                expr(s->lo);
                expr(s->hi);
                break;
            }
            case ExprKind::Call: {
                auto* c = static_cast<const CallExpr*>(e);
                expr(c->callee);
                for (const Expr* a : c->args) expr(a);
                break;
            }
            case ExprKind::Unary: expr(static_cast<const UnaryExpr*>(e)->operand); break;
            case ExprKind::Binary:
                expr(static_cast<const BinaryExpr*>(e)->lhs);
                expr(static_cast<const BinaryExpr*>(e)->rhs);
                break;
            case ExprKind::Paren: expr(static_cast<const ParenExpr*>(e)->inner); break;
            case ExprKind::Ref: expr(static_cast<const RefExpr*>(e)->operand); break;
            case ExprKind::StructLit:
                for (const FieldInit& f : static_cast<const StructLitExpr*>(e)->fields) expr(f.value);
                break;
            case ExprKind::VariantLit: {
                auto* v = static_cast<const VariantLitExpr*>(e);
                for (const Expr* a : v->args) expr(a);
                for (const FieldInit& f : v->fields) expr(f.value);
                break;
            }
            case ExprKind::ListLit:
                for (const Expr* i : static_cast<const ListLitExpr*>(e)->items) expr(i);
                break;
            default: break;
        }
    }
};

}  // namespace

void infer_errors(Program& prog, ModuleInfo& m, Diagnostics& diags) {
    Resolver r(prog, m, diags);
    std::vector<FnDef*> fns;
    std::map<FnDef*, std::vector<Source>> src;
    for (Item* it : m.ast->items) {
        const Symbol* s = r.lookup(it->name);
        if (!s || s->kind != SymKind::Fn || s->fn->node != it || !s->fn->infer_error) continue;
        if (!s->fn->ret || s->fn->ret->kind != TK::Result) continue;
        Collector c(r);
        c.block(s->fn->node->body);
        fns.push_back(s->fn);
        src[s->fn] = std::move(c.sources);
    }
    // Функция решается, когда известны все функции её `try` (ждёт их);
    // решённая будит ждущих её. Каждая функция считается один раз, когда
    // ждать больше нечего: время линейно по числу источников.
    std::map<const FnDef*, const EnumDef*> known;
    std::map<const FnDef*, std::vector<FnDef*>> users;  // функция → ждущие её
    std::map<const FnDef*, size_t> pending;              // сколько разных функций ещё ждёт
    std::vector<FnDef*> ready;
    for (FnDef* f : fns) {
        std::set<const FnDef*> callees;
        for (const Source& s : src[f]) {
            if (s.callee && !s.unknown) callees.insert(s.callee);
        }
        for (const FnDef* c : callees) users[c].push_back(f);
        pending[f] = callees.size();
        if (callees.empty()) ready.push_back(f);
    }
    for (size_t next = 0; next < ready.size(); next++) {
        FnDef* f = ready[next];
        EnumSet acc;
        bool first = true, unknown = false;
        for (const Source& s : src[f]) {
            EnumSet cur = s.enums;
            if (s.unknown) {
                unknown = true;
                continue;
            }
            if (s.callee) cur = {known.at(s.callee)};
            if (first) {
                acc = cur;
                first = false;
            } else {
                EnumSet both;
                for (const EnumDef* e : acc) {
                    if (cur.count(e)) both.insert(e);
                }
                acc = both;
            }
        }
        if (unknown || first || acc.size() != 1) continue;
        known[f] = *acc.begin();
        f->ret = prog.types.result(prog.types.enum_type(*acc.begin()), f->ret->elem);
        for (FnDef* u : users[f]) {
            if (--pending[u] == 0) ready.push_back(u);
        }
    }
    for (FnDef* f : fns) {
        if (known.count(f)) continue;
        Span at = f->node->ret ? f->node->ret->span : f->span;
        const std::vector<Source>& ss = src[f];
        std::string why;
        if (ss.empty()) {
            why = "it has no 'fail' or 'try'; remove '!' if it cannot fail";
        } else {
            bool unknown = false;
            for (const Source& s : ss) unknown = unknown || s.unknown;
            why = unknown ? "a 'fail' or 'try' here needs the type written; write 'MyError!T'"
                          : "its 'fail' and 'try' give different or ambiguous error enums; write 'MyError!T'";
        }
        diags.error(Code::E0507, at, "cannot infer the error type of '" + f->name + "': " + why);
    }
}

}  // namespace dp::sema
