// Проверка линейных значений (Р57, Р60; docs/spec/core.md, ffi.linear).
//
// Отдельный проход по уже проверенному дереву функции: у выражений есть
// типы, у имён — переменные. Состояние каждой линейной переменной ведётся
// по путям исполнения, как определённая инициализация: на развилках
// ветви сводятся, на выходах (конец блока, return, fail, ошибка try,
// break, continue) проверяется, что ресурс закрыт. Во время выполнения
// линейное значение — обычное значение: этот проход ничего не меняет в
// дереве.
#include <string>
#include <vector>

#include "sema/checker.h"

namespace dp::sema {

using namespace ast;

namespace {

// Состояние линейной переменной.
enum class St : uint8_t {
    None,         // значения ещё нет (или переменная не линейна)
    Owned,        // ресурс у переменной: его нужно закрыть
    Consumed,     // закрыт или перенесён
    Maybe,        // на одних путях закрыт, на других нет
    Deferred,     // закроет defer на выходе из блока
    ErrDeferred,  // закроет errdefer, если функция вернёт ошибку
    ErrMoved,     // закрыт или перенесён, но errdefer ещё действует: выход с ошибкой закрыл бы его снова
};

// Как выражение использует своё значение.
enum class Use : uint8_t {
    Consume,  // значение забирается: передача по значению, return, перенос
    Borrow,   // место читается или передаётся по ссылке
    Read,     // значение не линейного типа
    Discard,  // инструкция-выражение: значение отбрасывается
};

enum class Exit : uint8_t { Normal, Error, Both };

struct Flow {
    std::vector<St> st;
    bool live = true;
};

struct Scope {
    std::vector<const Local*> locals;
    std::vector<std::pair<const Local*, bool>> defers;  // переменная, errdefer
};

class Linear {
public:
    Linear(ModuleInfo& m, Diagnostics& diags) : m_(m), diags_(diags) {}

    void fn(FnDef& f) {
        fn_ = &f;
        flow_ = Flow{};
        flow_.st.assign(f.local_count, St::None);
        decl_loop_.assign(f.local_count, 0);
        reported_.assign(f.local_count, 0);
        scopes_.clear();
        loops_.clear();
        scopes_.emplace_back();
        FnItem* node = const_cast<FnItem*>(f.node);
        for (Param& p : node->params) {
            if (p.local) declare(p.local, true);
        }
        block_body(node->body);
        if (flow_.live) {
            Span end{m_.file, node->body ? node->body->span.end - 1 : node->span.end,
                     node->body ? node->body->span.end : node->span.end};
            exit_check(Exit::Normal, end);
        }
        scopes_.clear();
        fn_ = nullptr;
    }

private:
    ModuleInfo& m_;
    Diagnostics& diags_;
    FnDef* fn_ = nullptr;
    Flow flow_;
    std::vector<int> decl_loop_;     // по номеру переменной: глубина циклов при объявлении
    std::vector<char> reported_;     // по номеру: о переменной уже сообщили (без лавины)
    std::vector<Scope> scopes_;
    std::vector<size_t> loops_;      // число областей снаружи тела каждого цикла
    int defer_ = 0;                  // 1 — тело defer, 2 — тело errdefer
    // Нельзя поглотить до конца вызова или присваивания: под ссылкой
    // аргумента (args), получатель изменяющего метода, место присваивания.
    struct Held {
        const Local* l;
        bool assign;
    };
    std::vector<Held> held_;

    void error(Code code, Span span, const std::string& msg) { diags_.error(code, span, msg); }

    bool tracked(const Local* l) const {
        return l && !l->is_ref && l->id < flow_.st.size() && linear_type(l->type);
    }

    void declare(const Local* l, bool owned) {
        if (!tracked(l)) return;
        scopes_.back().locals.push_back(l);
        decl_loop_[l->id] = static_cast<int>(loops_.size());
        flow_.st[l->id] = owned ? St::Owned : St::None;
    }

    static Flow merge(const Flow& a, const Flow& b) {
        if (!a.live) return b;
        if (!b.live) return a;
        Flow out = a;
        for (size_t i = 0; i < out.st.size() && i < b.st.size(); i++) {
            if (out.st[i] != b.st[i]) out.st[i] = St::Maybe;
        }
        return out;
    }

    // Ресурс не закрыт там, где должен быть: сообщение — один раз на переменную.
    void not_closed(const Local* l, Span span, const std::string& where) {
        if (reported_[l->id]) return;
        reported_[l->id] = 1;
        error(Code::E0511, span, "'" + l->name + "' is not closed " + where);
    }

    // --- области

    // Конец области на обычном пути: defer закрывают, errdefer отменяются,
    // объявленные в ней ресурсы должны быть закрыты.
    void close_scope(const Scope& s, Span span) {
        for (auto it = s.defers.rbegin(); it != s.defers.rend(); ++it) {
            St& st = flow_.st[it->first->id];
            if (!it->second && st == St::Deferred) st = St::Consumed;
            if (it->second && st == St::ErrDeferred) st = St::Owned;
            if (it->second && st == St::ErrMoved) st = St::Consumed;
        }
        for (const Local* l : s.locals) {
            St st = flow_.st[l->id];
            if (st == St::Owned || st == St::Maybe || st == St::ErrDeferred) {
                not_closed(l, span, st == St::Maybe ? "on every path at the end of its block"
                                                    : "at the end of its block; close it or use 'defer'");
            }
        }
    }

    void block_body(Block* b) {
        if (!b) return;
        for (Stmt* s : b->stmts) stmt(s);
    }

    void block(Block* b) {
        if (!b) return;
        scopes_.emplace_back();
        block_body(b);
        if (flow_.live) close_scope(scopes_.back(), Span{b->span.file, b->span.end > 0 ? b->span.end - 1 : 0, b->span.end});
        scopes_.pop_back();
    }

    // Выход из функции: все ресурсы закрыты (defer — да; errdefer — только на
    // ошибке и только если ресурс ещё у переменной).
    void exit_check(Exit kind, Span span) {
        for (const Scope& s : scopes_) {
            for (const Local* l : s.locals) {
                St st = flow_.st[l->id];
                if (st == St::ErrMoved && kind != Exit::Normal) {
                    if (reported_[l->id]) continue;
                    reported_[l->id] = 1;
                    error(Code::E0512, span, "'" + l->name + "' is already closed or moved, but 'errdefer' would "
                                             "close it again if the function returns an error here");
                    continue;
                }
                bool bad = st == St::Owned || st == St::Maybe || (kind != Exit::Error && st == St::ErrDeferred);
                if (!bad) continue;
                if (kind == Exit::Error) {
                    not_closed(l, span, "when the function returns an error here; add 'errdefer' to close it");
                } else if (st == St::ErrDeferred) {
                    not_closed(l, span, "when the function returns here: 'errdefer' closes it only on errors");
                } else {
                    not_closed(l, span, st == St::Maybe ? "on every path when the function returns here"
                                                        : "when the function returns here");
                }
            }
        }
    }

    // break и continue: выход из областей тела цикла.
    void loop_exit(Span span) {
        if (loops_.empty()) return;
        for (size_t d = scopes_.size(); d > loops_.back(); d--) close_scope(scopes_[d - 1], span);
    }

    // --- переменные

    void consume(const Local* l, Span span) {
        for (const Held& h : held_) {
            if (h.l != l) continue;
            // дальше — как поглощение: без лавины E0511
            error(Code::E0513, span, h.assign ? "'" + l->name + "' is assigned into here and cannot be closed or moved "
                                                "in the assigned value"
                                              : "'" + l->name + "' is held by a reference in this call and cannot be "
                                                "closed or moved before the call returns");
            break;
        }
        St& st = flow_.st[l->id];
        if (defer_) {  // тело defer и errdefer исполнится позже: ресурс за ним
            if (st != St::Owned) {
                used_after(l, span, st);
                return;
            }
            if (decl_loop_[l->id] < static_cast<int>(loops_.size())) loop_close(l, span);
            st = defer_ == 2 ? St::ErrDeferred : St::Deferred;
            scopes_.back().defers.push_back({l, defer_ == 2});
            return;
        }
        switch (st) {
            case St::Owned:
                if (decl_loop_[l->id] < static_cast<int>(loops_.size())) loop_close(l, span);
                st = St::Consumed;
                return;
            case St::ErrDeferred:
                if (decl_loop_[l->id] < static_cast<int>(loops_.size())) loop_close(l, span);
                st = St::ErrMoved;  // выход с ошибкой дальше — E0512 (exit_check)
                return;
            case St::None: st = St::Consumed; return;  // чтение без значения — уже E0503
            default: used_after(l, span, st); return;
        }
    }

    void loop_close(const Local* l, Span span) {
        reported_[l->id] = 1;  // состояние после цикла — как до него: без второго сообщения
        error(Code::E0513, span, "'" + l->name + "' was opened outside this loop and cannot be closed or moved "
                                                 "inside it; close it after the loop or with 'defer'");
    }

    void used_after(const Local* l, Span span, St st) {
        if (reported_[l->id]) return;
        reported_[l->id] = 1;
        const char* why = st == St::Deferred ? "' is closed by 'defer' at the end of its block; use it by reference"
                          : st == St::Maybe  ? "' may already be closed or moved on another path"
                                             : "' is already closed or moved";
        error(Code::E0512, span, "'" + l->name + why);
    }

    void borrow(const Local* l, Span span) {
        St st = flow_.st[l->id];
        if (st == St::Consumed || st == St::Maybe || st == St::ErrMoved) used_after(l, span, st);
    }

    // Аргумент держит заимствование до конца вызова: ссылка `&x` или
    // значение со ссылкой внутри — `?&T`, заимствующая структура `@c` (Р62),
    // их список или срез.
    static bool keeps_borrow(bool by_ref, const Type* t) {
        if (by_ref) return true;
        if (!t) return false;
        if (t->kind == TK::Optional) t = t->elem;
        if ((t->kind == TK::List || t->kind == TK::Slice) && t->elem) return keeps_borrow(false, t->elem);
        return t->kind == TK::Ref || c_borrowing(t);
    }

    // Корень места: `r` у `r.f`, `r[i]`, `r[a..b]`. Индекс и границы — только
    // чтение, их переменные ссылку не получают.
    static const Local* place_root(const Expr* e) {
        for (;;) {
            if (auto* p = as<const ParenExpr>(e)) e = p->inner;
            else if (auto* f = as<const FieldExpr>(e)) {
                if (f->field < 0) return nullptr;
                e = f->object;
            } else if (auto* i = as<const IndexExpr>(e)) e = i->object;
            else if (auto* s = as<const SliceExpr>(e)) e = s->object;
            else if (auto* n = as<const NameExpr>(e)) return n->res.kind == ResKind::Local ? n->res.local : nullptr;
            else return nullptr;
        }
    }

    // Переменные, на которые держит ссылку аргумент: корень места под `&`,
    // в том числе в полях и элементах литерала и в `some(...)`.
    void ref_roots(const Expr* e, bool by_ref) {
        if (!e) return;
        if (by_ref) {  // аргумент параметра `&T`, `&mut T` — место
            if (auto* r = as<const RefExpr>(e)) e = r->operand;
            if (const Local* l = place_root(e)) held_.push_back(Held{l, false});
            else ref_roots(e, false);  // `&View { p: &r }` — ссылки в литерале
            return;
        }
        if (auto* p = as<const ParenExpr>(e)) return ref_roots(p->inner, false);
        if (auto* r = as<const RefExpr>(e)) return ref_roots(r->operand, true);
        // Часть литерала — `[View { p: &r }][0..1]`, `W { v: View { ... } }.v`.
        if (auto* f = as<const FieldExpr>(e)) return ref_roots(f->object, false);
        if (auto* i = as<const IndexExpr>(e)) return ref_roots(i->object, false);
        if (auto* x = as<const SliceExpr>(e)) return ref_roots(x->object, false);
        if (auto* s = as<const StructLitExpr>(e)) {
            for (const FieldInit& fi : s->fields) ref_roots(fi.value ? fi.value : fi.shorthand, false);
        } else if (auto* l = as<const ListLitExpr>(e)) {
            for (const Expr* a : l->items) ref_roots(a, false);
        } else if (auto* v = as<const VariantLitExpr>(e)) {
            for (const Expr* a : v->args) ref_roots(a, false);
            for (const FieldInit& fi : v->fields) ref_roots(fi.value ? fi.value : fi.shorthand, false);
        } else if (auto* c = as<const CallExpr>(e)) {
            if (c->call == CallKind::Wrap || c->call == CallKind::Variant) {
                for (const Expr* a : c->args) ref_roots(a, false);
            }
        }
    }

    // Аргументы вызова по порядку: переменные под ссылкой аргумента, который
    // держит её до конца вызова, попадают в held_ до его вычисления — закрыть
    // такой ресурс в нём же или правее нельзя (E0513), иначе вызов прочитал
    // бы закрытое.
    void args(CallExpr* c, const std::vector<bool>& by_ref) {
        size_t held = held_.size();
        for (size_t i = 0; i < c->args.size(); i++) {
            bool r = i < by_ref.size() && by_ref[i];
            if (keeps_borrow(r, c->args[i]->type)) ref_roots(c->args[i], r);
            expr(c->args[i], r ? Use::Borrow : Use::Consume);
        }
        held_.resize(held);  // ссылки вложенного вызова кончились с ним
    }

    // --- инструкции

    void stmt(Stmt* s) {
        if (!s || !flow_.live) return;
        switch (s->kind) {
            case StmtKind::Let: let_stmt(static_cast<LetStmt*>(s)); break;
            case StmtKind::Assign: assign_stmt(static_cast<AssignStmt*>(s)); break;
            case StmtKind::Expr: expr(static_cast<ExprStmt*>(s)->expr, Use::Discard); break;
            case StmtKind::If: if_stmt(static_cast<IfStmt*>(s)); break;
            case StmtKind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                expr(w->cond, Use::Read);
                auto* lit = as<BoolExpr>(w->cond);
                loop_body(w->body, lit && lit->value);
                break;
            }
            case StmtKind::For: {
                auto* f = static_cast<ForStmt*>(s);
                expr(f->iter, f->mode == ForMode::Range ? Use::Read : Use::Borrow);
                expr(f->hi, Use::Read);
                loop_body(f->body, false);
                break;
            }
            case StmtKind::Match: match_stmt(static_cast<MatchStmt*>(s)); break;
            case StmtKind::Return: {
                auto* r = static_cast<ReturnStmt*>(s);
                expr(r->value, Use::Consume);
                // `return` значения E!T (без обёртки ok) может вернуть и ошибку.
                bool both = r->value && r->value->type && r->value->type->kind == TK::Result && !r->value->conv;
                exit_check(both ? Exit::Both : Exit::Normal, s->span);
                flow_.live = false;
                break;
            }
            case StmtKind::Fail:
                expr(static_cast<FailStmt*>(s)->value, Use::Read);
                exit_check(Exit::Error, s->span);
                flow_.live = false;
                break;
            case StmtKind::Break: case StmtKind::Continue:
                loop_exit(s->span);
                flow_.live = false;
                break;
            case StmtKind::Defer: {
                auto* d = static_cast<DeferStmt*>(s);
                int saved = defer_;
                defer_ = d->on_error ? 2 : 1;
                stmt(d->body);
                defer_ = saved;
                break;
            }
            case StmtKind::Block: block(static_cast<BlockStmt*>(s)->block); break;
            case StmtKind::Error: break;
        }
    }

    void let_stmt(LetStmt* l) {
        if (l->destructure) {
            expr(l->value, Use::Consume);
            for (LetField& f : l->fields) {
                if (f.index < 0 || !l->sdef) continue;
                const Type* t = l->sdef->fields[static_cast<size_t>(f.index)].type;
                if (f.local) declare(f.local, true);
                else if (linear_type(t)) {
                    error(Code::E0511, f.name_span, "field '" + f.field + "' holds a resource; bind it to a name and "
                                                    "close it");
                }
            }
            return;
        }
        expr(l->value, Use::Consume);
        if (l->local) {
            declare(l->local, l->value != nullptr);
        } else if (l->value && l->value->type && linear_type(l->value->type)) {
            error(Code::E0511, l->value->span, "the resource is dropped without being closed");
        }
    }

    void assign_stmt(AssignStmt* a) {
        auto* n = as<NameExpr>(a->target);
        if (n && n->name == "_") {
            expr(a->value, Use::Consume);
            if (a->value->type && linear_type(a->value->type)) {
                error(Code::E0511, a->value->span, "'_ =' drops the resource without closing it");
            }
            return;
        }
        if (n && n->res.kind == ResKind::Local && tracked(n->res.local)) {
            const Local* l = n->res.local;
            expr(a->value, Use::Consume);
            St& st = flow_.st[l->id];
            if (decl_loop_[l->id] < static_cast<int>(loops_.size())) {
                loop_close(l, n->span);
            } else if (st == St::Owned || st == St::Maybe || st == St::Deferred || st == St::ErrDeferred ||
                       st == St::ErrMoved) {
                error(Code::E0513, n->span, "assigning over '" + l->name + "', whose resource is not closed yet");
            }
            st = St::Owned;
            return;
        }
        expr(a->target, Use::Borrow);
        if (a->target->type && linear_type(a->target->type)) {
            error(Code::E0513, a->target->span,
                  n ? "'" + n->name + "' is a reference: the caller's resource cannot be replaced"
                    : std::string("a '@linear' field cannot be replaced; destructure the struct with "
                                  "'let S { ... } = v'"));
        }
        // Место вычислено до значения (expr.order): поглотить его корень в
        // значении нельзя — запись ушла бы в закрытое.
        size_t held = held_.size();
        if (const Local* l = place_root(a->target)) held_.push_back(Held{l, true});
        expr(a->value, Use::Consume);
        held_.resize(held);
    }

    void if_stmt(IfStmt* s) {
        expr(s->cond, Use::Read);
        expr(s->let_value, Use::Consume);
        Flow before = flow_;
        scopes_.emplace_back();
        if (s->let_local) declare(s->let_local, true);
        block_body(s->then);
        if (flow_.live) close_scope(scopes_.back(), s->then ? end_of(s->then->span) : s->span);
        scopes_.pop_back();
        Flow after_then = flow_;
        flow_ = before;
        stmt(s->els);
        flow_ = merge(after_then, flow_);
    }

    static Span end_of(Span s) { return Span{s.file, s.end > 0 ? s.end - 1 : 0, s.end}; }

    void loop_body(Block* body, bool forever) {
        Flow before = flow_;
        loops_.push_back(scopes_.size());
        block(body);
        loops_.pop_back();
        // Внешние ресурсы тело не трогает (E0513): после цикла — состояние до него.
        flow_ = before;
        if (forever) flow_.live = false;  // `while true` выходит только через break: его состояние — то же
        if (forever) flow_.live = has_break(body);
    }

    static bool has_break(const Block* b);

    void match_stmt(MatchStmt* s) {
        const Type* t = s->subject ? s->subject->type : nullptr;
        bool lin = t && linear_type(t);
        expr(s->subject, lin ? Use::Consume : Use::Read);
        Flow before = flow_;
        Flow after;
        after.live = false;
        for (MatchArm& a : s->arms) {
            flow_ = before;
            scopes_.emplace_back();
            if (lin) arm_pattern(a.pat, t);
            bind(a.pat);
            stmt(a.body);
            if (flow_.live) close_scope(scopes_.back(), a.body ? end_of(a.body->span) : a.span);
            scopes_.pop_back();
            after = merge(after, flow_);
        }
        flow_ = s->arms.empty() ? before : after;
    }

    // Образец у линейного значения не отбрасывает ресурс: `_` и `some(_)` —
    // там, где он может лежать (E0511).
    void arm_pattern(Pat* p, const Type* t) {
        if (!p) return;
        switch (p->kind) {
            case PatKind::Wild:
                error(Code::E0511, p->span, "'_' drops the resource in the matched value; bind it to a name");
                break;
            case PatKind::Wrap: {
                auto* w = static_cast<WrapPat*>(p);
                const Type* inner = w->which == WrapKind::Err ? t->err : t->elem;
                if (inner && linear_type(inner)) arm_pattern(w->inner, inner);
                break;
            }
            case PatKind::Or:
                for (Pat* x : static_cast<OrPat*>(p)->alts) arm_pattern(x, t);
                break;
            default: break;
        }
    }

    void bind(Pat* p) {
        if (!p) return;
        switch (p->kind) {
            case PatKind::Bind: declare(static_cast<BindPat*>(p)->local, true); break;
            case PatKind::Wrap: bind(static_cast<WrapPat*>(p)->inner); break;
            case PatKind::Variant: {
                auto* v = static_cast<VariantPat*>(p);
                for (Pat* x : v->args) bind(x);
                for (FieldPat& f : v->fields) {
                    if (f.pat) bind(f.pat);
                    else declare(f.local, true);
                }
                break;
            }
            default: break;
        }
    }

    // --- выражения

    static bool is_place(const Expr* e) {
        while (auto* p = as<const ParenExpr>(e)) e = p->inner;
        if (auto* n = as<const NameExpr>(e)) return n->res.kind == ResKind::Local;
        if (auto* f = as<const FieldExpr>(e)) return f->field >= 0 && is_place(f->object);
        if (auto* i = as<const IndexExpr>(e)) return is_place(i->object);
        return false;
    }

    void expr(Expr* e, Use use) {
        if (!e || !flow_.live) return;
        bool lin = e->type && linear_type(e->type);
        switch (e->kind) {
            case ExprKind::Name: {
                auto* n = static_cast<NameExpr*>(e);
                if (n->res.kind != ResKind::Local) return;
                const Local* l = n->res.local;
                if (lin && use == Use::Consume && l && l->is_ref) {  // копия из-под ссылки закрыла бы чужой ресурс
                    error(Code::E0513, n->span, "'" + l->name + "' is a reference: its resource belongs to the "
                                                "caller and cannot be closed or moved here");
                    return;
                }
                if (!tracked(l)) return;
                if (use == Use::Consume) consume(n->res.local, n->span);
                else borrow(n->res.local, n->span);
                return;
            }
            case ExprKind::Paren: expr(static_cast<ParenExpr*>(e)->inner, use); return;
            case ExprKind::Field: {
                auto* f = static_cast<FieldExpr*>(e);
                if (f->field < 0) return;  // член модуля, вариант
                if (lin && use == Use::Consume) {
                    error(Code::E0513, f->name_span, "the '@linear' field '" + f->name + "' cannot be moved out alone; "
                                                     "destructure the struct with 'let S { ... } = v'");
                }
                expr(f->object, Use::Borrow);
                return;
            }
            case ExprKind::Index: {
                auto* x = static_cast<IndexExpr*>(e);
                expr(x->object, Use::Borrow);
                expr(x->index, Use::Read);
                return;
            }
            case ExprKind::Slice: {
                auto* x = static_cast<SliceExpr*>(e);
                expr(x->object, Use::Borrow);
                expr(x->lo, Use::Read);
                expr(x->hi, Use::Read);
                return;
            }
            case ExprKind::Ref: expr(static_cast<RefExpr*>(e)->operand, Use::Borrow); return;
            case ExprKind::Call: call(static_cast<CallExpr*>(e)); break;
            case ExprKind::Unary: expr(static_cast<UnaryExpr*>(e)->operand, Use::Read); break;
            case ExprKind::Binary: binary(static_cast<BinaryExpr*>(e)); break;
            case ExprKind::Try: {
                auto* t = static_cast<TryExpr*>(e);
                expr(t->operand, Use::Consume);
                exit_check(Exit::Error, t->span);
                break;
            }
            case ExprKind::StructLit:
                for (FieldInit& fi : static_cast<StructLitExpr*>(e)->fields) {
                    expr(fi.value ? fi.value : fi.shorthand, Use::Consume);
                }
                break;
            case ExprKind::VariantLit: {
                auto* v = static_cast<VariantLitExpr*>(e);
                for (Expr* a : v->args) expr(a, Use::Consume);
                for (FieldInit& fi : v->fields) expr(fi.value ? fi.value : fi.shorthand, Use::Consume);
                break;
            }
            case ExprKind::ListLit:
                for (Expr* a : static_cast<ListLitExpr*>(e)->items) expr(a, Use::Consume);
                break;
            default: break;
        }
        // Новое линейное значение (результат вызова, литерал, try): его кто-то
        // должен взять.
        if (lin && use != Use::Consume && !is_place(e)) {
            error(Code::E0511, e->span, "the resource produced here is never closed; keep it in a variable and "
                                        "close it");
        }
    }

    void call(CallExpr* c) {
        auto* f = as<FieldExpr>(c->callee);
        switch (c->call) {
            case CallKind::Fn: {
                std::vector<bool> by_ref;
                for (size_t i = 0; i < c->args.size(); i++) {
                    by_ref.push_back(c->fn && i < c->fn->params.size() && c->fn->params[i].mode != ParamMode::Value);
                }
                args(c, by_ref);
                return;
            }
            case CallKind::Ptr: {  // через `extern fn(...)` (Р61): режимы — по сигнатуре типа
                expr(c->callee, Use::Read);
                const sema::CFnSig* sig = c->callee->type && c->callee->type->kind == TK::CFn ? c->callee->type->sig : nullptr;
                std::vector<bool> by_ref;
                for (size_t i = 0; i < c->args.size(); i++) {
                    by_ref.push_back(sig && i < sig->params.size() && sig->params[i].first != ParamMode::Value);
                }
                args(c, by_ref);
                return;
            }
            case CallKind::Method: {
                if (f) expr(f->object, Use::Borrow);
                // Получатель держится до конца вызова: изменяющий метод пишет в него.
                size_t held = held_.size();
                if (const Local* l = f ? place_root(f->object) : nullptr) held_.push_back(Held{l, false});
                for (Expr* a : c->args) expr(a, Use::Consume);
                held_.resize(held);
                return;
            }
            case CallKind::Variant:
                for (Expr* a : c->args) expr(a, Use::Consume);
                return;
            default:
                for (Expr* a : c->args) expr(a, Use::Read);
                return;
        }
    }

    void binary(BinaryExpr* b) {
        if (b->op == BinOp::And || b->op == BinOp::Or || b->op == BinOp::Coalesce) {
            // Правая часть вычисляется не всегда.
            expr(b->lhs, b->op == BinOp::Coalesce ? Use::Consume : Use::Read);
            Flow before = flow_;
            expr(b->rhs, b->op == BinOp::Coalesce ? Use::Consume : Use::Read);
            flow_ = merge(before, flow_);
            return;
        }
        expr(b->lhs, Use::Read);
        expr(b->rhs, Use::Read);
    }
};

bool Linear::has_break(const Block* b) {
    if (!b) return false;
    struct Find {
        static bool stmt(const Stmt* s) {
            if (!s) return false;
            switch (s->kind) {
                case StmtKind::Break: return true;
                case StmtKind::If: {
                    auto* i = static_cast<const IfStmt*>(s);
                    return block(i->then) || stmt(i->els);
                }
                case StmtKind::Match:
                    for (const MatchArm& a : static_cast<const MatchStmt*>(s)->arms) {
                        if (stmt(a.body)) return true;
                    }
                    return false;
                case StmtKind::Block: return block(static_cast<const BlockStmt*>(s)->block);
                default: return false;  // break вложенного цикла выходит только из него
            }
        }
        static bool block(const Block* x) {
            if (!x) return false;
            for (const Stmt* s : x->stmts) {
                if (stmt(s)) return true;
            }
            return false;
        }
    };
    return Find::block(b);
}

}  // namespace

void check_linear(Program& prog, ModuleInfo& m, Diagnostics& diags) {
    Linear lin(m, diags);
    for (FnDef& f : prog.fns) {
        if (f.module != &m || !f.node || f.is_extern || !f.node->body) continue;
        lin.fn(f);
    }
}

}  // namespace dp::sema
