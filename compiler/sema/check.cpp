// Проверка тел: инструкции и поток (docs/spec/core.md, разделы 6–8).
// Выражения — check_expr.cpp, образцы — check_pat.cpp.
#include <algorithm>

#include "sema/builtins.h"
#include "sema/checker.h"

namespace dp::sema {

using namespace ast;

Checker::Checker(Program& prog, ModuleInfo& m, Diagnostics& diags)
    : prog_(prog), m_(m), diags_(diags), r_(prog, m, diags), tt_(prog.types) {}

void Checker::run() {
    // Константы — сначала: их значения нужны в телах.
    for (Item* it : m_.ast->items) {
        if (auto* c = as<ConstItem>(it)) {
            const Symbol* s = r_.lookup(c->name);
            if (s && s->kind == SymKind::Const && s->cn->node == c) const_value(*s->cn);
        }
    }
    for (Item* it : m_.ast->items) {
        const Symbol* s = r_.lookup(it->name);
        if (!s) continue;
        if (it->kind == ItemKind::Struct && s->kind == SymKind::Struct && s->st->node == it) struct_defaults(*s->st);
        if (it->kind == ItemKind::Fn && s->kind == SymKind::Fn && s->fn->node == it) fn(*s->fn);
    }
}

// --- области

Local* Checker::find_local(const std::string& name) const {
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s) {
        for (auto l = s->rbegin(); l != s->rend(); ++l) {
            if ((*l)->name == name) return *l;
        }
    }
    return nullptr;
}

Local* Checker::declare(const std::string& name, Span span, const Type* type, bool mutable_, bool is_ref, bool init) {
    if (name == "_" || name.empty()) return nullptr;
    if (or_depth_ > 0) {
        error(Code::E0321, span, "patterns joined by '|' cannot bind names");
        return nullptr;
    }
    if (Local* prev = find_local(name)) {
        Diagnostic& d = error(Code::E0310, span, "'" + name + "' is already defined in this function");
        d.notes.push_back(Note{prev->span, true, "previous definition here"});
        return nullptr;
    }
    Local& l = prog_.locals.emplace_back();
    l.name = name;
    l.span = span;
    l.type = type ? type : err();
    l.mutable_ = mutable_;
    l.is_ref = is_ref;
    l.id = next_local_++;
    scopes_.back().push_back(&l);
    if (flow_.init.size() <= l.id) flow_.init.resize(l.id + 1, 0);
    flow_.init[l.id] = init ? 1 : 0;
    return &l;
}

// --- объявления

void Checker::struct_defaults(StructDef& s) {
    for (FieldDef& f : s.fields) {
        if (f.default_value) check(const_cast<Expr*>(f.default_value), f.type);
    }
}

void Checker::fn(FnDef& f) {
    if (f.is_extern) return;  // функция C: тела нет (Р59)
    fn_ = &f;
    next_local_ = 0;
    flow_ = Flow{};
    loops_.clear();
    push_scope();
    FnItem* node = const_cast<FnItem*>(f.node);
    for (size_t i = 0; i < node->params.size() && i < f.params.size(); i++) {
        Param& p = node->params[i];
        if (find_local(p.name)) continue;  // повтор параметра — уже E0318 при сборе объявлений
        p.local = declare(p.name, p.name_span, f.params[i].type, p.mode == ParamMode::Mut,
                          p.mode != ParamMode::Value, true);
        if (p.local) p.local->is_param = true;
    }
    block(node->body);
    // Конец тела достижим: функция с результатом (кроме `!void`) обязана вернуть значение.
    if (flow_.live && f.ret && f.ret->kind != TK::Void && f.ret->kind != TK::Error) {
        bool void_result = f.ret->kind == TK::Result && f.ret->elem->kind == TK::Void;
        if (!void_result) {
            Span end{m_.file, node->body ? node->body->span.end - 1 : node->span.end, node->body ? node->body->span.end : node->span.end};
            error(Code::E0504, end, "function '" + f.name + "' can reach its end without returning a value");
        }
    }
    pop_scope();
    f.local_count = next_local_;
    fn_ = nullptr;
}

// --- поток

Checker::Flow Checker::merge(const Flow& a, const Flow& b) {
    if (!a.live) return b;
    if (!b.live) return a;
    Flow out;
    out.live = true;
    size_t n = std::max(a.init.size(), b.init.size());
    out.init.assign(n, 0);
    for (size_t i = 0; i < n; i++) {
        char x = i < a.init.size() ? a.init[i] : 0;
        char y = i < b.init.size() ? b.init[i] : 0;
        out.init[i] = x && y;
    }
    return out;
}

// --- инструкции

void Checker::block(Block* b) {
    if (!b) return;
    push_scope();
    for (Stmt* s : b->stmts) stmt(s);
    pop_scope();
}

void Checker::stmt(Stmt* s) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Let: let_stmt(static_cast<LetStmt*>(s)); break;
        case StmtKind::Assign: assign_stmt(static_cast<AssignStmt*>(s)); break;
        case StmtKind::Expr: expr_stmt(static_cast<ExprStmt*>(s)); break;
        case StmtKind::If: if_stmt(static_cast<IfStmt*>(s)); break;
        case StmtKind::While: while_stmt(static_cast<WhileStmt*>(s)); break;
        case StmtKind::For: for_stmt(static_cast<ForStmt*>(s)); break;
        case StmtKind::Match: match_stmt(static_cast<MatchStmt*>(s)); break;
        case StmtKind::Return: return_stmt(static_cast<ReturnStmt*>(s)); break;
        case StmtKind::Fail: fail_stmt(static_cast<FailStmt*>(s)); break;
        case StmtKind::Break: loop_jump(s, true); break;
        case StmtKind::Continue: loop_jump(s, false); break;
        case StmtKind::Defer: {
            auto* d = static_cast<DeferStmt*>(s);
            Flow saved = flow_;
            defer_depth_++;
            std::vector<Loop> saved_loops;
            saved_loops.swap(loops_);  // break и continue из defer не выходят
            stmt(d->body);
            loops_.swap(saved_loops);
            defer_depth_--;
            flow_ = saved;
            break;
        }
        case StmtKind::Block: block(static_cast<BlockStmt*>(s)->block); break;
        case StmtKind::Error: break;
    }
}

void Checker::let_stmt(LetStmt* l) {
    const Type* t = nullptr;
    if (l->type) {
        t = r_.type(l->type);
        if (contains_slice(t)) {
            error(Code::E0510, l->type->span, "a slice cannot be stored in a variable; pass it to a function directly");
            t = err();
        }
    }
    if (l->value) {
        if (t) {
            check(l->value, t);
        } else {
            t = infer(l->value, nullptr);
            if (t->kind == TK::Void) {
                error(Code::E0401, l->value->span, "expression has no value to store in '" + l->name + "'");
                t = err();
            } else if (t->kind == TK::Result) {
                error(Code::E0506, l->value->span,
                      "the result of type '" + tn(t) + "' must be handled: use 'try', 'match' or '_ ='");
            } else if (contains_slice(t)) {
                error(Code::E0510, l->value->span,
                      "a slice cannot be stored in a variable; pass it to a function directly");
                t = err();
            }
        }
    }
    l->local = declare(l->name, l->name_span, t, l->is_var, false, l->value != nullptr);
}

void Checker::assign_stmt(AssignStmt* a) {
    auto* n = as<NameExpr>(a->target);
    if (n && n->name == "_" && a->op == AssignOp::Set) {  // `_ = e` — явный отброс (Р10)
        const Type* t = infer(a->value, nullptr);
        if (t->kind == TK::Void) error(Code::E0401, a->value->span, "expression has no value to discard");
        return;
    }
    // Простое присваивание переменной инициализирует её; остальное читает цель.
    if (n && a->op == AssignOp::Set) {
        resolve_name(n);
        if (n->res.kind == ResKind::Local) {
            Local* l = n->res.local;
            n->type = l->type;
            check(a->value, l->type);
            if (frozen(l)) {
                error(Code::E0412, n->span, "cannot assign to '" + l->name +
                                                "' while 'for ... in &' iterates over its Map; iterate over a copy "
                                                "of the keys: 'for k in &m.keys()'");
            } else if (!l->mutable_) {
                error(Code::E0412, n->span, "cannot assign to '" + l->name + "': it is not declared with 'var'");
            }
            if (flow_.init.size() > l->id) flow_.init[l->id] = 1;
            return;
        }
    }
    const Type* t = infer(a->target, nullptr);
    mutable_place(a->target, "assign to");
    if (a->op == AssignOp::Set) {
        check(a->value, t);
        return;
    }
    // Составное присваивание `x op= e` — как `x = x op e` с типом x.
    if (t->kind == TK::Error) {
        infer(a->value, nullptr);
        return;
    }
    bool shift = a->op == AssignOp::Shl || a->op == AssignOp::Shr;
    bool int_only = shift || a->op == AssignOp::Rem || a->op == AssignOp::BitAnd || a->op == AssignOp::BitOr ||
                    a->op == AssignOp::BitXor;
    if (shift) {
        const Type* c = infer(a->value, nullptr);
        if (!is_integer(c) && c->kind != TK::Error) error(Code::E0403, a->value->span, "shift count must be an integer");
    } else {
        check(a->value, t);
    }
    bool ok = int_only ? is_integer(t) : (is_numeric(t) || (a->op == AssignOp::Add && t->kind == TK::Str));
    if (!ok) error(Code::E0403, a->target->span, "this compound assignment does not apply to '" + tn(t) + "'");
}

void Checker::expr_stmt(ExprStmt* s) {
    Expr* e = s->expr;
    const Type* t = infer(e, nullptr);
    Expr* inner = e;
    if (auto* tr = as<TryExpr>(inner)) inner = tr->operand;
    if (!as<CallExpr>(inner)) {
        if (e->kind != ExprKind::Error && t->kind != TK::Error) {
            error(Code::E0411, e->span, "this expression has no effect; a statement must be a call or an assignment");
        }
        return;
    }
    if (t->kind == TK::Result) {
        error(Code::E0506, e->span, "the error of this call is ignored; handle it with 'try', 'match' or '_ = ...'");
    }
}

void Checker::if_stmt(IfStmt* s) {
    push_scope();
    if (s->cond) {
        check(s->cond, prim(TK::Bool));
    } else {
        const Type* t = infer(s->let_value, nullptr);
        const Type* inner = err();
        if (t->kind == TK::Optional) inner = t->elem;
        else if (t->kind != TK::Error) {
            error(Code::E0417, s->let_value->span, "'if let' needs an optional value '?T', found '" + tn(t) + "'");
        }
        s->let_local = declare(s->let_name, s->let_span, inner, false, false, true);
    }
    Flow before = flow_;
    block(s->then);
    pop_scope();
    Flow after_then = flow_;
    flow_ = before;
    if (s->els) stmt(s->els);
    flow_ = merge(after_then, flow_);
}

void Checker::while_stmt(WhileStmt* s) {
    check(s->cond, prim(TK::Bool));
    auto* lit = as<BoolExpr>(s->cond);
    bool forever = lit && lit->value;
    Flow before = flow_;
    loops_.emplace_back();
    block(s->body);
    Loop loop = std::move(loops_.back());
    loops_.pop_back();
    // После цикла — состояние до него (тело могло не выполниться) и состояния у break.
    Flow after = before;
    if (forever) after.live = false;
    for (const Flow& b : loop.breaks) after = merge(after, b);
    flow_ = after;
}

void Checker::for_stmt(ForStmt* s) {
    const Type* elem = err();
    bool is_ref = s->mode == ForMode::Ref || s->mode == ForMode::Mut;
    if (s->mode == ForMode::Range) {
        elem = common(s->iter, s->hi, nullptr, s->var_span, "..");
        if (!is_integer(elem) && elem->kind != TK::Error) {
            error(Code::E0419, s->iter->span, "a range 'a..b' needs integers, found '" + tn(elem) + "'");
            elem = err();
        }
    } else {
        const Type* t = infer(s->iter, nullptr);
        if (s->mode == ForMode::Value) {
            if (t->kind != TK::Error) {
                error(Code::E0419, s->iter->span, "iterate a collection through a mark: 'for x in &xs'");
            }
        } else if (t->kind == TK::List) {
            elem = t->elem;
            if (s->mode == ForMode::Mut) mutable_place(s->iter, "mutate");
        } else if (t->kind == TK::Map) {  // Р54: ключи по возрастанию
            elem = t->key;
            if (s->mode == ForMode::Mut) {
                error(Code::E0419, s->iter->span, "keys of a Map cannot be changed; iterate with 'for k in &m'");
            }
        } else if (t->kind == TK::Slice || t->kind == TK::Str) {
            elem = t->kind == TK::Str ? prim(TK::U8) : t->elem;
            if (s->mode == ForMode::Mut) {
                error(Code::E0412, s->iter->span, "'" + tn(t) + "' is read-only; 'for ... in &mut' needs a List");
            }
        } else if (t->kind != TK::Error) {
            error(Code::E0419, s->iter->span, "cannot iterate over '" + tn(t) + "'");
        }
    }
    s->elem = elem;
    Flow before = flow_;
    push_scope();
    s->local = declare(s->var, s->var_span, elem, s->mode == ForMode::Mut, is_ref, true);
    if (s->local && is_ref) s->local->alias_of = root_local(s->iter);  // элемент xs — корень xs (Р44)
    // Обход Map: менять отображение в теле нельзя (Р54) — корень замораживается.
    Local* frozen_root = s->iter && s->iter->type && s->iter->type->kind == TK::Map ? root_local(s->iter) : nullptr;
    if (frozen_root) frozen_.push_back(frozen_root);
    loops_.emplace_back();
    block(s->body);
    loops_.pop_back();
    if (frozen_root) frozen_.pop_back();
    pop_scope();
    // Тело могло не выполниться ни разу.
    flow_ = before;
}

void Checker::match_stmt(MatchStmt* s) {
    const Type* t = infer(s->subject, nullptr);
    Flow before = flow_;
    Flow after;
    after.live = false;
    for (MatchArm& a : s->arms) {
        flow_ = before;
        push_scope();
        pat(a.pat, t);
        stmt(a.body);
        pop_scope();
        after = merge(after, flow_);
    }
    exhaustive(s, t);
    flow_ = s->arms.empty() ? before : after;
}

void Checker::loop_jump(Stmt* s, bool is_break) {
    if (loops_.empty()) {
        if (defer_depth_ > 0) {
            error(Code::E0508, s->span, std::string("'") + (is_break ? "break" : "continue") + "' cannot leave a 'defer'");
        } else {
            error(Code::E0420, s->span, std::string("'") + (is_break ? "break" : "continue") + "' outside of a loop");
        }
    } else if (is_break) {
        loops_.back().breaks.push_back(flow_);
    }
    flow_.live = false;
}

void Checker::return_stmt(ReturnStmt* s) {
    if (defer_depth_ > 0) error(Code::E0508, s->span, "'return' cannot leave a 'defer'");
    const Type* ret = fn_ ? fn_->ret : prim(TK::Void);
    bool void_ret = ret->kind == TK::Void || (ret->kind == TK::Result && ret->elem->kind == TK::Void);
    if (!s->value) {
        if (!void_ret && ret->kind != TK::Error) {
            error(Code::E0509, s->span, "'return' needs a value of type '" + tn(ret) + "'");
        }
    } else if (ret->kind == TK::Void) {
        infer(s->value, nullptr);
        error(Code::E0509, s->value->span, "function '" + fn_->name + "' has no result; 'return' takes no value");
    } else {
        check(s->value, ret);
    }
    flow_.live = false;
}

void Checker::fail_stmt(FailStmt* s) {
    if (defer_depth_ > 0) error(Code::E0508, s->span, "'fail' cannot leave a 'defer'");
    const Type* ret = fn_ ? fn_->ret : prim(TK::Void);
    if (ret->kind != TK::Result) {
        infer(s->value, nullptr);
        if (ret->kind != TK::Error) {
            error(Code::E0505, s->span, "'fail' needs a function whose result is 'E!T'");
        }
    } else if (!ret->err) {
        // Тип ошибки не выведен — сообщение уже есть (E0507); значение не проверяем.
    } else {
        check(s->value, ret->err);
    }
    flow_.live = false;
}

bool check_main(Program& prog, Diagnostics& diags) {
    auto it = prog.root->scope.find("main");
    if (it == prog.root->scope.end() || it->second.kind != SymKind::Fn) {
        diags.error(Code::E0322, "module '" + prog.root->name + "' has no function 'main' to start the program");
        return false;
    }
    const FnDef& f = *it->second.fn;
    bool ok_ret = f.ret->kind == TK::Void || (f.ret->kind == TK::Result && f.ret->elem->kind == TK::Void);
    if (!f.params.empty() || !ok_ret) {
        diags.error(Code::E0322, f.span, "'main' must take no parameters and return nothing or 'E!void'");
        return false;
    }
    return true;
}

void check(Program& prog, Diagnostics& diags) {
    for (auto& m : prog.modules) infer_errors(prog, *m, diags);
    for (auto& m : prog.modules) Checker(prog, *m, diags).run();
}

}  // namespace dp::sema
