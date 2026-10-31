// Тело `parallel for` без `os` и C по графу вызовов (Р69; docs/spec/core.md,
// stmt.for). Отдельный проход после проверки всех модулей: функция «трогает
// внешний мир», если сама зовёт функцию C, указатель на функцию или чтение
// файлов и аргументов программы — или зовёт такую функцию. Вывод (`print`,
// `println`, `eprintln`) разрешён: runtime выдаёт его по номеру итерации.
#include <map>
#include <string>
#include <vector>

#include "sema/builtins.h"
#include "sema/checker.h"

namespace dp::sema {

using namespace ast;

namespace {

struct Effect {
    bool has = false;
    std::string what;            // своё действие: «calls C function 'fopen'»
    const FnDef* via = nullptr;  // или вызов функции с действием
};

// Что вызов делает сам: непустая строка — действие, запрещённое в теле.
std::string direct(const CallExpr* c) {
    switch (c->call) {
        case CallKind::Fn:
            if (c->fn && c->fn->is_extern) return "calls C function '" + c->fn->name + "'";
            return "";
        case CallKind::Ptr: return "calls C through a function pointer";
        case CallKind::Builtin: {
            auto b = static_cast<BuiltinFn>(c->builtin);
            if (b == BuiltinFn::ReadFile) return "reads files";
            if (b == BuiltinFn::ArgsLen || b == BuiltinFn::ArgAt) return "reads program arguments";
            return "";
        }
        default: return "";
    }
}

// Обход тела: каждый вызов — в visit.
template <class Visit>
struct Walk {
    Visit& visit;

    void block(const Block* b) {
        if (!b) return;
        for (const Stmt* s : b->stmts) stmt(s);
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
                visit.loop(f, *this);
                break;
            }
            case StmtKind::Match: {
                auto* m = static_cast<const MatchStmt*>(s);
                expr(m->subject);
                for (const MatchArm& a : m->arms) stmt(a.body);
                break;
            }
            case StmtKind::Return: expr(static_cast<const ReturnStmt*>(s)->value); break;
            case StmtKind::Fail: expr(static_cast<const FailStmt*>(s)->value); break;
            case StmtKind::Defer: stmt(static_cast<const DeferStmt*>(s)->body); break;
            case StmtKind::Block: block(static_cast<const BlockStmt*>(s)->block); break;
            case StmtKind::Lock:
                expr(static_cast<const LockStmt*>(s)->mutex);
                block(static_cast<const LockStmt*>(s)->body);
                break;
            case StmtKind::Tasks:
                visit.tasks(s->span);
                block(static_cast<const TasksStmt*>(s)->body);
                break;
            default: break;
        }
    }

    void expr(const Expr* e) {
        if (!e) return;
        switch (e->kind) {
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
                visit.call(c);
                expr(c->callee);
                for (const Expr* a : c->args) expr(a);
                break;
            }
            case ExprKind::Unary: expr(static_cast<const UnaryExpr*>(e)->operand); break;
            case ExprKind::Binary:
                expr(static_cast<const BinaryExpr*>(e)->lhs);
                expr(static_cast<const BinaryExpr*>(e)->rhs);
                break;
            case ExprKind::Try: expr(static_cast<const TryExpr*>(e)->operand); break;
            case ExprKind::Spawn:
                visit.tasks(e->span);
                expr(static_cast<const SpawnExpr*>(e)->operand);
                break;
            case ExprKind::Paren: expr(static_cast<const ParenExpr*>(e)->inner); break;
            case ExprKind::Ref: expr(static_cast<const RefExpr*>(e)->operand); break;
            case ExprKind::StructLit:
                for (const FieldInit& f : static_cast<const StructLitExpr*>(e)->fields) {
                    expr(f.value ? f.value : f.shorthand);
                }
                break;
            case ExprKind::VariantLit: {
                auto* v = static_cast<const VariantLitExpr*>(e);
                for (const Expr* a : v->args) expr(a);
                for (const FieldInit& f : v->fields) expr(f.value ? f.value : f.shorthand);
                break;
            }
            case ExprKind::ListLit:
                for (const Expr* a : static_cast<const ListLitExpr*>(e)->items) expr(a);
                break;
            default: break;
        }
    }
};

// Своё действие и вызовы каждой функции.
struct Scan {
    Effect effect;
    std::vector<const FnDef*> calls;

    void call(const CallExpr* c) {
        std::string w = direct(c);
        if (!w.empty() && !effect.has) {
            effect.has = true;
            effect.what = w;
        }
        if (c->call == CallKind::Fn && c->fn && !c->fn->is_extern) calls.push_back(c->fn);
    }
    void tasks(Span) {
        if (effect.has) return;
        effect.has = true;
        effect.what = "starts tasks";
    }
    template <class W>
    void loop(const ForStmt* f, W& w) {
        w.block(f->body);
    }
};

class Parallel {
public:
    Parallel(Program& prog, Diagnostics& diags) : prog_(prog), diags_(diags) {}

    void run() {
        // Своё действие и граф вызовов, затем распространение по вызовам до
        // неподвижной точки.
        std::map<const FnDef*, std::vector<const FnDef*>> calls;
        for (const FnDef& f : prog_.fns) {
            if (f.is_extern || !f.node || !f.node->body) continue;
            Scan s;
            Walk<Scan>{s}.block(f.node->body);
            eff_[&f] = s.effect;
            calls[&f] = std::move(s.calls);
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& [f, callees] : calls) {
                Effect& e = eff_[f];
                if (e.has) continue;
                for (const FnDef* g : callees) {
                    auto it = eff_.find(g);
                    if (it == eff_.end() || !it->second.has) continue;
                    e.has = true;
                    e.via = g;
                    changed = true;
                    break;
                }
            }
        }
        for (const FnDef& f : prog_.fns) {
            if (f.is_extern || !f.node || !f.node->body) continue;
            fn_ = &f;
            Walk<Parallel>{*this}.block(f.node->body);
        }
    }

    // Обход: вызовы в теле `parallel for` (и во вложенных в него циклах).
    void call(const CallExpr* c) {
        if (depth_ == 0) return;
        std::string w = direct(c);
        if (!w.empty()) {
            if (c->call == CallKind::Fn) w = "'" + c->fn->name + "' is a C function";
            else if (c->call == CallKind::Ptr) w = "this call goes to C through a function pointer";
            else w = "'" + std::string(kBuiltinFnNames[c->builtin]) + "' " + w;
            report(c->span, w);
            return;
        }
        if (c->call != CallKind::Fn || !c->fn) return;
        auto it = eff_.find(c->fn);
        if (it == eff_.end() || !it->second.has) return;
        report(c->span, chain(c->fn));
    }

    // Задачи в теле (Р70): поток итерации их не запускает.
    void tasks(Span span) {
        if (depth_ > 0) report(span, "this starts tasks");
    }

    template <class W>
    void loop(const ForStmt* f, W& w) {
        if (f->parallel) depth_++;
        w.block(f->body);
        if (f->parallel) depth_--;
    }

private:
    Program& prog_;
    Diagnostics& diags_;
    std::map<const FnDef*, Effect> eff_;
    const FnDef* fn_ = nullptr;
    int depth_ = 0;

    std::string name(const FnDef* f) const {
        if (fn_ && f->module != fn_->module) return f->module->name + "." + f->name;
        return f->name;
    }

    // «'step' calls 'load', which calls C function 'fopen'».
    std::string chain(const FnDef* f) const {
        std::string out = "'" + name(f) + "'";
        for (int hops = 0; f && hops < 8; hops++) {
            auto it = eff_.find(f);
            if (it == eff_.end()) break;
            const Effect& e = it->second;
            if (!e.via) return out + (hops == 0 ? " " : ", which ") + e.what;
            out += (hops == 0 ? " calls '" : ", which calls '") + name(e.via) + "'";
            f = e.via;
        }
        return out + ", which reads files or calls C";
    }

    void report(Span span, const std::string& why) {
        diags_.error(Code::E0517, span,
                     why + ": 'parallel for' cannot use files, program arguments, C or tasks, also in the functions "
                           "it calls; read the input before the loop and call C after it");
    }
};

}  // namespace

void check_parallel(Program& prog, Diagnostics& diags) { Parallel(prog, diags).run(); }

}  // namespace dp::sema
