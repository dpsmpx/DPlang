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

// Что обходит цикл — для сообщения.
std::string over(TK coll) {
    if (coll == TK::Map) return "its Map; iterate over a copy of the keys: 'for k in &m.keys()'";
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

void Checker::note_write(Expr* place, Write how, const char* why, bool loops) {
    if (access_loops_.empty() || !place) return;
    std::vector<Step> steps;
    const Local* root = path(place, loop_iters_, steps);
    const Local* base = raw_base(place);
    if (!root || !base) return;
    drop_reads(loop_reads_, place);  // цель записи при выводе её типа прочитана
    std::string head = std::string("cannot ") + why + " '" + text(place) + "'";
    for (const AccessLoop& L : access_loops_) {
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
        // цикла (`out[i]`): итерации идут одновременно.
        bool by_var = false;
        for (const Step& s : steps) {
            auto* n = s.kind == Step::Index ? as<NameExpr>(const_cast<Expr*>(s.index)) : nullptr;
            if (n && n->res.kind == ResKind::Local && n->res.local == L.var) by_var = true;
        }
        if (by_var && how != Write::Shape) continue;
        std::string var = L.var ? L.var->name : "i";
        error(Code::E0412, place->span,
              head + " inside 'parallel for': iterations run at the same time; write each result to an element "
                     "indexed by the loop variable, such as 'out[" + var + "]', and combine them after the loop");
        return;
    }
}

void Checker::note_use(NameExpr* n) {
    if (access_loops_.empty() || n->res.kind != ResKind::Local) return;
    for (size_t k = 0; k < access_loops_.size(); k++) {
        if (access_loops_[k].mut && access_loops_[k].root == n->res.local) loop_reads_.push_back(LoopRead{n, k});
    }
}

void Checker::note_extend(Expr* object, Expr* place) {
    for (LoopRead& r : loop_reads_) {
        if (r.place == object) r.place = place;
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
        if (k == TK::List || k == TK::Map || k == TK::Slice) L.coll = k;
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
        if (L.mut) note_write(s->iter, Write::Ref, "mutate", false);
    }
    access_loops_.push_back(L);
}

void Checker::loop_end() {
    if (access_loops_.empty()) return;
    size_t k = access_loops_.size() - 1;
    const AccessLoop& L = access_loops_[k];
    std::vector<Step> coll;
    path(L.iter, loop_iters_, coll);
    for (const LoopRead& r : loop_reads_) {
        if (r.loop != k) continue;
        std::vector<Step> steps;
        path(r.place, loop_iters_, steps);
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

void Checker::view_operands(const std::vector<Expr*>& ops, const std::vector<Expr*>& right, bool keep) {
    for (size_t i = 0; i < ops.size(); i++) {
        std::vector<Expr*> later = keep ? right : std::vector<Expr*>{};
        for (size_t j = i + 1; j < ops.size(); j++) changes(ops[j], later, true);
        view_check(ops[i], later);
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

}  // namespace dp::sema
