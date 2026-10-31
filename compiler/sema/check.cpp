// Проверка тел: инструкции и поток (docs/spec/core.md, разделы 6–8).
// Выражения — check_expr.cpp, образцы — check_pat.cpp.
#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>

#include "sema/builtins.h"
#include "sema/checker.h"

namespace dp::sema {

using namespace ast;

Checker::Checker(Program& prog, ModuleInfo& m, Diagnostics& diags)
    : prog_(prog), m_(m), diags_(diags), r_(prog, m, diags), tt_(prog.types) {
    // Вариант модели задач (О13, прототип): режим замера, не часть языка.
    if (const char* v = std::getenv("DPL_TASKS"); v && *v) {
        std::string s(v);
        tasks_mode_ = s == "a" || s == "b" || s == "c" ? s[0] : '?';
    }
}

// Константы модуля, на которые ссылается значение c (по имени).
std::vector<ConstDef*> Checker::const_deps(const ConstDef& c) {
    std::vector<ConstDef*> out;
    std::vector<const Expr*> st;
    if (c.node && c.node->value) st.push_back(c.node->value);
    auto push = [&](const Expr* e) {
        if (e) st.push_back(e);
    };
    while (!st.empty()) {
        const Expr* e = st.back();
        st.pop_back();
        switch (e->kind) {
            case ExprKind::Name: {
                const Symbol* s = r_.lookup(static_cast<const NameExpr*>(e)->name);
                if (s && s->kind == SymKind::Const && s->cn->module == &m_) out.push_back(s->cn);
                break;
            }
            case ExprKind::Field: push(static_cast<const FieldExpr*>(e)->object); break;
            case ExprKind::Index:
                push(static_cast<const IndexExpr*>(e)->object);
                push(static_cast<const IndexExpr*>(e)->index);
                break;
            case ExprKind::Slice: {
                auto* x = static_cast<const SliceExpr*>(e);
                push(x->object);
                push(x->lo);
                push(x->hi);
                break;
            }
            case ExprKind::Call: {
                auto* x = static_cast<const CallExpr*>(e);
                push(x->callee);
                for (const Expr* a : x->args) push(a);
                break;
            }
            case ExprKind::Unary: push(static_cast<const UnaryExpr*>(e)->operand); break;
            case ExprKind::Binary:
                push(static_cast<const BinaryExpr*>(e)->lhs);
                push(static_cast<const BinaryExpr*>(e)->rhs);
                break;
            case ExprKind::Try: push(static_cast<const TryExpr*>(e)->operand); break;
            case ExprKind::Spawn: push(static_cast<const SpawnExpr*>(e)->operand); break;
            case ExprKind::Paren: push(static_cast<const ParenExpr*>(e)->inner); break;
            case ExprKind::Ref: push(static_cast<const RefExpr*>(e)->operand); break;
            case ExprKind::StructLit:
                for (const FieldInit& f : static_cast<const StructLitExpr*>(e)->fields) push(f.value ? f.value : f.shorthand);
                break;
            case ExprKind::VariantLit: {
                auto* x = static_cast<const VariantLitExpr*>(e);
                for (const Expr* a : x->args) push(a);
                for (const FieldInit& f : x->fields) push(f.value ? f.value : f.shorthand);
                break;
            }
            case ExprKind::ListLit:
                for (const Expr* a : static_cast<const ListLitExpr*>(e)->items) push(a);
                break;
            default: break;
        }
    }
    return out;
}

// Значение константы — после констант, на которые оно ссылается: обход
// зависимостей явным стеком, поэтому проверка значения находит их
// готовыми, и рекурсия не растёт с длиной цепочки констант. Ссылка назад
// по стеку обхода — цикл: та константа на время проверки отмечается
// вычисляемой, и const_value сообщает о цикле (E0421) на ней же, как без
// обхода.
void Checker::const_chain(ConstDef& root) {
    if (root.state != 0) return;
    struct Frame {
        ConstDef* c;
        std::vector<ConstDef*> deps;
        size_t next = 0;
    };
    std::vector<Frame> st;
    std::set<const ConstDef*> open;
    st.push_back(Frame{&root, const_deps(root)});
    open.insert(&root);
    while (!st.empty()) {
        Frame& f = st.back();
        if (f.next < f.deps.size()) {
            ConstDef* d = f.deps[f.next++];
            if (d->state == 0 && !open.count(d)) {
                std::vector<ConstDef*> deps = const_deps(*d);
                st.push_back(Frame{d, std::move(deps)});  // f больше не действителен
                open.insert(d);
            }
            continue;
        }
        ConstDef* c = f.c;
        std::vector<ConstDef*> back;
        for (ConstDef* d : f.deps) {
            if (d != c && d->state == 0 && open.count(d)) {
                d->state = 1;
                back.push_back(d);
            }
        }
        st.pop_back();
        open.erase(c);
        const_value(*c);
        for (ConstDef* d : back) {
            if (d->state == 1) d->state = 0;  // проверка до неё не дошла
        }
    }
}

void Checker::run() {
    // Константы — сначала: их значения нужны в телах.
    for (Item* it : m_.ast->items) {
        if (auto* c = as<ConstItem>(it)) {
            const Symbol* s = r_.lookup(c->name);
            if (s && s->kind == SymKind::Const && s->cn->node == c) const_chain(*s->cn);
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
    if (!declaring_params_ && type && type->kind != TK::Error) {
        if (contains_c_only(type)) {
            error(Code::E0328, span, "'" + name + "' cannot hold '" + tn(type) +
                                         "': '[N]T' and '&T' are fields of a '@c' struct; index or pass them where they are");
        } else if (c_borrowing(type)) {
            error(Code::E0328, span, "'" + name + "' cannot hold '" + tn(type) +
                                         "': it has borrowed fields and lives only in a call argument; build it in the call");
        } else if (contains_slice(type)) {
            // Имя образца, `if let` и разбора — переменная: срез в ней пережил бы
            // изменение своего списка (ref.slices, Р44).
            error(Code::E0510, span, "a slice cannot be bound to '" + name + "'; pass it to a function directly");
        }
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
    declaring_params_ = true;
    for (size_t i = 0; i < node->params.size() && i < f.params.size(); i++) {
        Param& p = node->params[i];
        if (find_local(p.name)) continue;  // повтор параметра — уже E0318 при сборе объявлений
        p.local = declare(p.name, p.name_span, f.params[i].type, p.mode == ParamMode::Mut,
                          p.mode != ParamMode::Value, true);
        if (p.local) p.local->is_param = true;
    }
    declaring_params_ = false;
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
            if (d->on_error && fn_ && (!fn_->ret || fn_->ret->kind != TK::Result)) {
                error(Code::E0505, d->span, "'errdefer' runs when the function returns an error; '" + fn_->name +
                                                "' cannot fail: use 'defer'");
            }
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
        case StmtKind::Lock: lock_stmt(static_cast<LockStmt*>(s)); break;
        case StmtKind::Tasks: tasks_stmt(static_cast<TasksStmt*>(s)); break;
        case StmtKind::Error: break;
    }
    views(s);  // Р66: вид левее действия над его местом
}

void Checker::let_stmt(LetStmt* l) {
    if (l->destructure) return destructure(l);
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
    // О13, прототип: конкурентное значение — только из atomic(…), не копия.
    if (t && holds_concurrent(t)) {
        auto* c = as<CallExpr>(l->value);
        bool fresh = c && c->call == CallKind::Builtin && concurrent(t) && !holds_concurrent(t->elem) &&
                     (c->builtin == static_cast<uint32_t>(BuiltinFn::AtomicNew) ||
                      c->builtin == static_cast<uint32_t>(BuiltinFn::MutexNew) ||
                      c->builtin == static_cast<uint32_t>(BuiltinFn::ChannelNew));
        if (!fresh) {
            error(Code::E0518, l->value ? l->value->span : l->name_span,
                  "'" + l->name + "' would hold a copy of '" + tn(t) + "'; it is shared, not copied: create it with "
                  "'atomic(v)', 'mutex(v)' or 'channel(n)' and pass '&" + l->name + "'");
            t = err();
        }
    }
    if (t && linear_in_container(t)) {
        error(Code::E0514, l->type ? l->type->span : l->name_span,
              "'" + tn(t) + "' holds a '@linear' value in a List, a Map, a Pool or a slice; keep resources in variables");
    }
    l->local = declare(l->name, l->name_span, t, l->is_var, false, l->value != nullptr);
}

// Разбор `let S { f, g: x } = v` (Р60): только `@linear` и в модуле S.
void Checker::destructure(LetStmt* l) {
    const Symbol* sym = r_.lookup(l->name);
    StructDef* d = sym && sym->kind == SymKind::Struct ? sym->st : nullptr;
    if (!d) {
        error(Code::E0515, l->name_span, "'" + l->name + "' is not a struct of this module; destructuring needs one");
        if (l->value) infer(l->value, nullptr);
        return;
    }
    if (!d->linear) {
        error(Code::E0515, l->name_span, "only a '@linear' struct is destructured; read the fields of '" + d->name +
                                             "' with 'v.f'");
    } else if (d->module != &m_) {
        error(Code::E0515, l->name_span, "'" + d->name + "' is destructured only in module '" + d->module->name +
                                             "' that declares it; close it with that module's functions");
    }
    l->sdef = d;
    if (l->value) check(l->value, tt_.struct_type(d));
    std::vector<char> seen(d->fields.size(), 0);
    for (LetField& f : l->fields) {
        int idx = -1;
        for (size_t i = 0; i < d->fields.size(); i++) {
            if (d->fields[i].name == f.field) idx = static_cast<int>(i);
        }
        if (idx < 0) {
            error(Code::E0406, f.field_span, "struct '" + d->name + "' has no field '" + f.field + "'");
            continue;
        }
        if (seen[static_cast<size_t>(idx)]) {
            error(Code::E0515, f.field_span, "field '" + f.field + "' is listed twice");
            continue;
        }
        seen[static_cast<size_t>(idx)] = 1;
        f.index = idx;
        f.local = declare(f.name, f.name_span, d->fields[static_cast<size_t>(idx)].type, l->is_var, false, true);
    }
    for (size_t i = 0; i < d->fields.size(); i++) {
        if (!seen[i]) {
            error(Code::E0515, l->name_span, "field '" + d->fields[i].name + "' is missing; list every field of '" +
                                                 d->name + "', '" + d->fields[i].name + ": _' to skip it");
        }
    }
}

// `lock m as t { … }` (О13, прототип): t — данные m (`&mut`) до конца блока.
void Checker::lock_stmt(LockStmt* s) {
    const Type* t = infer(s->mutex, nullptr);
    const Type* elem = err();
    if (t->kind == TK::Mutex) {
        elem = t->elem;
    } else if (t->kind != TK::Error) {
        error(Code::E0401, s->mutex->span, "'lock' needs a Mutex, found '" + tn(t) + "'");
    }
    if (fn_ && is_det(*fn_)) {
        error(Code::E0425, s->span, "a 'Mutex' is shared between threads; '" + fn_->name +
                                        "' is class D and cannot use it");
    }
    Local* root = root_local(s->mutex);
    push_scope();
    s->local = declare(s->var, s->var_span, elem, true, true, true);
    if (s->local) s->local->alias_of = root;
    locked_.push_back(root);
    block(s->body);
    locked_.pop_back();
    pop_scope();
}

// `tasks { … }` — вариант А (О13, прототип): задачи ставит `spawn`.
// `parallel { … }` — вариант Б: каждый оператор — задача. Блок кончается,
// когда кончились его задачи; выйти из него раньше нельзя (E0516).
void Checker::tasks_stmt(TasksStmt* s) {
    variant_ok(s->each ? 'b' : 'a', s->span, s->each ? "'parallel { ... }'" : "'tasks { ... }'");
    if (fn_ && is_det(*fn_)) {
        error(Code::E0425, s->span, "tasks run at the same time; '" + fn_->name + "' is class D and cannot start them");
    }
    TasksScope sc;
    sc.first_inner = next_local_;
    sc.each = s->each;
    tasks_.push_back(std::move(sc));
    loops_.emplace_back();
    loops_.back().tasks = true;
    if (!s->each) {
        block(s->body);
    } else if (s->body) {
        push_scope();
        int k = 0;
        for (Stmt* x : s->body->stmts) {
            if (x->kind == StmtKind::Let || x->kind == StmtKind::Defer) {
                error(Code::E0520, x->span, x->kind == StmtKind::Let
                                                ? "a variable of a statement in 'parallel { ... }' ends with it; "
                                                  "declare it before the block and assign it here"
                                                : "'parallel { ... }' runs each statement as a task; put 'defer' "
                                                  "before the block");
            }
            size_t mark = tasks_.back().accesses.size();
            stmt(x);
            std::vector<TaskAccess>& acc = tasks_.back().accesses;
            for (size_t i = mark; i < acc.size(); i++) acc[i].stmt = k;
            k++;
        }
        pop_scope();
    }
    loops_.pop_back();
    tasks_end();
    tasks_.pop_back();
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
        if (n->res.kind == ResKind::Local && holds_concurrent(n->res.local->type)) {  // О13, прототип
            error(Code::E0518, n->span, "cannot assign to '" + n->name + "': '" + tn(n->res.local->type) +
                                            "' is shared, not replaced; use its methods");
            infer(a->value, nullptr);
            return;
        }
        if (n->res.kind == ResKind::Local) {
            Local* l = n->res.local;
            n->type = l->type;
            check(a->value, l->type);
            if (mutable_place(n, "assign to")) note_write(n, "assign to");
            if (flow_.init.size() > l->id) flow_.init[l->id] = 1;
            return;
        }
    }
    const Type* t = infer(a->target, nullptr);
    if (mutable_place(a->target, "assign to")) {  // Р66; `m[k] = v` может добавить ключ — меняет m
        Expr* target = a->target;
        while (auto* p = as<ParenExpr>(target)) target = p->inner;
        auto* ix = as<IndexExpr>(target);
        if (ix && ix->object->type && ix->object->type->kind == TK::Map) note_write(ix->object, "assign to");
        else note_write(target, "assign to");
    }
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
    // `spawn f(…)` оператором в блоке `tasks` — задача блока (вариант А).
    auto* sp = as<SpawnExpr>(e);
    if (sp && !tasks_.empty() && !tasks_.back().each) sp->group = true;
    const Type* t = infer(e, nullptr);
    if (sp) return;
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
    if (!tasks_.empty()) task_loop_begin(nullptr);
    block(s->body);
    if (!tasks_.empty()) task_loop_end();
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
        } else if (t->kind == TK::Pool) {  // Р67: живые элементы по возрастанию номера слота
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
    // Р66: в теле коллекцию меняют и (в `&mut`-обходе) читают только через
    // переменную цикла; `parallel for` пишет снаружи только `out[i]`.
    loop_begin(s);
    loops_.emplace_back();
    loops_.back().parallel = s->parallel;
    if (s->parallel) parallel_depth_++;
    if (!tasks_.empty()) task_loop_begin(s);
    block(s->body);
    if (!tasks_.empty()) task_loop_end();
    if (s->parallel) parallel_depth_--;
    loops_.pop_back();
    loop_end();
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
    } else if (loops_.back().tasks) {
        error(Code::E0516, s->span, std::string("'") + (is_break ? "break" : "continue") + "' cannot leave '" +
                                        (!tasks_.empty() && tasks_.back().each ? "parallel { ... }" : "tasks") +
                                        "': the block waits for its tasks; leave the loop after the block");
    } else if (is_break) {
        if (loops_.back().parallel) parallel_exit(s->span, "break");
        loops_.back().breaks.push_back(flow_);
    }
    flow_.live = false;
}

bool Checker::parallel_exit(Span span, const char* what) {
    std::string w(what);
    if (parallel_depth_ == 0 && !tasks_.empty()) {
        error(Code::E0516, span, "'" + w + "' cannot leave '" + (tasks_.back().each ? "parallel { ... }" : "tasks") +
                                     "': the block waits for its tasks; write the outcome to a variable and act on "
                                     "it after the block");
        return true;
    }
    if (parallel_depth_ == 0) return false;
    std::string hint = w == "break" ? "skip the rest of this iteration with 'continue'"
                       : w == "return" ? "write the result to an element 'out[i]' and return after the loop"
                                       : "handle the error with 'match' and write it to an element 'out[i]'";
    error(Code::E0516, span, "'" + w + "' cannot leave 'parallel for': iterations run at the same time; " + hint);
    return true;
}

void Checker::return_stmt(ReturnStmt* s) {
    if (defer_depth_ > 0) error(Code::E0508, s->span, "'return' cannot leave a 'defer'");
    else parallel_exit(s->span, "return");
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
    else parallel_exit(s->span, "fail");
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
    for (auto& m : prog.modules) {
        size_t before = diags.error_count();
        Checker(prog, *m, diags).run();
        // Линейность — по дереву без ошибок, чтобы не множить сообщения.
        if (diags.error_count() == before) check_linear(prog, *m, diags);
    }
    if (diags.error_count() == 0) check_parallel(prog, diags);
}

}  // namespace dp::sema
