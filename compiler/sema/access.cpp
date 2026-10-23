// Замер О4 (sema/access.h): доступы, которые отвергли бы правила-кандидаты,
// — событиями журнала вместо отказа. Ведётся только у `dpl debug access`.
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

bool within(Span a, Span b) { return a.file == b.file && a.begin >= b.begin && a.end <= b.end; }

void push(AccessLog& log, const char* rule, const char* kind, Span span, Span other, const Local* root,
          std::string detail) {
    AccessEvent e;
    e.rule = rule;
    e.kind = kind;
    e.span = span;
    e.other = other;
    e.root = root ? root->name : "";
    e.detail = std::move(detail);
    log.events.push_back(std::move(e));
}

std::string loop_detail(const char* coll, bool mut) { return std::string(coll) + (mut ? " mut" : ""); }

}  // namespace

void Checker::note_write(Expr* place, Write how) {
    AccessLog* log = probe();
    if (!log || probe_loops_.empty() || !place) return;
    std::vector<Step> steps;
    const Local* root = path(place, probe_iter_, steps);
    const Local* base = raw_base(place);
    if (!root || !base) return;
    // Цель записи при выводе её типа прочитана — это не чтение корня.
    auto& ev = log->events;
    ev.erase(std::remove_if(ev.begin(), ev.end(),
                            [&](const AccessEvent& e) {
                                return (e.kind == "read" || e.kind == "read_root") && within(e.span, place->span);
                            }),
             ev.end());
    // Длина или ключи корня: метод формы или `&mut` над корнем целиком,
    // присваивание корню.
    bool structural = steps.empty() && how != Write::Method;
    for (const ProbeLoop& L : probe_loops_) {
        if (derived(base, L.var, probe_iter_)) continue;
        if (L.root && root == L.root) {
            push(*log, "loop", structural ? "structural" : "element", place->span, L.head, root,
                 loop_detail(L.coll, L.mut));
        }
        if (!L.parallel || base->id >= L.first_inner) continue;
        // Запись во внешнюю переменную из тела `parallel for` (развилка 6).
        const char* kind = "var";
        if (steps.empty()) {
            if (how == Write::Shape) kind = "structural";
        } else {
            bool by_var = false, indexed = false;
            for (const Step& s : steps) {
                if (s.kind == Step::Field) continue;
                indexed = true;
                auto* n = s.index ? as<NameExpr>(const_cast<Expr*>(s.index)) : nullptr;
                if (n && n->res.kind == ResKind::Local && n->res.local == L.var) by_var = true;
            }
            if (by_var) kind = "index_loopvar";
            else if (indexed) kind = "index_computed";
        }
        push(*log, "par", kind, place->span, L.head, root, L.coll);
    }
}

void Checker::note_use(NameExpr* n) {
    AccessLog* log = probe();
    if (!log || probe_loops_.empty() || n->res.kind != ResKind::Local) return;
    for (const ProbeLoop& L : probe_loops_) {
        if (!L.mut || L.root != n->res.local) continue;
        push(*log, "loop", "read", n->span, L.head, L.root, loop_detail(L.coll, true));
        // Тело `parallel for x in &mut xs` читает xs, который пишут другие итерации.
        if (L.parallel) push(*log, "par", "read_root", n->span, L.head, L.root, L.coll);
    }
}

void Checker::probe_loop_begin(ForStmt* s) {
    AccessLog* log = probe();
    if (!log) return;
    ProbeLoop L;
    L.var = s->local;
    L.mut = s->mode == ForMode::Mut;
    L.parallel = s->parallel;
    L.first_inner = s->local ? s->local->id : next_local_;
    L.head = s->iter ? s->iter->span : s->var_span;
    L.coll = "range";
    if ((s->mode == ForMode::Ref || L.mut) && s->iter) {
        std::vector<Step> steps;
        L.root = path(s->iter, probe_iter_, steps);
        const Type* t = s->iter->type;
        L.coll = !t ? "?" : t->kind == TK::List ? "list" : t->kind == TK::Map ? "map" : t->kind == TK::Str ? "str" : "slice";
        if (s->local) probe_iter_[s->local] = s->iter;
        // Второй обход того же корня, если хоть один из двух меняет его.
        const Local* base = raw_base(s->iter);
        for (const ProbeLoop& o : probe_loops_) {
            if (!o.root || o.root != L.root || !(o.mut || L.mut) || derived(base, o.var, probe_iter_)) continue;
            push(*log, "loop", "nested", s->iter->span, o.head, L.root,
                 loop_detail(o.coll, o.mut) + (L.mut ? ", inner mut" : ", inner read"));
        }
        // `for x in &mut outer` в теле `parallel for` — запись во внешнюю переменную.
        if (L.mut) {
            for (const ProbeLoop& o : probe_loops_) {
                if (o.parallel && base && base->id < o.first_inner && !derived(base, o.var, probe_iter_)) {
                    push(*log, "par", steps.empty() ? "var" : "index_computed", s->iter->span, o.head, L.root, o.coll);
                }
            }
        }
    }
    probe_loops_.push_back(L);
}

void Checker::probe_loop_end() {
    if (probe() && !probe_loops_.empty()) probe_loops_.pop_back();
}

bool Checker::probe_call(Expr* a, bool a_mut, bool a_view, Expr* b, bool b_mut, bool b_view) {
    AccessLog* log = probe();
    if (!log) return false;
    std::vector<Step> pa, pb;
    const Local* root = path(a, probe_iter_, pa);
    path(b, probe_iter_, pb);
    // Первое расхождение путей решает, пересекаются ли места.
    const char* rel = "overlap";
    for (size_t i = 0; i < pa.size() && i < pb.size(); i++) {
        if (pa[i].kind != pb[i].kind || pa[i].kind == Step::Index || pa[i].kind == Step::Slice) {
            rel = "computed_index";
            break;
        }
        if (pa[i].value != pb[i].value) {
            rel = pa[i].kind == Step::Field ? "fields" : "const_index";
            break;
        }
    }
    const char* mode = a_mut && b_mut ? "mut_mut" : a_view || b_view ? "mut_view" : "mut_ref";
    push(*log, "call", rel, b ? b->span : Span{}, a ? a->span : Span{}, root, mode);
    return true;
}

}  // namespace dp::sema
