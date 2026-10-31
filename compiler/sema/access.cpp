// Исключительный изменяемый доступ (Р66; docs/spec/core.md, ref.alias,
// stmt.for, expr.order). Место — путь от переменной: поля и постоянные
// индексы; два места пересекаются, если одно — начало другого или их
// индексы вычислены. Правило одно для вызова (E0502), обхода и `parallel
// for` (E0412) и вида левее действия (E0502).
#include <algorithm>

#include "sema/checker.h"

namespace dp::sema {

using namespace ast;

namespace {

// Шаг пути от переменной к месту: поле, постоянный индекс, другой индекс,
// срез.
struct Step {
    enum Kind : uint8_t { Field, Const, Index, Slice } kind = Field;
    uint64_t value = 0;
    const Expr* index = nullptr;
};

using Iters = std::map<const Local*, Expr*>;

Expr* bare(Expr* e) {
    while (auto* p = as<ParenExpr>(e)) e = p->inner;
    return e;
}

// Переменная, названная в основе места (переменная обхода — она сама).
const Local* raw_base(Expr* e) {
    e = bare(e);
    if (auto* n = as<NameExpr>(e)) return n->res.kind == ResKind::Local ? n->res.local : nullptr;
    if (auto* f = as<FieldExpr>(e)) return f->field >= 0 ? raw_base(f->object) : nullptr;
    if (auto* i = as<IndexExpr>(e)) return raw_base(i->object);
    if (auto* s = as<SliceExpr>(e)) return raw_base(s->object);
    return nullptr;
}

// Корень места и шаги от него; переменная обхода `for x in &xs` — элемент
// своей коллекции по вычисленному индексу. nullptr — не место.
const Local* path(Expr* e, const Iters& iters, std::vector<Step>& out, int depth = 0) {
    e = bare(e);
    if (!e || depth > 64) return nullptr;
    if (auto* n = as<NameExpr>(e)) {
        if (n->res.kind != ResKind::Local) return nullptr;
        const Local* l = n->res.local;
        auto it = iters.find(l);
        if (it != iters.end() && l->alias_of) {
            const Local* b = path(it->second, iters, out, depth + 1);
            if (b) {
                out.push_back(Step{Step::Index});
                return b;
            }
        }
        return l;
    }
    const Local* b = nullptr;
    if (auto* f = as<FieldExpr>(e)) {
        if (f->field < 0 || !(b = path(f->object, iters, out, depth + 1))) return nullptr;
        out.push_back(Step{Step::Field, static_cast<uint64_t>(f->field)});
    } else if (auto* i = as<IndexExpr>(e)) {
        if (!(b = path(i->object, iters, out, depth + 1))) return nullptr;
        Expr* ix = bare(i->index);
        if (auto* k = as<IntExpr>(ix)) out.push_back(Step{Step::Const, k->value, ix});
        else out.push_back(Step{Step::Index, 0, ix});
    } else if (auto* s = as<SliceExpr>(e)) {
        if (!(b = path(s->object, iters, out, depth + 1))) return nullptr;
        out.push_back(Step{Step::Slice});
    }
    return b;
}

// Переменная l — var или переменная обхода части var.
bool derived(const Local* l, const Local* var, const Iters& iters) {
    for (int guard = 0; l && var && guard < 64; guard++) {
        if (l == var) return true;
        auto it = iters.find(l);
        if (it == iters.end()) return false;
        l = raw_base(it->second);
    }
    return false;
}

// Пути от одного корня пересекаются: до первого расхождения одинаковы, и
// расхождение — не разные поля и не разные постоянные индексы.
bool crosses(const std::vector<Step>& a, const std::vector<Step>& b) {
    for (size_t i = 0; i < a.size() && i < b.size(); i++) {
        if (a[i].kind != b[i].kind || a[i].kind == Step::Index || a[i].kind == Step::Slice) return true;
        if (a[i].value != b[i].value) return false;
    }
    return true;
}

// Запись места для сообщения: `xs`, `w.bodies`, `xs[0]`, `xs[i]`.
std::string text(Expr* e) {
    e = bare(e);
    if (auto* n = as<NameExpr>(e)) return n->name;
    if (auto* f = as<FieldExpr>(e)) return text(f->object) + "." + f->name;
    if (auto* i = as<IndexExpr>(e)) {
        Expr* ix = bare(i->index);
        if (auto* k = as<IntExpr>(ix)) return text(i->object) + "[" + std::to_string(k->value) + "]";
        if (auto* n = as<NameExpr>(ix)) return text(i->object) + "[" + n->name + "]";
        return text(i->object) + "[...]";
    }
    if (auto* s = as<SliceExpr>(e)) return text(s->object) + "[..]";
    return "...";
}

// Чтения коллекции обхода, которые сами — место записи или обхода, и
// цепочка его объектов: это не чтения. Индекс в месте (`xs[xs[0]] = 1`) —
// чтение.
template <class Reads>
void drop_reads(Reads& reads, Expr* place) {
    std::vector<const Expr*> chain;
    for (Expr* e = place; e;) {
        e = bare(e);
        chain.push_back(e);
        if (auto* f = as<FieldExpr>(e)) e = f->object;
        else if (auto* i = as<IndexExpr>(e)) e = i->object;
        else if (auto* sl = as<SliceExpr>(e)) e = sl->object;
        else e = nullptr;
    }
    reads.erase(std::remove_if(reads.begin(), reads.end(),
                               [&](const auto& r) { return std::find(chain.begin(), chain.end(), r.place) != chain.end(); }),
                reads.end());
}

// Чтения кода блока задач (с номера from), которые сами — место записи
// или заимствования и цепочка его объектов: это не чтения.
void drop_task_reads(std::vector<Checker::TaskAccess>& acc, Expr* place, size_t from) {
    std::vector<const Expr*> chain;
    for (Expr* e = place; e;) {
        e = bare(e);
        chain.push_back(e);
        if (auto* f = as<FieldExpr>(e)) e = f->object;
        else if (auto* i = as<IndexExpr>(e)) e = i->object;
        else if (auto* sl = as<SliceExpr>(e)) e = sl->object;
        else e = nullptr;
    }
    auto in_chain = [&](const Checker::TaskAccess& a) {
        return !a.write && std::find(chain.begin(), chain.end(), a.place) != chain.end();
    };
    if (from > acc.size()) from = acc.size();
    acc.erase(std::remove_if(acc.begin() + static_cast<std::ptrdiff_t>(from), acc.end(), in_chain), acc.end());
}

// Путь проходит индексом по переменной цикла: `out[i]`, `out[i].x`.
bool by_var(const std::vector<Step>& steps, const Local* var) {
    for (const Step& s : steps) {
        auto* n = s.kind == Step::Index ? as<NameExpr>(const_cast<Expr*>(s.index)) : nullptr;
        if (n && n->res.kind == ResKind::Local && n->res.local == var) return true;
    }
    return false;
}

// Что обходит цикл — для сообщения.
std::string over(TK coll) {
    if (coll == TK::Map) return "its Map; iterate over a copy of the keys: 'for k in &m.keys()'";
    if (coll == TK::Pool) return "its Pool; iterate over a copy of the handles: 'for h in &p.handles()'";
    if (coll == TK::Slice) return "its slice; iterate over a slice of a copy made before the loop";
    return "it";
}

}  // namespace

bool Checker::read_only_loop(const Local* l) const {
    auto it = loop_iters_.find(l);
    if (it == loop_iters_.end() || !it->second->type) return false;
    TK k = it->second->type->kind;
    return k == TK::Map || k == TK::Slice || k == TK::Str;
}

bool Checker::overlap(Expr* a, Expr* b) const {
    std::vector<Step> pa, pb;
    const Local* ra = path(a, loop_iters_, pa);
    const Local* rb = path(b, loop_iters_, pb);
    return !ra || !rb || ra != rb || crosses(pa, pb);
}

void Checker::note_write(Expr* place, const char* why, bool loops) {
    if (!tasks_.empty() && place && !concurrent(place->type)) {
        // Код блока задач меняет место; его чтение целью записи — не чтение.
        // Заголовок `for x in &mut xs` (loops = false) сам не пишет: пишут
        // через x.
        for (TasksScope& sc : tasks_) {
            drop_task_reads(sc.accesses, place, 0);
            if (loops) sc.accesses.push_back(TaskAccess{place, true, false});
        }
    }
    if (access_loops_.empty() || !place) return;
    std::vector<Step> steps;
    const Local* root = path(place, loop_iters_, steps);
    const Local* base = raw_base(place);
    if (!root || !base) return;
    drop_reads(loop_reads_, place);  // цель записи при выводе её типа прочитана
    std::string head = std::string("cannot ") + why + " '" + text(place) + "'";
    for (AccessLoop& L : access_loops_) {
        if (derived(base, L.var, loop_iters_)) continue;
        if (loops && L.root && root == L.root && L.coll != TK::Error) {
            std::vector<Step> coll;
            path(L.iter, loop_iters_, coll);
            if (crosses(steps, coll)) {
                std::string loop = L.mut ? "'for ... in &mut'" : "'for ... in &'";
                std::string hint = L.coll == TK::List ? "; change it through the loop variable, or iterate over indexes: "
                                                        "'for i in 0..len(" + text(L.iter) + ")'"
                                                      : "";
                error(Code::E0412, place->span, head + " while " + loop + " iterates over " + over(L.coll) + hint);
                return;
            }
        }
        if (!L.parallel || base->id >= L.first_inner) continue;
        // Снаружи тела `parallel for` пишется только элемент по переменной
        // цикла (`out[i]`, `out[i].push(x)`): итерации идут одновременно, и
        // у каждой свой элемент.
        if (by_var(steps, L.var)) {
            // Р69: читать эту переменную тело дальше может только как out[i].
            if (std::find(L.written.begin(), L.written.end(), root) == L.written.end()) L.written.push_back(root);
            continue;
        }
        std::string var = L.var ? L.var->name : "i";
        error(Code::E0412, place->span,
              head + " inside 'parallel for': iterations run at the same time; write each result to an element "
                     "indexed by the loop variable, such as 'out[" + var + "]', and combine them after the loop");
        return;
    }
}

void Checker::note_use(NameExpr* n) {
    if (!tasks_.empty() && n->res.kind == ResKind::Local && !concurrent(n->res.local->type)) {
        for (TasksScope& sc : tasks_) sc.accesses.push_back(TaskAccess{n, false, false});
    }
    if (access_loops_.empty() || n->res.kind != ResKind::Local) return;
    for (size_t k = 0; k < access_loops_.size(); k++) {
        const AccessLoop& L = access_loops_[k];
        if (L.mut && L.root == n->res.local) loop_reads_.push_back(LoopRead{n, k, false});
        // Р69: внешняя переменная в теле `parallel for` — проверка в конце
        // тела, когда известно, что тело пишет.
        if (L.parallel && n->res.local->id < L.first_inner) loop_reads_.push_back(LoopRead{n, k, true});
    }
}

void Checker::note_len(Expr* e) {
    e = bare(e);
    for (TasksScope& sc : tasks_) {
        for (TaskAccess& a : sc.accesses) {
            if (a.place == e && !a.write) a.len = true;
        }
    }
    loop_reads_.erase(std::remove_if(loop_reads_.begin(), loop_reads_.end(),
                                     [&](const LoopRead& r) { return r.par && r.place == e; }),
                      loop_reads_.end());
}

void Checker::note_extend(Expr* object, Expr* place) {
    for (LoopRead& r : loop_reads_) {
        if (r.place == object) r.place = place;
    }
    for (TasksScope& sc : tasks_) {
        for (TaskAccess& a : sc.accesses) {
            if (a.place == object && !a.write) a.place = place;
        }
    }
}

void Checker::loop_begin(ForStmt* s) {
    AccessLoop L;
    L.var = s->local;
    L.mut = s->mode == ForMode::Mut;
    L.parallel = s->parallel;
    L.first_inner = s->local ? s->local->id : next_local_;
    L.iter = s->iter;
    if ((s->mode == ForMode::Ref || L.mut) && s->iter && s->iter->type) {
        TK k = s->iter->type->kind;
        if (k == TK::List || k == TK::Map || k == TK::Pool || k == TK::Slice) L.coll = k;
        // Строка — как список байтов; её срез — копия (ref.slices): обход
        // среза строки корень не держит.
        if (k == TK::Str && !as<SliceExpr>(bare(s->iter))) L.coll = TK::List;
        std::vector<Step> steps;
        L.root = path(s->iter, loop_iters_, steps);
        if (s->local) loop_iters_[s->local] = s->iter;
        // Второй обход той же коллекции, если хоть один из двух её меняет; её
        // чтение в заголовке сверяет это правило, а не правило чтения.
        const Local* base = raw_base(s->iter);
        drop_reads(loop_reads_, s->iter);
        for (const AccessLoop& o : access_loops_) {
            if (!o.root || o.root != L.root || o.coll == TK::Error || !(o.mut || L.mut) ||
                derived(base, o.var, loop_iters_)) {
                continue;
            }
            std::vector<Step> outer;
            path(o.iter, loop_iters_, outer);
            if (!crosses(steps, outer)) continue;
            error(Code::E0412, s->iter->span,
                  "cannot iterate over '" + text(s->iter) + "' " + (L.mut ? "with '&mut' " : "") + "while 'for ... in " +
                      (o.mut ? "&mut" : "&") + "' iterates over it; iterate over indexes: 'for i in 0..len(" +
                      text(o.iter) + ")'");
            L.coll = TK::Error;  // тело этого обхода — следствие той же ошибки
            break;
        }
        // `for x in &mut outer` в теле `parallel for` — запись во внешнюю переменную.
        if (L.mut) note_write(s->iter, "mutate", false);
    }
    access_loops_.push_back(L);
}

void Checker::loop_end() {
    if (access_loops_.empty()) return;
    size_t k = access_loops_.size() - 1;
    const AccessLoop& L = access_loops_[k];
    std::vector<Step> coll;
    path(L.iter, loop_iters_, coll);
    std::vector<const Local*> reported;
    for (const LoopRead& r : loop_reads_) {
        if (r.loop != k) continue;
        std::vector<Step> steps;
        const Local* root = path(r.place, loop_iters_, steps);
        if (r.par) {
            // Р69: переменную, которую тело пишет как out[i], оно и читает
            // только так — другой элемент пишет другая итерация.
            if (!root || by_var(steps, L.var) ||
                std::find(L.written.begin(), L.written.end(), root) == L.written.end() ||
                std::find(reported.begin(), reported.end(), root) != reported.end()) {
                continue;
            }
            reported.push_back(root);
            std::string var = L.var ? L.var->name : "i";
            error(Code::E0412, r.place->span,
                  "cannot read '" + text(r.place) + "' inside 'parallel for': another iteration writes '" + root->name +
                      "' at the same time; read only this iteration's element '" + root->name + "[" + var +
                      "]', or copy what you need before the loop");
            continue;
        }
        if (!crosses(steps, coll)) continue;
        error(Code::E0412, r.place->span,
              "cannot read '" + text(r.place) + "' while 'for ... in &mut' changes it; read the value before the "
                                                "loop: 'let v = " + text(r.place) + "'");
    }
    loop_reads_.erase(std::remove_if(loop_reads_.begin(), loop_reads_.end(),
                                     [&](const LoopRead& r) { return r.loop == k; }),
                      loop_reads_.end());
    access_loops_.pop_back();
}

// --- вид левее действия

void Checker::changes(Expr* e, std::vector<Expr*>& out, bool top) const {
    e = bare(e);
    if (!e) return;
    auto place = [&](Expr* p) {
        if (raw_base(p)) out.push_back(p);
    };
    switch (e->kind) {
        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            auto* f = as<FieldExpr>(c->callee);
            if (c->call == CallKind::Method && f && c->method < Method::Contains) place(f->object);
            changes(c->callee, out);
            for (Expr* a : c->args) changes(a, out);
            return;
        }
        case ExprKind::Ref: {
            auto* r = static_cast<RefExpr*>(e);
            if (r->mut && !top) place(r->operand);
            changes(r->operand, out);
            return;
        }
        case ExprKind::Field: changes(static_cast<FieldExpr*>(e)->object, out); return;
        case ExprKind::Index:
            changes(static_cast<IndexExpr*>(e)->object, out);
            changes(static_cast<IndexExpr*>(e)->index, out);
            return;
        case ExprKind::Slice: {
            auto* s = static_cast<SliceExpr*>(e);
            changes(s->object, out);
            changes(s->lo, out);
            changes(s->hi, out);
            return;
        }
        case ExprKind::Unary: changes(static_cast<UnaryExpr*>(e)->operand, out); return;
        case ExprKind::Binary:
            changes(static_cast<BinaryExpr*>(e)->lhs, out);
            changes(static_cast<BinaryExpr*>(e)->rhs, out);
            return;
        case ExprKind::Try: changes(static_cast<TryExpr*>(e)->operand, out); return;
        case ExprKind::Spawn: changes(static_cast<SpawnExpr*>(e)->operand, out); return;
        case ExprKind::StructLit:
            for (FieldInit& f : static_cast<StructLitExpr*>(e)->fields) changes(f.value, out);
            return;
        case ExprKind::VariantLit: {
            auto* v = static_cast<VariantLitExpr*>(e);
            for (FieldInit& f : v->fields) changes(f.value, out);
            for (Expr* a : v->args) changes(a, out);
            return;
        }
        case ExprKind::ListLit:
            for (Expr* a : static_cast<ListLitExpr*>(e)->items) changes(a, out);
            return;
        default: return;
    }
}

bool Checker::view_at(Expr* place, Expr* at, const std::vector<Expr*>& right) {
    std::vector<Step> pv;
    const Local* rv = path(place, loop_iters_, pv);
    if (!rv) return false;
    for (Expr* r : right) {
        std::vector<Step> pr;
        if (path(r, loop_iters_, pr) != rv || !crosses(pv, pr)) continue;
        error(Code::E0502, at->span, "'" + text(place) + "' is viewed here and '" + text(r) +
                                         "' changes later in the same expression; copy it before: 'let old = " +
                                         text(place).substr(0, text(place).find_first_of(".[")) + "'");
        return true;
    }
    return false;
}

// Операнды по порядку: у каждого — места, которые меняют операнды правее
// (и right у родителя, который хранит их виды). Один проход с конца:
// список-литерал в сотни тысяч элементов — линейно.
void Checker::view_operands(const std::vector<Expr*>& ops, const std::vector<Expr*>& right, bool keep) {
    std::vector<Expr*> later = keep ? right : std::vector<Expr*>{};
    for (size_t i = ops.size(); i-- > 0;) {
        view_check(ops[i], later);
        changes(ops[i], later, true);
    }
}

// Вид — срез, список как срез, `&x` и `&место` — читает своё место, пока
// живёт: в операнде — до конца родителя. right — места, которые меняют
// действия правее (операнды родителя правее; у среза — ещё границы, у
// индекса — сам индекс).
void Checker::view_check(Expr* e, const std::vector<Expr*>& right) {
    e = bare(e);
    if (!e) return;
    if (e->conv && e->conv->kind == TK::Slice && e->type && (e->type->kind == TK::List || e->type->kind == TK::Array)) {
        view_at(e, e, right);
    }
    switch (e->kind) {
        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            std::vector<Expr*> ops;
            auto* f = as<FieldExpr>(c->callee);
            // Получатель метода, который не меняет его, — первый операнд.
            if (c->call == CallKind::Method && f && c->method >= Method::Contains) ops.push_back(f->object);
            ops.insert(ops.end(), c->args.begin(), c->args.end());
            view_operands(ops, {}, false);
            return;
        }
        case ExprKind::Ref: {
            auto* r = static_cast<RefExpr*>(e);
            if (!r->mut && is_place(r->operand)) view_at(r->operand, e, right);
            view_check(r->operand, r->mut ? std::vector<Expr*>{} : right);
            return;
        }
        case ExprKind::Slice: {
            auto* s = static_cast<SliceExpr*>(e);
            std::vector<Expr*> bounds, hi;
            changes(s->lo, bounds);
            changes(s->hi, bounds);
            changes(s->hi, hi);
            bool str = s->object->type && s->object->type->kind == TK::Str;
            std::vector<Expr*> need = str ? bounds : right;
            if (!str) need.insert(need.end(), bounds.begin(), bounds.end());
            if (view_at(s->object, e, need)) return;  // срез среза — одна ошибка
            std::vector<Expr*> inner = right;
            inner.insert(inner.end(), bounds.begin(), bounds.end());
            view_check(s->object, inner);
            view_check(s->lo, hi);
            view_check(s->hi, {});
            return;
        }
        case ExprKind::Index: {
            auto* i = static_cast<IndexExpr*>(e);
            std::vector<Expr*> inner = right;
            changes(i->index, inner);
            view_check(i->object, inner);
            view_check(i->index, {});
            return;
        }
        case ExprKind::Field: view_check(static_cast<FieldExpr*>(e)->object, right); return;
        case ExprKind::Unary: view_check(static_cast<UnaryExpr*>(e)->operand, {}); return;
        case ExprKind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            view_operands({b->lhs, b->rhs}, {}, false);
            return;
        }
        case ExprKind::Try: view_check(static_cast<TryExpr*>(e)->operand, right); return;
        case ExprKind::Spawn: view_check(static_cast<SpawnExpr*>(e)->operand, {}); return;
        case ExprKind::StructLit: {
            std::vector<Expr*> ops;
            for (FieldInit& f : static_cast<StructLitExpr*>(e)->fields) ops.push_back(f.value ? f.value : f.shorthand);
            view_operands(ops, right, holds_view(e->type));
            return;
        }
        case ExprKind::VariantLit: {
            auto* v = static_cast<VariantLitExpr*>(e);
            std::vector<Expr*> ops(v->args.begin(), v->args.end());
            for (FieldInit& f : v->fields) ops.push_back(f.value ? f.value : f.shorthand);
            view_operands(ops, right, false);
            return;
        }
        case ExprKind::ListLit: {
            auto* l = static_cast<ListLitExpr*>(e);
            view_operands(std::vector<Expr*>(l->items.begin(), l->items.end()), right, holds_view(e->type));
            return;
        }
        default: return;
    }
}

void Checker::views(Stmt* s) {
    switch (s->kind) {
        case StmtKind::Let: view_check(static_cast<LetStmt*>(s)->value, {}); break;
        case StmtKind::Assign: {
            auto* a = static_cast<AssignStmt*>(s);
            view_check(a->target, {});
            view_check(a->value, {});
            break;
        }
        case StmtKind::Expr: view_check(static_cast<ExprStmt*>(s)->expr, {}); break;
        case StmtKind::If: {
            auto* i = static_cast<IfStmt*>(s);
            view_check(i->cond, {});
            view_check(i->let_value, {});
            break;
        }
        case StmtKind::While: view_check(static_cast<WhileStmt*>(s)->cond, {}); break;
        case StmtKind::For: {
            auto* f = static_cast<ForStmt*>(s);
            view_check(f->iter, {});
            view_check(f->hi, {});
            break;
        }
        case StmtKind::Match: view_check(static_cast<MatchStmt*>(s)->subject, {}); break;
        case StmtKind::Return: view_check(static_cast<ReturnStmt*>(s)->value, {}); break;
        case StmtKind::Fail: view_check(static_cast<FailStmt*>(s)->value, {}); break;
        default: break;
    }
}

// --- задачи (О13, прототип): `tasks { spawn f(&x, &mut y) }`
//
// Заимствование задачей живёт до конца блока. Пока блок идёт, место
// `&mut` не трогает никто, кроме задачи, место `&` никто не меняет; код
// блока вне `spawn` — такой же участник. `spawn` в цикле блока — каждая
// итерация ставит задачу, поэтому `&mut` — элемент по переменной цикла
// (`&mut out[w]`), как в `parallel for`. Конкурентные значения (`Atomic`,
// `Mutex`, `Channel`) делятся по `&` — их правило не касается.

namespace {

// Значение с дескриптором C (`extern type`): поток его не создавал.
bool holds_handle(const Type* t, std::vector<const Type*>& seen) {
    if (!t) return false;
    if (t->kind == TK::Handle) return true;
    if (std::find(seen.begin(), seen.end(), t) != seen.end()) return false;
    seen.push_back(t);
    if (t->kind == TK::Struct && t->sdef) {
        for (const FieldDef& f : t->sdef->fields) {
            if (holds_handle(f.type, seen)) return true;
        }
        return false;
    }
    if (t->kind == TK::Enum && t->edef) {
        for (const VariantDef& v : t->edef->variants) {
            for (const Type* x : v.types) {
                if (holds_handle(x, seen)) return true;
            }
            for (const FieldDef& f : v.fields) {
                if (holds_handle(f.type, seen)) return true;
            }
        }
        return false;
    }
    return holds_handle(t->elem, seen) || holds_handle(t->key, seen) || holds_handle(t->err, seen);
}

const char* variant_name(char v) { return v == 'a' ? "A" : v == 'b' ? "B" : "C"; }

}  // namespace

bool Checker::variant_ok(char variant, Span span, const char* form) {
    if (tasks_mode_ == 0 || tasks_mode_ == variant) return true;
    if (tasks_mode_ == '?') {
        error(Code::E0519, span, std::string(form) + " needs a variant of the concurrency prototype: DPL_TASKS must "
                                                     "be 'a', 'b' or 'c'");
    } else {
        error(Code::E0519, span, std::string(form) + " is variant " + variant_name(variant) +
                                     " of the concurrency prototype; this program is checked as variant " +
                                     variant_name(tasks_mode_) + " (DPL_TASKS=" + tasks_mode_ + ")");
    }
    return false;
}

void Checker::task_loop_begin(const ForStmt* f) {
    // Разные итерации — разные элементы только у диапазона и обхода `&`/`&mut`.
    const Local* var = nullptr;
    if (f && f->local && (f->mode == ForMode::Range || f->mode == ForMode::Ref || f->mode == ForMode::Mut)) {
        var = f->local;
    }
    for (TasksScope& sc : tasks_) sc.loops.push_back(var);
}

void Checker::task_loop_end() {
    for (TasksScope& sc : tasks_) {
        if (!sc.loops.empty()) sc.loops.pop_back();
    }
}

// Задача блока: вызов c уже проверен; marks — размеры доступов областей до
// его аргументов.
void Checker::task_spawn(SpawnExpr* sp, CallExpr* c, const std::vector<size_t>& marks) {
    TasksScope& sc = tasks_.back();
    std::vector<Expr*> borrowed;
    for (Expr* a : c->args) {
        if (a->type) {
            std::vector<const Type*> seen;
            if (holds_handle(a->type, seen)) {
                error(Code::E0520, a->span, "'" + tn(a->type) + "' holds a C handle ('extern type'); a task cannot "
                                            "take it: the C library may tie it to the thread that made it");
                continue;
            }
        }
        if (a->type && holds_view(a->type)) {
            error(Code::E0520, a->span, "'" + tn(a->type) + "' views memory of the code that starts the task; pass "
                                        "the List by reference and the bounds by value");
            continue;
        }
        auto* r = as<RefExpr>(bare(a));
        if (!r) continue;
        if (!is_place(r->operand)) {  // `&S { … }`; другое отвергает E0501
            if (as<StructLitExpr>(bare(r->operand))) {
                error(Code::E0520, a->span, "a task holds its '&' arguments until the end of the block, and this "
                                            "value is temporary; store it in a variable declared before 'tasks'");
            }
            continue;
        }
        Expr* place = bare(r->operand);
        if (concurrent(place->type)) continue;
        std::vector<Step> steps;
        const Local* root = path(place, loop_iters_, steps);
        if (!root) continue;
        if (root->id >= sc.first_inner) {
            error(Code::E0520, place->span, "'" + root->name + "' is declared inside 'tasks'; a task borrows only "
                                            "variables declared before the block, which outlive its tasks");
            continue;
        }
        TaskBorrow b;
        b.place = place;
        b.mut = r->mut;
        b.spawn = sp;
        b.loops = sc.loops;
        sc.borrows.push_back(b);
        borrowed.push_back(place);
    }
    // Заимствование — не чтение и не запись кода блока; в блоках вокруг оно —
    // доступ их кода.
    std::vector<TaskAccess>& acc = sc.accesses;
    size_t from = marks.empty() ? 0 : marks.back();
    for (Expr* p : borrowed) {
        drop_task_reads(acc, p, from);
        acc.erase(std::remove_if(acc.begin() + static_cast<std::ptrdiff_t>(std::min(from, acc.size())), acc.end(),
                                 [&](const TaskAccess& x) { return x.write && bare(x.place) == p; }),
                  acc.end());
    }
    for (size_t k = 0; k + 1 < tasks_.size(); k++) {
        for (const TaskBorrow& b : sc.borrows) {
            if (b.spawn != sp) continue;
            drop_task_reads(tasks_[k].accesses, b.place, k < marks.size() ? marks[k] : 0);
            tasks_[k].accesses.push_back(TaskAccess{b.place, b.mut, false});
        }
    }
}

void Checker::tasks_end() {
    TasksScope& sc = tasks_.back();
    std::vector<const Local*> reported;
    auto once = [&](const Local* root) {
        if (std::find(reported.begin(), reported.end(), root) != reported.end()) return false;
        reported.push_back(root);
        return true;
    };
    struct Path {
        const Local* root = nullptr;
        std::vector<Step> steps;
    };
    std::vector<Path> bp(sc.borrows.size());
    for (size_t i = 0; i < sc.borrows.size(); i++) bp[i].root = path(sc.borrows[i].place, loop_iters_, bp[i].steps);
    // `&mut` в цикле блока: у каждой итерации — свой элемент.
    for (size_t i = 0; i < sc.borrows.size(); i++) {
        const TaskBorrow& b = sc.borrows[i];
        if (!b.mut) continue;
        const Local* base = raw_base(b.place);
        for (const Local* var : b.loops) {
            if (var && (by_var(bp[i].steps, var) || derived(base, var, loop_iters_))) continue;
            if (!once(bp[i].root)) break;
            std::string hint = var ? "; borrow an element indexed by the loop variable, such as '&mut out[" +
                                         var->name + "]'"
                                   : "; start one task per element in a 'for' over a range";
            error(Code::E0521, b.place->span, "every iteration of this loop starts a task with '&mut " +
                                                  text(b.place) + "': the tasks would change it at the same time" + hint);
            break;
        }
    }
    // Две задачи: одно место, хоть одна меняет его.
    for (size_t i = 0; i < sc.borrows.size(); i++) {
        for (size_t j = 0; j < i; j++) {
            const TaskBorrow& a = sc.borrows[j];
            const TaskBorrow& b = sc.borrows[i];
            if (a.spawn == b.spawn || !(a.mut || b.mut) || bp[i].root != bp[j].root || !bp[i].root) continue;
            if (!crosses(bp[i].steps, bp[j].steps) || !once(bp[i].root)) continue;
            error(Code::E0521, b.place->span,
                  "'" + text(b.place) + "' is borrowed by two tasks of this block, and one of them changes it ('&mut " +
                      text(a.mut ? a.place : b.place) + "'); give each task its own element or field");
        }
    }
    // Код блока и задача: одно место, хоть кто-то меняет его.
    for (const TaskAccess& x : sc.accesses) {
        std::vector<Step> steps;
        const Local* root = path(x.place, loop_iters_, steps);
        if (!root) continue;
        for (size_t i = 0; i < sc.borrows.size(); i++) {
            const TaskBorrow& b = sc.borrows[i];
            if (bp[i].root != root || !(x.write || b.mut) || !crosses(steps, bp[i].steps)) continue;
            // `len(xs)`, пока задачи меняют элементы: длину они не меняют.
            if (x.len && bp[i].steps.size() > steps.size()) continue;
            if (!once(root)) break;
            std::string what = x.write ? "change" : "read";
            error(Code::E0521, x.place->span,
                  "cannot " + what + " '" + text(x.place) + "' inside 'tasks': a task " +
                      (b.mut ? "changes" : "reads") + " '" + text(b.place) +
                      "' until the end of the block; prepare it before 'tasks' and use it after");
            break;
        }
    }
}

}  // namespace dp::sema
