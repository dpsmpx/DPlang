// Проверка выражений: имена, типы, преобразования, вызовы, места,
// константы (docs/spec/core.md, разделы 4, 5, 8, 9).
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <cstdio>

#include "sema/builtins.h"
#include "sema/checker.h"
#include "support/format.h"

namespace dp::sema {

using namespace ast;

namespace {

// Целое значение (модуль v и знак) помещается в целый тип t.
bool fits(uint64_t v, bool negative, const Type* t) {
    int bits = int_bits(t);
    if (is_signed(t)) {
        uint64_t lim = 1ull << (bits - 1);  // |min|; max = lim - 1
        return negative ? v <= lim : v < lim;
    }
    if (negative) return v == 0;
    return bits == 64 || v < (1ull << bits);
}

const Type* unwrap_hint(const Type* h) {
    while (h && (h->kind == TK::Optional || h->kind == TK::Result)) h = h->elem;
    return h;
}

const char* bin_text(BinOp op) {
    switch (op) {
        case BinOp::Or: return "||";
        case BinOp::And: return "&&";
        case BinOp::Eq: return "==";
        case BinOp::Ne: return "!=";
        case BinOp::Lt: return "<";
        case BinOp::Le: return "<=";
        case BinOp::Gt: return ">";
        case BinOp::Ge: return ">=";
        case BinOp::Coalesce: return "??";
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::BitOr: return "|";
        case BinOp::BitXor: return "^";
        case BinOp::AddWrap: return "+%";
        case BinOp::SubWrap: return "-%";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "/";
        case BinOp::Rem: return "%";
        case BinOp::Shl: return "<<";
        case BinOp::Shr: return ">>";
        case BinOp::BitAnd: return "&";
        case BinOp::MulWrap: return "*%";
    }
    return "?";
}

struct MethodInfo {
    const char* name;
    Method m;
};

constexpr MethodInfo kMethods[] = {
    {"push", Method::Push},   {"pop", Method::Pop},           {"insert", Method::Insert},
    {"remove", Method::Remove}, {"clear", Method::Clear},     {"sort", Method::Sort},
    {"reserve", Method::Reserve}, {"add", Method::Add}, {"handles", Method::Handles},
    {"contains", Method::Contains}, {"trim", Method::Trim},   {"find", Method::Find},
    {"starts_with", Method::StartsWith}, {"split", Method::Split}, {"get", Method::Get},
    {"keys", Method::Keys}, {"load", Method::Load}, {"store", Method::Store}, {"sub", Method::Sub},
    {"swap", Method::Swap}, {"cas", Method::Cas}, {"send", Method::Send}, {"recv", Method::Recv},
    {"try_recv", Method::TryRecv}, {"close", Method::Close},
};

}  // namespace

static Expr* strip_paren(Expr* e) {
    while (auto* p = as<ParenExpr>(e)) e = p->inner;
    return e;
}

// --- преобразования

bool Checker::convertible(const Type* from, const Type* to) {
    if (from == to || !from || !to) return true;
    if (from->kind == TK::Error || to->kind == TK::Error) return true;
    if (is_integer(from) && is_integer(to)) {
        bool sf = is_signed(from), st = is_signed(to);
        int bf = int_bits(from), bt = int_bits(to);
        if (sf == st) return bf <= bt;
        return !sf && st && bf < bt;
    }
    if (from->kind == TK::F32 && to->kind == TK::F64) return true;
    if (from->kind == TK::Ref && to->kind == TK::Ref) return c_prefix(to->elem, from->elem);  // Р62: общее начало
    if ((from->kind == TK::List || from->kind == TK::Array) && to->kind == TK::Slice) return from->elem == to->elem;
    if (to->kind == TK::Optional && from->kind != TK::Optional) return convertible(from, to->elem);
    if (to->kind == TK::Result && from->kind != TK::Result) return convertible(from, to->elem);
    return false;
}

void Checker::coerce(Expr* e, const Type* want) {
    if (!e || !want || !e->type || e->type == want) return;
    if (convertible(e->type, want)) {
        if (e->type->kind != TK::Error && want->kind != TK::Error) e->conv = want;
        return;
    }
    error(Code::E0401, e->span, "expected '" + tn(want) + "', found '" + tn(e->type) + "'");
}

const Type* Checker::check(Expr* e, const Type* want) {
    if (!e) return want ? want : err();
    infer(e, want);
    coerce(e, want);
    return want ? want : e->type;
}

// Выражение, тип которого берётся из контекста: литерал, `none`, `.V`, `[]`.
bool Checker::literal_like(const Expr* e) const {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Int: case ExprKind::Float: case ExprKind::None: case ExprKind::VariantLit: return true;
        case ExprKind::ListLit: return static_cast<const ListLitExpr*>(e)->items.empty();
        case ExprKind::Unary: return literal_like(static_cast<const UnaryExpr*>(e)->operand);
        case ExprKind::Paren: return literal_like(static_cast<const ParenExpr*>(e)->inner);
        case ExprKind::Binary: {
            auto* b = static_cast<const BinaryExpr*>(e);
            return b->op != BinOp::Eq && b->op != BinOp::Ne && b->op != BinOp::Lt && b->op != BinOp::Le &&
                   b->op != BinOp::Gt && b->op != BinOp::Ge && literal_like(b->lhs) && literal_like(b->rhs);
        }
        default: return false;
    }
}

// `==` и `<` у типа (expr.compare). Рекурсивный тип (List[Node] в Node,
// type.recursive) в процессе обхода считается подходящим — наибольшая
// неподвижная точка. Ответ запоминается: «нет» — всегда, «да» — если он
// не опирался на внешний тип, который ещё проверяется. Так обход
// линеен и на «ромбах» `struct S0 { a: S1, b: S1 }`. Обход — явным стеком:
// глубина не растёт с длиной цепочки структур.
bool Checker::type_prop(const Type* root, bool ord) {
    std::map<const Type*, bool>& cache = prop_cache_[ord ? 1 : 0];
    struct Frame {
        const Type* t;
        std::vector<const Type*> parts;
        size_t next = 0;
        size_t low = SIZE_MAX;  // самый внешний тип стека, на который опёрся ответ
        bool r = true;
    };
    std::vector<Frame> st;
    std::unordered_map<const Type*, size_t> at;  // тип на стеке → его номер
    size_t outer = SIZE_MAX;
    // Ответ для части p: 0 или 1 — сразу, 2 — тип положен на стек.
    auto visit = [&](const Type* p, size_t& low) -> int {
        for (;;) {
            switch (p->kind) {
                case TK::Error: return 1;
                // Р54: отображения не сравниваются; указатель C (Р61), массив и ссылка-поле C (Р62) — тоже.
                case TK::Void: case TK::Result: case TK::Map: case TK::Pool: case TK::CFn: case TK::Array: case TK::Ref:
                case TK::Atomic: case TK::Mutex: case TK::Channel:
                    return 0;
                case TK::PoolHandle: return ord ? 0 : 1;  // Р67: хэндлы сравниваются ==, но не упорядочены
                case TK::Bool: case TK::Str: return 1;
                case TK::List: case TK::Slice: p = p->elem; continue;
                case TK::Optional:  // ?T не упорядочен
                    if (ord) return 0;
                    p = p->elem;
                    continue;
                case TK::Struct: case TK::Enum: break;
                default: return ord ? is_numeric(p) : 1;
            }
            break;
        }
        if (auto it = cache.find(p); it != cache.end()) return it->second ? 1 : 0;
        if (auto it = at.find(p); it != at.end()) {
            low = std::min(low, it->second);
            return 1;
        }
        Frame f{p, {}};
        if (p->kind == TK::Struct) {
            if (p->sdef->linear || p->sdef->union_) f.r = false;  // Р60: линейные; Р62: у объединения — байты
            for (const FieldDef& fd : p->sdef->fields) f.parts.push_back(fd.type);
        } else {
            for (const VariantDef& v : p->edef->variants) {
                for (const Type* x : v.types) f.parts.push_back(x);
                for (const FieldDef& fd : v.fields) f.parts.push_back(fd.type);
            }
        }
        at[p] = st.size();
        st.push_back(std::move(f));
        return 2;
    };
    int v = visit(root, outer);
    if (v != 2) return v == 1;
    for (;;) {
        size_t i = st.size() - 1;
        if (st[i].r && st[i].next < st[i].parts.size()) {
            const Type* p = st[i].parts[st[i].next++];
            size_t low = st[i].low;
            int r = visit(p, low);  // может положить кадр: st[i] — по номеру
            st[i].low = low;
            if (r == 0) st[i].r = false;
            continue;
        }
        Frame done = std::move(st.back());
        st.pop_back();
        at.erase(done.t);
        size_t depth = st.size();
        if (!done.r || done.low >= depth) cache[done.t] = done.r;
        size_t& low = st.empty() ? outer : st.back().low;
        if (done.r && done.low < depth) low = std::min(low, done.low);
        if (st.empty()) return done.r;
        if (!done.r) st.back().r = false;
    }
}

bool Checker::equatable(const Type* t) { return type_prop(t, false); }

bool Checker::ordered(const Type* t) { return type_prop(t, true); }

// --- имена и пути

void Checker::resolve_name(NameExpr* n) {
    if (n->res.kind != ResKind::None || n->type) return;  // уже разрешено (или уже сообщено)
    if (n->name == "_") {
        error(Code::E0307, n->span, "'_' is not a value; it can only discard one: '_ = e'");
        n->type = err();
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
            case SymKind::Handle:  // `extern type` — только тип (Р59)
                error(Code::E0405, n->span, "type '" + n->name + "' is not a value");
                n->type = err();
                break;
        }
        return;
    }
    int b = find_builtin_fn(n->name);
    if (b >= 0 && builtin_internal(b) && !m_.stdlib) b = -1;  // `__…` — только стандартной библиотеке (Р51)
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
    error(Code::E0307, n->span, "cannot find '" + n->name + "' in this scope");
    n->type = err();
}

// Разрешает путь `m.x`, `E.V`, `m.E.V`; значение объекта не проверяет.
void Checker::resolve_field(FieldExpr* f) {
    if (f->res.kind != ResKind::None) return;
    Res* obj = nullptr;
    if (auto* n = as<NameExpr>(f->object)) {
        resolve_name(n);
        obj = &n->res;
    } else if (auto* inner = as<FieldExpr>(f->object)) {
        resolve_field(inner);
        obj = &inner->res;
    }
    if (!obj) return;
    if (obj->kind == ResKind::Module) {
        const Symbol* s = r_.member(*obj->mod, f->name, f->name_span);
        if (!s) {
            f->type = err();
            return;
        }
        switch (s->kind) {
            case SymKind::Fn: f->res.kind = ResKind::Fn; f->res.fn = s->fn; break;
            case SymKind::Const: f->res.kind = ResKind::Const; f->res.cn = s->cn; break;
            case SymKind::Struct: f->res.kind = ResKind::Struct; f->res.st = s->st; break;
            case SymKind::Enum: f->res.kind = ResKind::Enum; f->res.en = s->en; break;
            case SymKind::Module: break;
            case SymKind::Handle:
                error(Code::E0405, f->span, "type '" + f->name + "' is not a value");
                f->type = err();
                break;
        }
    } else if (obj->kind == ResKind::Enum) {
        int v = obj->en->find(f->name);
        if (v < 0) {
            error(Code::E0312, f->name_span, "enum '" + obj->en->name + "' has no variant '" + f->name + "'");
            f->type = err();
            return;
        }
        f->res.kind = ResKind::Variant;
        f->res.en = obj->en;
        f->res.index = static_cast<uint32_t>(v);
    }
}

const Type* Checker::name_expr(NameExpr* n) {
    resolve_name(n);
    if (n->type) return n->type;
    switch (n->res.kind) {
        case ResKind::Local: {
            Local* l = n->res.local;
            note_use(n);
            if (l && std::find(locked_.begin(), locked_.end(), l) != locked_.end()) {  // О13, прототип
                error(Code::E0412, n->span, "'" + l->name + "' is locked by this 'lock' block; use its data through "
                                                "the block's variable");
            }
            if (fn_ && flow_.live && l->id < flow_.init.size() && !flow_.init[l->id]) {
                error(Code::E0503, n->span, "'" + l->name + "' is used before it has a value");
                flow_.init[l->id] = 1;
            }
            return l->type;
        }
        case ResKind::Const: {
            ConstDef& c = const_cast<ConstDef&>(*n->res.cn);
            if (c.module == &m_) const_value(c);
            return c.type ? c.type : err();
        }
        case ResKind::Fn: return fn_value(*n->res.fn, n->span);
        case ResKind::BuiltinFn:
            error(Code::E0405, n->span, "built-in '" + n->name + "' is not a value; call it: " + n->name + "(...)");
            return err();
        case ResKind::Struct: case ResKind::Enum: case ResKind::BuiltinType:
            error(Code::E0405, n->span, "type '" + n->name + "' is not a value");
            return err();
        case ResKind::Module:
            error(Code::E0405, n->span, "module '" + n->name + "' is not a value");
            return err();
        default: return err();
    }
}

const Type* Checker::field_expr(FieldExpr* f, const Type* /*hint*/) {
    resolve_field(f);
    if (f->type) return f->type;
    switch (f->res.kind) {
        case ResKind::Variant: {
            const VariantDef& v = f->res.en->variants[f->res.index];
            if (v.form != VariantForm::Unit) {
                error(Code::E0414, f->span, "variant '" + v.name + "' carries data: " +
                                                (v.form == VariantForm::Tuple ? f->res.en->name + "." + v.name + "(...)"
                                                                              : f->res.en->name + "." + v.name + " { ... }"));
            }
            return tt_.enum_type(f->res.en);
        }
        case ResKind::Const: {
            ConstDef& c = const_cast<ConstDef&>(*f->res.cn);
            if (c.module == &m_) const_value(c);
            return c.type ? c.type : err();
        }
        case ResKind::Fn: return fn_value(*f->res.fn, f->span);
        case ResKind::Struct: case ResKind::Enum:
            error(Code::E0405, f->span, "type '" + f->name + "' is not a value");
            return err();
        default: break;
    }
    // Объект — тип или модуль без такого члена: сообщение уже было или будет здесь.
    if (auto* n = as<NameExpr>(f->object)) {
        if (n->res.kind == ResKind::Struct || n->res.kind == ResKind::BuiltinType) {
            error(Code::E0406, f->name_span, "type '" + n->name + "' has no member '" + f->name + "'");
            return err();
        }
        if (n->res.kind == ResKind::Module) return err();
    }
    const Type* t = infer(f->object, nullptr);
    if (t->kind == TK::Error) return err();
    if (t->kind == TK::Struct) {
        const StructDef* s = t->sdef;
        for (size_t i = 0; i < s->fields.size(); i++) {
            if (s->fields[i].name == f->name) {
                f->field = static_cast<int>(i);
                note_extend(f->object, f);
                // Р62: заимствованное поле пишется для C; в dpl его не прочесть.
                if (c_borrowing(t) && c_borrow_field(s->fields[i].type)) {
                    error(Code::E0328, f->name_span, "field '" + f->name + "' of '" + tn(t) +
                                                         "' is borrowed for C and cannot be read in dpl");
                    return err();
                }
                return s->fields[i].type;
            }
        }
        error(Code::E0406, f->name_span, "struct '" + tn(t) + "' has no field '" + f->name + "'");
        return err();
    }
    if (t->kind == TK::PoolHandle && (f->name == "index" || f->name == "generation")) {  // Р67
        f->field = f->name == "index" ? 0 : 1;
        note_extend(f->object, f);
        return prim(TK::U32);
    }
    error(Code::E0406, f->name_span, "'" + tn(t) + "' has no field '" + f->name + "'");
    return err();
}

// --- литералы и операторы

const Type* Checker::int_lit(IntExpr* e, const Type* hint, bool negative, Span span) {
    const Type* h = unwrap_hint(hint);
    if (h && is_float(h)) return h;
    const Type* t = h && is_integer(h) ? h : prim(TK::I64);
    if (!fits(e->value, negative, t)) {
        std::string lit = (negative ? "-" : "") + std::to_string(e->value);
        if (h && is_integer(h)) {
            error(Code::E0402, span, "literal " + lit + " does not fit type '" + tn(t) + "'");
        } else {
            error(Code::E0402, span, "literal " + lit + " does not fit 'i64'; give it a type, e.g. u64(" + lit + ")");
        }
        return err();
    }
    return t;
}

const Type* Checker::unary(UnaryExpr* u, const Type* hint) {
    switch (u->op) {
        case UnOp::Neg: {
            Expr* op = u->operand;
            while (auto* p = as<ParenExpr>(op)) op = p->inner;
            if (auto* lit = as<IntExpr>(op)) {
                const Type* t = int_lit(lit, hint, true, u->span);
                for (Expr* x = u->operand; x;) {  // литерал и скобки — того же типа
                    x->type = t;
                    auto* p = as<ParenExpr>(x);
                    x = p ? p->inner : nullptr;
                }
                return t;
            }
            const Type* t = infer(u->operand, hint);
            if (t->kind == TK::Error) return t;
            if ((is_integer(t) && is_signed(t)) || is_float(t)) return t;
            error(Code::E0403, u->span, "unary '-' needs a signed integer or a float, found '" + tn(t) + "'");
            return err();
        }
        case UnOp::Not:
            check(u->operand, prim(TK::Bool));
            return prim(TK::Bool);
        case UnOp::BitNot: {
            const Type* t = infer(u->operand, hint);
            if (t->kind == TK::Error || is_integer(t)) return t;
            error(Code::E0403, u->span, "'~' needs an integer, found '" + tn(t) + "'");
            return err();
        }
    }
    return err();
}

// Общий тип операндов: тот, к которому другой преобразуется неявно.
// Сторона, которой нужен контекст (литерал, `.V`, `none`), проверяется второй.
const Type* Checker::common(Expr* lhs, Expr* rhs, const Type* hint, Span op_span, const char* op) {
    const Type* lt;
    const Type* rt;
    if (literal_like(lhs) && !literal_like(rhs)) {
        rt = infer(rhs, hint);
        lt = infer(lhs, rt->kind == TK::Error ? hint : rt);
    } else {
        lt = infer(lhs, hint);
        rt = infer(rhs, lt->kind == TK::Error ? hint : lt);
    }
    const Type* t;
    if (lt->kind == TK::Error || rt->kind == TK::Error) return err();
    if (lt == rt) t = lt;
    else if (convertible(lt, rt)) t = rt;
    else if (convertible(rt, lt)) t = lt;
    else {
        error(Code::E0401, op_span, std::string("mismatched types '") + tn(lt) + "' and '" + tn(rt) + "' in '" + op +
                                        "'; convert one side explicitly");
        return err();
    }
    coerce(lhs, t);
    coerce(rhs, t);
    return t;
}

const Type* Checker::binary(BinaryExpr* b, const Type* hint) {
    const char* op = bin_text(b->op);
    switch (b->op) {
        case BinOp::And: case BinOp::Or:
            check(b->lhs, prim(TK::Bool));
            check(b->rhs, prim(TK::Bool));
            b->operand = prim(TK::Bool);
            return prim(TK::Bool);
        case BinOp::Eq: case BinOp::Ne: case BinOp::Lt: case BinOp::Le: case BinOp::Gt: case BinOp::Ge: {
            const Type* t = common(b->lhs, b->rhs, nullptr, b->op_span, op);
            b->operand = t;
            bool eq = b->op == BinOp::Eq || b->op == BinOp::Ne;
            if (t->kind != TK::Error && (eq ? !equatable(t) : !ordered(t))) {
                error(Code::E0422, b->op_span, std::string("values of type '") + tn(t) + "' cannot be compared with '" +
                                                    op + "'");
            }
            return prim(TK::Bool);
        }
        case BinOp::Coalesce: {
            const Type* lt = infer(b->lhs, hint ? tt_.optional(hint) : nullptr);
            b->operand = lt;
            if (lt->kind == TK::Error) {
                infer(b->rhs, hint);
                return err();
            }
            if (lt->kind != TK::Optional) {
                error(Code::E0403, b->op_span, "'?" "?' needs an optional value '?T' on the left, found '" + tn(lt) + "'");
                infer(b->rhs, hint);
                return err();
            }
            check(b->rhs, lt->elem);
            return lt->elem;
        }
        case BinOp::Shl: case BinOp::Shr: {
            const Type* lt = infer(b->lhs, hint);
            const Type* rt = infer(b->rhs, nullptr);
            b->operand = lt;
            if (lt->kind == TK::Error || rt->kind == TK::Error) return err();
            if (!is_integer(lt) || !is_integer(rt)) {
                error(Code::E0403, b->op_span, std::string("'") + op + "' needs integers, found '" + tn(lt) + "' and '" +
                                                    tn(rt) + "'");
                return err();
            }
            return lt;
        }
        default: break;
    }
    const Type* t = common(b->lhs, b->rhs, hint, b->op_span, op);
    b->operand = t;
    if (t->kind == TK::Error) return t;
    bool ok;
    switch (b->op) {
        case BinOp::Add: ok = is_numeric(t) || t->kind == TK::Str; break;
        case BinOp::Sub: case BinOp::Mul: case BinOp::Div: ok = is_numeric(t); break;
        default: ok = is_integer(t); break;  // % & | ^ +% -% *%
    }
    if (!ok) {
        error(Code::E0403, b->op_span, std::string("'") + op + "' does not apply to '" + tn(t) + "'");
        return err();
    }
    return t;
}

// --- вызовы

const Type* Checker::call(CallExpr* c, const Type* hint) {
    if (auto* n = as<NameExpr>(c->callee)) {
        resolve_name(n);
        switch (n->res.kind) {
            case ResKind::Fn: return user_call(c, *n->res.fn);
            case ResKind::BuiltinFn: return builtin_call(c, n->res.index, hint);
            case ResKind::BuiltinType: {
                TK k = kBuiltinTypes[n->res.index].kind;
                if (k == TK::List || k == TK::Void) break;
                return convert_call(c, prim(k), false);
            }
            case ResKind::Struct:
                error(Code::E0409, c->callee->span, "a struct is built with a literal: " + n->name + " { ... }");
                infer_loose(c->args);
                return err();
            case ResKind::None:
                infer_loose(c->args);
                return err();
            case ResKind::Local:
                if (n->res.local->type->kind == TK::CFn) return ptr_call(c, *n->res.local);
                [[fallthrough]];
            case ResKind::Const: case ResKind::Enum: case ResKind::Module:
                error(Code::E0409, c->callee->span, "'" + n->name + "' is not a function");
                infer_loose(c->args);
                return err();
            default: break;
        }
    } else if (auto* f = as<FieldExpr>(c->callee)) {
        resolve_field(f);
        if (f->res.kind == ResKind::Fn) return user_call(c, *f->res.fn);
        if (f->res.kind == ResKind::Variant) {
            c->call = CallKind::Variant;
            c->en = f->res.en;
            c->variant = f->res.index;
            return variant_value(f->res.en, f->res.index, VariantForm::Tuple, &c->args, nullptr, c->span);
        }
        if (f->type && f->type->kind == TK::Error) {
            infer_loose(c->args);
            return err();
        }
        if (auto* obj = as<NameExpr>(f->object)) {
            if (obj->res.kind == ResKind::BuiltinType) {
                TK k = kBuiltinTypes[obj->res.index].kind;
                if (f->name == "wrap" && k >= TK::I8 && k <= TK::U64) return convert_call(c, prim(k), true);
                error(Code::E0409, f->span, "type '" + obj->name + "' has no function '" + f->name +
                                                 "'; integer types have '" + obj->name + ".wrap(x)'");
                infer_loose(c->args);
                return err();
            }
            if (obj->res.kind == ResKind::Module || obj->res.kind == ResKind::Struct ||
                obj->res.kind == ResKind::Enum) {
                if (f->res.kind == ResKind::None && obj->res.kind != ResKind::Module) {
                    error(Code::E0409, f->span, "'" + obj->name + "." + f->name + "' cannot be called");
                } else if (f->res.kind != ResKind::None) {
                    error(Code::E0409, f->span, "'" + f->name + "' is not a function");
                }
                infer_loose(c->args);
                return err();
            }
        }
        return method_call(c, f);
    }
    error(Code::E0409, c->callee->span, "this expression cannot be called");
    infer_loose(c->args);
    return err();
}

// Аргументы вызова, который не разрешился или не сошёлся по числу
// аргументов: типы для дальнейших проверок, без сообщений о пометках `&` —
// они следствие первой ошибки.
void Checker::infer_loose(const std::vector<Expr*>& args) {
    for (Expr* a : args) {
        if (auto* r = as<RefExpr>(a)) r->type = infer(r->operand, nullptr);
        else infer(a, nullptr);
    }
}

const Type* Checker::user_call(CallExpr* c, const FnDef& f) {
    c->call = CallKind::Fn;
    c->fn = &f;
    if (fn_ && is_det(*fn_) && !is_det(f)) {  // класс D вызывает только класс D (Р55)
        std::string who = "'" + fn_->name + "' is class D and cannot ";
        if (f.module->stdlib) {  // os и другие модули с вводом-выводом
            error(Code::E0425, c->span, "'" + f.module->name + "." + f.name + "' does input or output; " + who +
                                            "use it. Return the value and use it outside class D");
        } else if (f.module == fn_->module) {
            error(Code::E0425, c->span, "'" + f.name + "' is not deterministic; " + who + "call it. Mark it '@det'");
        } else {
            error(Code::E0425, c->span, "'" + f.module->name + "." + f.name + "' is not deterministic; " + who +
                                            "call it. Mark it '@det' or import its module with '@det import " +
                                            f.module->name + "'");
        }
    }
    if (c->args.size() != f.params.size()) {
        error(Code::E0407, c->span, "'" + f.name + "' takes " + std::to_string(f.params.size()) + " argument" +
                                        (f.params.size() == 1 ? "" : "s") + ", found " + std::to_string(c->args.size()));
        infer_loose(c->args);
        return f.ret ? f.ret : err();
    }
    check_args(c, f.params);
    ref_args(c, f.params);
    return f.ret ? f.ret : err();
}

const Type* Checker::fn_value(const FnDef& f, Span span) {
    if (!f.exported) {
        error(Code::E0405, span, "function '" + f.name + "' is not a value; call it: " + f.name +
                                     "(...), or mark it '@export' to pass it to C");
        return err();
    }
    CFnSig sig;
    for (const ParamDef& p : f.params) sig.params.emplace_back(p.mode, p.type);
    sig.ret = f.ret ? f.ret : prim(TK::Void);
    return tt_.cfn(sig);
}

// Вызов через указатель на функцию (Р61): это вызов C — не из класса D.
const Type* Checker::ptr_call(CallExpr* c, const Local& callee) {
    c->call = CallKind::Ptr;
    infer(c->callee, nullptr);  // чтение переменной: тип и определённая инициализация
    const CFnSig& sig = *callee.type->sig;
    if (fn_ && is_det(*fn_)) {
        error(Code::E0425, c->span, "'" + callee.name + "' calls C through a pointer; '" + fn_->name +
                                        "' is class D and cannot call C");
    }
    if (c->args.size() != sig.params.size()) {
        error(Code::E0407, c->span, "'" + callee.name + "' takes " + std::to_string(sig.params.size()) + " argument" +
                                        (sig.params.size() == 1 ? "" : "s") + ", found " + std::to_string(c->args.size()));
        infer_loose(c->args);
        return sig.ret;
    }
    std::vector<ParamDef> params;
    for (size_t i = 0; i < sig.params.size(); i++) {
        params.push_back(ParamDef{"#" + std::to_string(i + 1), sig.params[i].first, sig.params[i].second, callee.span});
    }
    check_args(c, params);
    ref_args(c, params);
    return sig.ret;
}

void Checker::check_args(CallExpr* c, const std::vector<ParamDef>& params) {
    for (size_t i = 0; i < c->args.size(); i++) {
        Expr* a = c->args[i];
        const ParamDef& p = params[i];
        auto* r = as<RefExpr>(a);
        // Р62: параметр `?&T` функции C — `&x` как значение.
        bool ref_value = p.type && (p.type->kind == TK::Ref ||
                                    (p.type->kind == TK::Optional && p.type->elem->kind == TK::Ref));
        if (p.mode == ParamMode::Value) {
            if (r && !ref_value) {
                error(Code::E0408, a->span, "parameter '" + p.name + "' takes a value; remove '" +
                                                std::string(r->mut ? "&mut" : "&") + "'");
                infer(r->operand, p.type);
                r->type = err();
                continue;
            }
            check(a, p.type);
            continue;
        }
        const char* want = p.mode == ParamMode::Mut ? "&mut" : "&";
        if (!r) {
            error(Code::E0408, a->span, "parameter '" + p.name + "' is '" + want + "': pass " + want + " " +
                                            (as<NameExpr>(a) ? static_cast<NameExpr*>(a)->name : std::string("x")));
            infer(a, p.type);
            continue;
        }
        if (r->mut != (p.mode == ParamMode::Mut)) {
            error(Code::E0408, a->span, "parameter '" + p.name + "' is '" + want + "', found '" +
                                            (r->mut ? "&mut" : "&") + "'");
        }
        const Type* t = infer(r->operand, p.type);
        r->type = t;
        // Р62: `&S { ... }` — временное на время вызова (только чтение).
        bool literal = !r->mut && as<StructLitExpr>(strip_paren(r->operand));
        if (!is_place(r->operand) && !literal) {
            if (t->kind != TK::Error) {
                error(Code::E0501, r->operand->span, r->mut ? "a reference needs a variable, a field or an element"
                                                            : "a reference needs a variable, a field, an element or a struct literal");
            }
            continue;
        }
        if (r->mut && mutable_place(r->operand, "mutate")) note_write(r->operand, "mutate");
        bool prefix = !r->mut && c_prefix(p.type, t);  // Р62: `&S` в `&B` по общему началу
        if (t != p.type && !prefix && t->kind != TK::Error && p.type->kind != TK::Error) {
            error(Code::E0401, r->operand->span, "expected '" + tn(p.type) + "', found '" + tn(t) + "'");
        }
    }
}

const Type* Checker::builtin_call(CallExpr* c, uint32_t b, const Type* hint) {
    c->call = CallKind::Builtin;
    c->builtin = b;
    auto bf = static_cast<BuiltinFn>(b);
    std::string name(kBuiltinFnNames[b]);
    bool checked2 = bf == BuiltinFn::CheckedAdd || bf == BuiltinFn::CheckedSub || bf == BuiltinFn::CheckedMul ||
                    bf == BuiltinFn::CheckedDiv;
    size_t want = bf == BuiltinFn::Repeat || bf == BuiltinFn::Min || bf == BuiltinFn::Max || bf == BuiltinFn::ReadFile ||
                          checked2
                      ? 2
                  : bf == BuiltinFn::ArgsLen ? 0
                                             : 1;
    bool io = bf == BuiltinFn::Print || bf == BuiltinFn::Println || bf == BuiltinFn::Eprintln ||
              bf == BuiltinFn::ArgsLen || bf == BuiltinFn::ArgAt || bf == BuiltinFn::ReadFile;
    if (io && fn_ && is_det(*fn_)) {  // ввод-вывод — не класс D (Р12, Р55)
        error(Code::E0425, c->span, "'" + name + "' does input or output; '" + fn_->name +
                                        "' is class D and cannot use it. Return the value and use it outside class D");
    }
    if (c->args.size() != want) {
        error(Code::E0407, c->span, "'" + name + "' takes " + std::to_string(want) + " argument" +
                                        (want == 1 ? "" : "s") + ", found " + std::to_string(c->args.size()));
        infer_loose(c->args);
        return err();
    }
    for (size_t i = 0; i < c->args.size(); i++) {
        Expr* a = c->args[i];
        bool out = bf == BuiltinFn::ReadFile && i == 1;  // __read_file(path, &mut out)
        auto* r = as<RefExpr>(a);
        if (out) {
            if (!r || !r->mut) {
                error(Code::E0408, a->span, "the second argument of '" + name + "' is '&mut str'");
                if (r) r->type = infer(r->operand, nullptr);
                else infer(a, nullptr);
                return err();
            }
            r->type = check(r->operand, prim(TK::Str));
            if (!is_place(r->operand)) {
                error(Code::E0501, r->operand->span, "a reference needs a variable, a field or an element");
            } else if (mutable_place(r->operand, "mutate")) {
                note_write(r->operand, "mutate");
            }
            continue;
        }
        if (r) {
            error(Code::E0501, r->span, "built-in '" + name + "' takes values; remove the mark");
            r->type = infer(r->operand, nullptr);
            return err();
        }
    }
    switch (bf) {
        case BuiltinFn::Len: {
            const Type* t = infer(c->args[0], nullptr);
            note_len(c->args[0]);
            if (t->kind != TK::List && t->kind != TK::Slice && t->kind != TK::Str && t->kind != TK::Map &&
                t->kind != TK::Pool && t->kind != TK::Array && t->kind != TK::Error) {
                error(Code::E0401, c->args[0]->span,
                      "'len' needs a List, a slice, a Map, a Pool or a str, found '" + tn(t) + "'");
            }
            return prim(TK::I64);
        }
        case BuiltinFn::Repeat: {
            const Type* n = infer(c->args[1], prim(TK::I64));
            if (!is_integer(n) && n->kind != TK::Error) {
                error(Code::E0401, c->args[1]->span, "'repeat' count must be an integer, found '" + tn(n) + "'");
            }
            const Type* h = unwrap_hint(hint);
            if (h && h->kind == TK::List) {
                check(c->args[0], h->elem);
                return h;
            }
            const Type* t = infer(c->args[0], nullptr);
            return t->kind == TK::Error ? t : tt_.list(t);
        }
        case BuiltinFn::Min: case BuiltinFn::Max: {
            const Type* t = common(c->args[0], c->args[1], unwrap_hint(hint), c->span, name.c_str());
            if (!is_numeric(t) && t->kind != TK::Error) {
                error(Code::E0403, c->span, "'" + name + "' needs numbers, found '" + tn(t) + "'");
                return err();
            }
            return t;
        }
        case BuiltinFn::Abs: {
            const Type* t = infer(c->args[0], unwrap_hint(hint));
            if (t->kind == TK::Error || (is_integer(t) && is_signed(t)) || is_float(t)) return t;
            error(Code::E0403, c->span, "'abs' needs a signed integer or a float, found '" + tn(t) + "'");
            return err();
        }
        case BuiltinFn::CheckedAdd: case BuiltinFn::CheckedSub: case BuiltinFn::CheckedMul:
        case BuiltinFn::CheckedDiv: {
            const Type* t = common(c->args[0], c->args[1], unwrap_hint(hint), c->span, name.c_str());
            if (t->kind == TK::Error) return err();
            if (!is_integer(t)) {
                error(Code::E0403, c->span, "'" + name + "' needs integers, found '" + tn(t) + "'");
                return err();
            }
            return tt_.optional(t);
        }
        case BuiltinFn::AtomicNew: {  // О13, прототип: тип — из записи переменной или из значения
            if (fn_ && is_det(*fn_)) {
                error(Code::E0425, c->span, "'atomic' is shared between threads; '" + fn_->name +
                                                "' is class D and cannot use it");
            }
            const Type* h = unwrap_hint(hint);
            if (h && h->kind == TK::Atomic) {
                check(c->args[0], h->elem);
                return h;
            }
            const Type* t = infer(c->args[0], nullptr);
            if (t->kind == TK::Error) return err();
            if (!is_integer(t) && t->kind != TK::Bool) {
                error(Code::E0401, c->args[0]->span, "'atomic' holds an integer or a bool, found '" + tn(t) + "'");
                return err();
            }
            return tt_.atomic(t);
        }
        case BuiltinFn::ChannelNew: {  // О13, прототип: тип элемента — только из записи
            if (fn_ && is_det(*fn_)) {
                error(Code::E0425, c->span, "'channel' is shared between threads; '" + fn_->name +
                                                "' is class D and cannot use it");
            }
            check(c->args[0], prim(TK::I64));
            const Type* h = unwrap_hint(hint);
            if (h && h->kind == TK::Channel) return h;
            error(Code::E0401, c->span, "write the element type: 'let ch: Channel[T] = channel(n)'");
            return err();
        }
        case BuiltinFn::MutexNew: {  // О13, прототип: данные — копия значения
            if (fn_ && is_det(*fn_)) {
                error(Code::E0425, c->span, "'mutex' is shared between threads; '" + fn_->name +
                                                "' is class D and cannot use it");
            }
            const Type* h = unwrap_hint(hint);
            if (h && h->kind == TK::Mutex) {
                check(c->args[0], h->elem);
                return h;
            }
            const Type* t = infer(c->args[0], nullptr);
            if (t->kind == TK::Error) return err();
            if (t->kind == TK::Void) {
                error(Code::E0401, c->args[0]->span, "'mutex' needs a value to protect");
                return err();
            }
            return tt_.mutex(t);
        }
        case BuiltinFn::CheckedNeg: {
            const Type* t = infer(c->args[0], unwrap_hint(hint));
            if (t->kind == TK::Error) return err();
            if (!is_integer(t) || !is_signed(t)) {
                error(Code::E0403, c->span, "'checked_neg' needs a signed integer, found '" + tn(t) + "'");
                return err();
            }
            return tt_.optional(t);
        }
        case BuiltinFn::ParseInt:
            check(c->args[0], prim(TK::Str));
            return tt_.optional(prim(TK::I64));
        case BuiltinFn::IsDigit: case BuiltinFn::IsAlpha: case BuiltinFn::IsSpace: case BuiltinFn::IsAlnum:
            check(c->args[0], prim(TK::U8));
            return prim(TK::Bool);
        case BuiltinFn::Assert:
            check(c->args[0], prim(TK::Bool));
            return prim(TK::Void);
        case BuiltinFn::Print: case BuiltinFn::Println: case BuiltinFn::Eprintln:
            check(c->args[0], prim(TK::Str));
            return prim(TK::Void);
        case BuiltinFn::ArgsLen: return prim(TK::I64);
        case BuiltinFn::ArgAt:
            check(c->args[0], prim(TK::I64));
            return prim(TK::Str);
        case BuiltinFn::ReadFile:
            check(c->args[0], prim(TK::Str));
            return prim(TK::I32);
        case BuiltinFn::Count: break;
    }
    return err();
}

const Type* Checker::method_call(CallExpr* c, FieldExpr* f) {
    const Type* t = infer(f->object, nullptr);
    if (t->kind == TK::Error) {
        infer_loose(c->args);
        return err();
    }
    const MethodInfo* mi = nullptr;
    for (const MethodInfo& m : kMethods) {
        if (f->name == m.name) mi = &m;
    }
    if (t->kind == TK::Atomic) return atomic_method(c, f, t, mi ? mi->m : Method::Push, mi != nullptr);
    if (t->kind == TK::Channel) return channel_method(c, f, t, mi ? mi->m : Method::Push, mi != nullptr);
    bool list = t->kind == TK::List, slice = t->kind == TK::Slice, str = t->kind == TK::Str, map = t->kind == TK::Map;
    bool pool = t->kind == TK::Pool;
    bool list_method = mi && mi->m <= Method::Contains && mi->m != Method::Add;
    bool str_method = mi && mi->m >= Method::Trim && mi->m <= Method::Split;
    bool map_method = mi && (mi->m == Method::Get || mi->m == Method::Keys || mi->m == Method::Contains ||
                             mi->m == Method::Remove || mi->m == Method::Clear || mi->m == Method::Reserve);
    bool pool_method = mi && (mi->m == Method::Add || mi->m == Method::Get || mi->m == Method::Contains ||
                              mi->m == Method::Remove || mi->m == Method::Clear || mi->m == Method::Reserve ||
                              mi->m == Method::Handles);
    bool ok = mi && (map ? map_method : pool ? pool_method : (list_method && (list || slice)) || (str_method && str));
    if (!ok) {
        error(Code::E0410, f->name_span, "'" + tn(t) + "' has no method '" + f->name + "'");
        infer_loose(c->args);
        return err();
    }
    c->call = CallKind::Method;
    c->method = mi->m;
    size_t arity = 0;
    switch (mi->m) {
        case Method::Push: case Method::Contains: case Method::Remove: case Method::Find: case Method::StartsWith:
        case Method::Split: case Method::Get: case Method::Reserve: case Method::Add:
            arity = 1;
            break;
        case Method::Insert: arity = 2; break;
        default: arity = 0; break;
    }
    if (c->args.size() != arity) {
        error(Code::E0407, c->span, "method '" + f->name + "' takes " + std::to_string(arity) + " argument" +
                                        (arity == 1 ? "" : "s") + ", found " + std::to_string(c->args.size()));
        infer_loose(c->args);
        return err();
    }
    bool mutating = map || pool ? mi->m < Method::Contains : list_method && mi->m != Method::Contains;
    if (mutating) {
        if (slice) {
            error(Code::E0412, f->name_span, "slices are read-only; '" + f->name + "' needs a List");
        } else {
            std::string why = "call '" + f->name + "' on";
            if (mutable_place(f->object, why.c_str())) {
                note_write(f->object, why.c_str());
            }
        }
    }
    const Type* elem = t->elem;
    if (pool) {  // Р67
        const Type* h = tt_.pool_handle(elem);
        switch (mi->m) {
            case Method::Add: check(c->args[0], elem); return h;
            case Method::Get: check(c->args[0], h); return tt_.optional(elem);
            case Method::Contains: check(c->args[0], h); return prim(TK::Bool);
            case Method::Remove: check(c->args[0], h); return tt_.optional(elem);
            case Method::Clear: return prim(TK::Void);
            case Method::Reserve: check(c->args[0], prim(TK::I64)); return prim(TK::Void);
            case Method::Handles: return tt_.list(h);
            default: return err();
        }
    }
    if (map) {
        switch (mi->m) {
            case Method::Get: check(c->args[0], t->key); return tt_.optional(elem);
            case Method::Contains: check(c->args[0], t->key); return prim(TK::Bool);
            case Method::Remove: check(c->args[0], t->key); return tt_.optional(elem);
            case Method::Clear: return prim(TK::Void);
            case Method::Reserve: check(c->args[0], prim(TK::I64)); return prim(TK::Void);
            case Method::Keys: return tt_.list(t->key);
            default: return err();
        }
    }
    switch (mi->m) {
        case Method::Push: check(c->args[0], elem); return prim(TK::Void);
        case Method::Pop: return tt_.optional(elem);
        case Method::Insert:
            check(c->args[0], prim(TK::I64));
            check(c->args[1], elem);
            return prim(TK::Void);
        case Method::Remove: check(c->args[0], prim(TK::I64)); return elem;
        case Method::Clear: return prim(TK::Void);
        case Method::Reserve: check(c->args[0], prim(TK::I64)); return prim(TK::Void);
        case Method::Sort:
            if (!ordered(elem)) error(Code::E0422, f->name_span, "elements of type '" + tn(elem) + "' cannot be sorted");
            return prim(TK::Void);
        case Method::Contains:
            check(c->args[0], elem);
            if (!equatable(elem)) error(Code::E0422, f->name_span, "elements of type '" + tn(elem) + "' cannot be compared");
            return prim(TK::Bool);
        case Method::Trim: return prim(TK::Str);
        case Method::Find: check(c->args[0], prim(TK::Str)); return tt_.optional(prim(TK::I64));
        case Method::StartsWith: check(c->args[0], prim(TK::Str)); return prim(TK::Bool);
        case Method::Split: check(c->args[0], prim(TK::Str)); return tt_.list(prim(TK::Str));
        case Method::Get: case Method::Keys: case Method::Add: case Method::Handles: break;
        case Method::Load: case Method::Store: case Method::Sub: case Method::Swap: case Method::Cas: break;
        case Method::Send: case Method::Recv: case Method::TryRecv: case Method::Close: break;
    }
    return err();
}

// Методы Atomic[T] (О13, прототип): через общий доступ — получатель не
// меняется для правила доступа (Р66) и может быть `let` и `&`.
const Type* Checker::atomic_method(CallExpr* c, FieldExpr* f, const Type* t, Method m, bool known) {
    bool num = is_integer(t->elem);
    size_t arity = 0;
    bool ok = known;
    switch (m) {
        case Method::Load: arity = 0; break;
        case Method::Store: case Method::Swap: arity = 1; break;
        case Method::Add: case Method::Sub: arity = 1; ok = ok && num; break;
        case Method::Cas: arity = 2; break;
        default: ok = false; break;
    }
    if (!ok) {
        error(Code::E0410, f->name_span, "'" + tn(t) + "' has no method '" + f->name + "'");
        infer_loose(c->args);
        return err();
    }
    if (fn_ && is_det(*fn_)) {
        error(Code::E0425, c->span, "an 'Atomic' is shared between threads; '" + fn_->name +
                                        "' is class D and cannot use it");
    }
    c->call = CallKind::Method;
    c->method = m;
    if (c->args.size() != arity) {
        error(Code::E0407, c->span, "method '" + f->name + "' takes " + std::to_string(arity) + " argument" +
                                        (arity == 1 ? "" : "s") + ", found " + std::to_string(c->args.size()));
        infer_loose(c->args);
        return err();
    }
    switch (m) {
        case Method::Load: return t->elem;
        case Method::Store: check(c->args[0], t->elem); return prim(TK::Void);
        case Method::Cas:
            check(c->args[0], t->elem);
            check(c->args[1], t->elem);
            return prim(TK::Bool);
        default: check(c->args[0], t->elem); return t->elem;
    }
}

// Методы Channel[T] (О13, прототип): send ждёт места, recv — значения
// (none — закрыт и пуст), try_recv не ждёт, close.
const Type* Checker::channel_method(CallExpr* c, FieldExpr* f, const Type* t, Method m, bool known) {
    bool ok = known && (m == Method::Send || m == Method::Recv || m == Method::TryRecv || m == Method::Close);
    if (!ok) {
        error(Code::E0410, f->name_span, "'" + tn(t) + "' has no method '" + f->name + "'");
        infer_loose(c->args);
        return err();
    }
    if (fn_ && is_det(*fn_)) {
        error(Code::E0425, c->span, "a 'Channel' is shared between threads; '" + fn_->name +
                                        "' is class D and cannot use it");
    }
    c->call = CallKind::Method;
    c->method = m;
    size_t arity = m == Method::Send ? 1 : 0;
    if (c->args.size() != arity) {
        error(Code::E0407, c->span, "method '" + f->name + "' takes " + std::to_string(arity) + " argument" +
                                        (arity == 1 ? "" : "s") + ", found " + std::to_string(c->args.size()));
        infer_loose(c->args);
        return err();
    }
    switch (m) {
        case Method::Send: check(c->args[0], t->elem); return prim(TK::Void);
        case Method::Recv: case Method::TryRecv: return tt_.optional(t->elem);
        default: return prim(TK::Void);
    }
}

const Type* Checker::convert_call(CallExpr* c, const Type* target, bool wrap) {
    c->call = wrap ? CallKind::Wrap : CallKind::Convert;
    c->target = target;
    std::string what = wrap ? type_name(target) + ".wrap" : type_name(target);
    if (c->args.size() != 1) {
        error(Code::E0407, c->span, "'" + what + "' takes 1 argument, found " + std::to_string(c->args.size()));
        infer_loose(c->args);
        return target;
    }
    // Литерал в `T(5)` получает тип T сразу: вне диапазона — ошибка компиляции.
    const Type* t = infer(c->args[0], !wrap && is_numeric(target) ? target : nullptr);
    if (t->kind == TK::Error) return target;
    bool ok;
    if (wrap) ok = is_integer(t);
    else if (is_numeric(target)) ok = is_numeric(t);
    else if (target->kind == TK::Str) ok = is_numeric(t) || t->kind == TK::Bool || t->kind == TK::Str;
    else ok = false;
    if (!ok) {
        error(Code::E0418, c->span, "cannot convert '" + tn(t) + "' with '" + what + "(...)'");
    }
    return target;
}

const Type* Checker::variant_value(const EnumDef* en, uint32_t index, VariantForm form, std::vector<Expr*>* args,
                                   std::vector<FieldInit>* fields, Span span) {
    const VariantDef& v = en->variants[index];
    std::string full = en->name + "." + v.name;
    if (form != v.form) {
        std::string how = v.form == VariantForm::Unit    ? full
                          : v.form == VariantForm::Tuple ? full + "(...)"
                                                         : full + " { ... }";
        error(Code::E0414, span, "variant '" + v.name + "' is written " + how);
        if (args) {
            infer_loose(*args);
        }
        if (fields) {
            for (FieldInit& fi : *fields) {
                if (fi.value) infer(fi.value, nullptr);
            }
        }
        return tt_.enum_type(en);
    }
    if (form == VariantForm::Tuple && args) {
        if (args->size() != v.types.size()) {
            error(Code::E0407, span, "variant '" + v.name + "' takes " + std::to_string(v.types.size()) +
                                         " value" + (v.types.size() == 1 ? "" : "s") + ", found " +
                                         std::to_string(args->size()));
            infer_loose(*args);
        } else {
            for (size_t i = 0; i < args->size(); i++) check((*args)[i], v.types[i]);
        }
    } else if (form == VariantForm::Record && fields) {
        field_inits(*fields, v.fields, false, span, "variant '" + v.name + "'");
    }
    return tt_.enum_type(en);
}

void Checker::field_inits(std::vector<FieldInit>& inits, const std::vector<FieldDef>& fields, bool defaults_ok,
                          Span span, const std::string& what, bool c_zero) {
    std::vector<char> given(fields.size(), 0);
    for (FieldInit& fi : inits) {
        int idx = -1;
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i].name == fi.name) idx = static_cast<int>(i);
        }
        Expr* value = fi.value;
        if (!value) {
            if (!fi.shorthand) {
                fi.shorthand = m_.ast->make<NameExpr>();
                fi.shorthand->name = fi.name;
                fi.shorthand->span = fi.name_span;
            }
            value = fi.shorthand;
        }
        if (idx < 0) {
            error(Code::E0413, fi.name_span, what + " has no field '" + fi.name + "'");
            infer(value, nullptr);
            continue;
        }
        if (given[static_cast<size_t>(idx)]) {
            error(Code::E0413, fi.name_span, "field '" + fi.name + "' is given twice");
        }
        given[static_cast<size_t>(idx)] = 1;
        if (fields[static_cast<size_t>(idx)].counter) {  // Р62: счётчик пишет компилятор
            std::string of;
            for (const FieldDef& f : fields) {
                if (f.count == idx) of += (of.empty() ? "'" : ", '") + f.name + "'";
            }
            error(Code::E0413, fi.name_span, "field '" + fi.name + "' is the length of " + of +
                                                 " (@count); C gets it from the slice, remove it here");
        }
        check(value, fields[static_cast<size_t>(idx)].type);
    }
    std::string missing;
    for (size_t i = 0; i < fields.size(); i++) {
        // Р62: у `@c` пропущенное поле — ноль, если он у типа есть.
        if (!given[i] && !(defaults_ok && fields[i].default_value) && !(c_zero && zeroable(fields[i].type))) {
            missing += (missing.empty() ? "'" : ", '") + fields[i].name + "'";
        }
    }
    if (!missing.empty()) error(Code::E0413, span, what + " is missing field" + (missing.find(',') != std::string::npos ? "s " : " ") + missing);
}

const Type* Checker::struct_lit(StructLitExpr* s) {
    Res* p = nullptr;
    if (auto* n = as<NameExpr>(s->path)) {
        resolve_name(n);
        p = &n->res;
    } else if (auto* f = as<FieldExpr>(s->path)) {
        resolve_field(f);
        p = &f->res;
    }
    if (p && p->kind == ResKind::Struct) {
        s->res = *p;
        field_inits(s->fields, p->st->fields, true, s->span, "struct '" + p->st->name + "'", p->st->c);
        if (p->st->union_ && s->fields.size() > 1) {  // Р62: у объединения — одно поле
            error(Code::E0413, s->fields[1].name_span, "a '@union' literal sets one field; '" + p->st->name +
                                                           "' shares the memory of its fields");
        }
        return tt_.struct_type(p->st);
    }
    if (p && p->kind == ResKind::Variant) {
        s->res = *p;
        return variant_value(p->en, p->index, VariantForm::Record, nullptr, &s->fields, s->span);
    }
    if (p && p->kind != ResKind::None) {
        error(Code::E0320, s->path->span, "struct literal needs a struct or an enum variant");
    }
    for (FieldInit& fi : s->fields) {
        if (fi.value) infer(fi.value, nullptr);
    }
    return err();
}

// Перечисление для краткой записи `.V`: из ожидаемого типа или единственное
// видимое перечисление с таким вариантом.
const EnumDef* Checker::enum_for_variant(const std::string& name, const Type* hint, Span span) {
    const Type* h = hint;
    while (h && (h->kind == TK::Optional || h->kind == TK::Result)) h = h->kind == TK::Result && !h->elem ? nullptr : h->elem;
    if (h && h->kind == TK::Enum) {
        if (h->edef->find(name) >= 0) return h->edef;
        error(Code::E0312, span, "enum '" + tn(h) + "' has no variant '" + name + "'");
        return nullptr;
    }
    std::vector<const EnumDef*> found;
    for (auto& [n, s] : m_.scope) {
        if (s.kind == SymKind::Enum && s.en->find(name) >= 0) found.push_back(s.en);
        if (s.kind == SymKind::Module) {
            for (auto& [mn, ms] : s.mod->scope) {
                if (ms.kind == SymKind::Enum && ms.en->pub && ms.en->find(name) >= 0) found.push_back(ms.en);
            }
        }
    }
    if (found.size() == 1) return found[0];
    if (found.empty()) {
        error(Code::E0312, span, "no enum in scope has a variant '" + name + "'");
    } else {
        std::string list;
        for (const EnumDef* e : found) list += (list.empty() ? "" : ", ") + e->name;
        error(Code::E0404, span, "cannot tell which enum '." + name + "' belongs to (" + list + "); write it: " +
                                     found[0]->name + "." + name);
    }
    return nullptr;
}

const Type* Checker::variant_lit(VariantLitExpr* v, const Type* hint) {
    const EnumDef* en = enum_for_variant(v->name, hint, v->name_span);
    if (!en) {
        infer_loose(v->args);
        for (FieldInit& fi : v->fields) {
            if (fi.value) infer(fi.value, nullptr);
        }
        return err();
    }
    uint32_t idx = static_cast<uint32_t>(en->find(v->name));
    v->res.kind = ResKind::Variant;
    v->res.en = en;
    v->res.index = idx;
    return variant_value(en, idx, v->form, &v->args, &v->fields, v->span);
}

const Type* Checker::list_lit(ListLitExpr* l, const Type* hint) {
    const Type* h = unwrap_hint(hint);
    if (h && h->kind == TK::Error) {  // тип уже с ошибкой: без второго сообщения
        for (Expr* x : l->items) infer(x, nullptr);
        return err();
    }
    if (h && h->kind == TK::Map) {  // Р54: у Map литерал — только пустой
        if (l->items.empty()) return h;
        error(Code::E0401, l->span, "a Map literal must be empty: '[]'; add entries with m[k] = v");
        for (Expr* x : l->items) infer(x, nullptr);
        return err();
    }
    if (h && h->kind == TK::Pool) {  // Р67: у Pool — тоже
        if (l->items.empty()) return h;
        error(Code::E0401, l->span, "a Pool literal must be empty: '[]'; add values with p.add(v)");
        for (Expr* x : l->items) infer(x, nullptr);
        return err();
    }
    if (h && h->kind == TK::Array) {  // Р62: значение поля `[N]T` — ровно N элементов
        for (Expr* x : l->items) check(x, h->elem);
        if (l->items.size() != h->len) {
            error(Code::E0401, l->span, "'" + tn(h) + "' takes exactly " + std::to_string(h->len) + " element" +
                                            (h->len == 1 ? "" : "s") + ", found " + std::to_string(l->items.size()));
        }
        return h;
    }
    const Type* elem = h && (h->kind == TK::List || h->kind == TK::Slice) ? h->elem : nullptr;
    if (l->items.empty()) {
        if (!elem) {
            error(Code::E0404, l->span, "cannot infer the element type of '[]'; annotate it: let xs: List[i32] = []");
            return err();
        }
        return tt_.list(elem);
    }
    if (!elem) {
        elem = infer(l->items[0], nullptr);
        if (elem->kind == TK::Error) {
            for (size_t i = 1; i < l->items.size(); i++) infer(l->items[i], nullptr);
            return err();
        }
    } else {
        check(l->items[0], elem);
    }
    for (size_t i = 1; i < l->items.size(); i++) check(l->items[i], elem);
    return tt_.list(elem);
}

const Type* Checker::index_expr(IndexExpr* i) {
    const Type* t = infer(i->object, nullptr);
    note_extend(i->object, i);
    if (t->kind == TK::Map) {  // Р54: m[k] — значение ключа
        check(i->index, t->key);
        return t->elem;
    }
    if (t->kind == TK::Pool) {  // Р67: p[h] — значение по хэндлу
        check(i->index, tt_.pool_handle(t->elem));
        return t->elem;
    }
    const Type* k = infer(i->index, prim(TK::I64));
    if (k->kind != TK::Error) {
        if (!is_integer(k)) error(Code::E0401, i->index->span, "index must be an integer, found '" + tn(k) + "'");
        else coerce(i->index, prim(TK::I64));
    }
    switch (t->kind) {
        case TK::List: case TK::Slice: case TK::Array: return t->elem;
        case TK::Str: return prim(TK::U8);
        case TK::Error: return err();
        default:
            error(Code::E0401, i->object->span, "cannot index '" + tn(t) + "'; indexing needs a List, a slice or a str");
            return err();
    }
}

const Type* Checker::slice_expr(SliceExpr* s) {
    const Type* t = infer(s->object, nullptr);
    note_extend(s->object, s);
    for (Expr* bound : {s->lo, s->hi}) {
        if (!bound) continue;
        const Type* k = infer(bound, prim(TK::I64));
        if (k->kind == TK::Error) continue;
        if (!is_integer(k)) error(Code::E0401, bound->span, "slice bound must be an integer, found '" + tn(k) + "'");
        else coerce(bound, prim(TK::I64));
    }
    switch (t->kind) {
        case TK::List: case TK::Slice: case TK::Array: return tt_.slice(t->elem);
        case TK::Str: return prim(TK::Str);
        case TK::Map:
            error(Code::E0403, s->object->span, "a Map cannot be sliced; read values with m[k]");
            return err();
        case TK::Pool:
            error(Code::E0403, s->object->span, "a Pool cannot be sliced; read values with p[h]");
            return err();
        case TK::Error: return err();
        default:
            error(Code::E0401, s->object->span, "cannot slice '" + tn(t) + "'; slicing needs a List, a slice or a str");
            return err();
    }
}

const Type* Checker::try_expr(TryExpr* t) {
    if (defer_depth_ > 0) error(Code::E0508, t->span, "'try' cannot leave a 'defer'");
    else parallel_exit(t->span, "try");
    const Type* op = infer(t->operand, nullptr);
    if (op->kind == TK::Error) return err();
    if (op->kind != TK::Result) {
        error(Code::E0505, t->span, "'try' needs a result 'E!T', found '" + tn(op) + "'");
        return err();
    }
    const Type* ret = fn_ ? fn_->ret : nullptr;
    if (!ret || ret->kind != TK::Result) {
        error(Code::E0505, t->span, "'try' passes the error on, so the function's result must be 'E!T'");
    } else if (ret->err && op->err && ret->err != op->err) {
        error(Code::E0505, t->span, "the error type '" + tn(op->err) + "' differs from the function's error type '" +
                                        tn(ret->err) + "'");
    }
    return op->elem;
}

// `spawn f(…)` (О13, прототип). В блоке `tasks` — задача блока (вариант А);
// иначе — `Task[T]` (вариант В).
const Type* Checker::spawn_expr(SpawnExpr* sp) {
    if (!sp->group) {
        variant_ok('c', sp->span, "'spawn' outside a 'tasks' block");
        error(Code::E0519, sp->span, "'spawn' outside a 'tasks' block (variant C) is not implemented yet in the prototype");
        infer(sp->operand, nullptr);
        return err();
    }
    std::vector<size_t> marks;
    for (const TasksScope& sc : tasks_) marks.push_back(sc.accesses.size());
    const Type* t = infer(sp->operand, nullptr);
    auto* c = as<CallExpr>(sp->operand);
    if (!c) {
        if (t->kind != TK::Error) error(Code::E0520, sp->operand->span, "'spawn' needs a call: 'spawn f(...)'");
        return prim(TK::Void);
    }
    if (c->call != CallKind::Fn || !c->fn || c->fn->is_extern) {
        if (t->kind != TK::Error) {
            error(Code::E0520, c->span, "a task runs a dpl function: 'spawn f(...)'; call methods, built-in "
                                        "functions and C from that function");
        }
        return prim(TK::Void);
    }
    if (t->kind != TK::Void && t->kind != TK::Error) {
        error(Code::E0520, c->span, "the result of a task in 'tasks' is lost: '" + c->fn->name + "' returns '" +
                                        tn(t) + "'; write the result through a '&mut' parameter");
    }
    task_spawn(sp, c, marks);
    return prim(TK::Void);
}

// --- выражение целиком

const Type* Checker::infer(Expr* e, const Type* hint) {
    if (!e) return err();
    if (e->type) return e->type;
    const Type* t = err();
    switch (e->kind) {
        case ExprKind::Int: t = int_lit(static_cast<IntExpr*>(e), hint, false, e->span); break;
        case ExprKind::Float: {
            const Type* h = unwrap_hint(hint);
            t = h && is_float(h) ? h : prim(TK::F64);
            break;
        }
        case ExprKind::Str: t = prim(TK::Str); break;
        case ExprKind::Byte: t = prim(TK::U8); break;
        case ExprKind::Bool: t = prim(TK::Bool); break;
        case ExprKind::None: {
            const Type* h = hint;
            while (h && h->kind == TK::Result) h = h->elem;
            if (h && h->kind == TK::Optional) {
                t = h;
            } else {
                error(Code::E0404, e->span, "cannot infer the type of 'none'; it needs an optional type '?T'");
            }
            break;
        }
        case ExprKind::Name: t = name_expr(static_cast<NameExpr*>(e)); break;
        case ExprKind::Field: t = field_expr(static_cast<FieldExpr*>(e), hint); break;
        case ExprKind::Index: t = index_expr(static_cast<IndexExpr*>(e)); break;
        case ExprKind::Slice: t = slice_expr(static_cast<SliceExpr*>(e)); break;
        case ExprKind::Call: t = call(static_cast<CallExpr*>(e), hint); break;
        case ExprKind::Unary: t = unary(static_cast<UnaryExpr*>(e), hint); break;
        case ExprKind::Binary: t = binary(static_cast<BinaryExpr*>(e), hint); break;
        case ExprKind::Try: t = try_expr(static_cast<TryExpr*>(e)); break;
        case ExprKind::Spawn: t = spawn_expr(static_cast<SpawnExpr*>(e)); break;
        case ExprKind::Paren: t = infer(static_cast<ParenExpr*>(e)->inner, hint); break;
        case ExprKind::Ref: {
            auto* r = static_cast<RefExpr*>(e);
            const Type* h = hint && hint->kind == TK::Optional ? hint->elem : hint;
            if (h && h->kind == TK::Ref && !r->mut) {  // Р62: поле `&T`, `?&T` и параметр `?&T` функции C
                const Type* ot = infer(r->operand, h->elem);
                if (!is_place(r->operand) && !as<StructLitExpr>(strip_paren(r->operand)) && ot->kind != TK::Error) {
                    error(Code::E0501, r->operand->span, "a reference needs a variable, a field, an element or a struct literal");
                }
                t = ot->kind == TK::Error ? err() : tt_.ref(ot);
                break;
            }
            error(Code::E0501, e->span, "'&' marks only call arguments for '&' and '&mut' parameters, and 'for ... in'");
            infer(r->operand, hint);
            break;
        }
        case ExprKind::StructLit: t = struct_lit(static_cast<StructLitExpr*>(e)); break;
        case ExprKind::VariantLit: t = variant_lit(static_cast<VariantLitExpr*>(e), hint); break;
        case ExprKind::ListLit: t = list_lit(static_cast<ListLitExpr*>(e), hint); break;
        case ExprKind::Error: break;
    }
    e->type = t ? t : err();
    return e->type;
}

// --- места и ссылки (Р44)

bool Checker::is_place(Expr* e) {
    if (auto* p = as<ParenExpr>(e)) return is_place(p->inner);
    if (auto* n = as<NameExpr>(e)) return n->res.kind == ResKind::Local;
    if (auto* f = as<FieldExpr>(e)) return f->field >= 0 && is_place(f->object);
    if (auto* i = as<IndexExpr>(e)) return is_place(i->object);
    return false;
}

bool Checker::mutable_place(Expr* e, const char* why) {
    if (!e || (e->type && e->type->kind == TK::Error)) return true;
    if (auto* p = as<ParenExpr>(e)) return mutable_place(p->inner, why);
    if (auto* n = as<NameExpr>(e)) {
        if (n->res.kind != ResKind::Local) {
            error(Code::E0412, e->span, std::string("cannot ") + why + " '" + n->name + "'");
            return false;
        }
        Local* l = n->res.local;
        if (l->mutable_) return true;
        // У элемента обхода Map и среза `&mut` нет (E0419, E0412).
        std::string reason = l->is_param ? (l->is_ref ? "it is a '&' parameter; make it '&mut'"
                                                      : "it is a value parameter; take it as '&mut' to change it")
                             : l->is_ref ? (read_only_loop(l) ? "it is an element read through 'for ... in &'"
                                                              : "it is an element read through 'for ... in &'; use '&mut'")
                                         : "it is not declared with 'var'";
        error(Code::E0412, e->span, std::string("cannot ") + why + " '" + l->name + "': " + reason);
        return false;
    }
    if (auto* f = as<FieldExpr>(e)) {
        if (f->field >= 0 && f->object->type && f->object->type->kind == TK::PoolHandle) {
            error(Code::E0412, e->span, std::string("cannot ") + why + " '" + f->name +
                                            "' of a Handle: it is read-only; handles come from 'p.add(v)'");
            return false;
        }
        if (f->field >= 0) return mutable_place(f->object, why);
        error(Code::E0412, e->span, std::string("cannot ") + why + " this expression");
        return false;
    }
    if (auto* i = as<IndexExpr>(e)) {
        const Type* t = i->object->type;
        if (t && (t->kind == TK::Slice || t->kind == TK::Str)) {
            error(Code::E0412, e->span, std::string("cannot ") + why + " an element of '" + tn(t) + "': it is read-only");
            return false;
        }
        return mutable_place(i->object, why);
    }
    error(Code::E0412, e->span, std::string("cannot ") + why + " this expression; it is not a variable, field or element");
    return false;
}

Local* Checker::root_local(Expr* e) {
    if (auto* p = as<ParenExpr>(e)) return root_local(p->inner);
    if (auto* n = as<NameExpr>(e)) {
        if (n->res.kind != ResKind::Local) return nullptr;
        Local* l = n->res.local;
        return l->alias_of ? l->alias_of : l;
    }
    if (auto* f = as<FieldExpr>(e)) return root_local(f->object);
    if (auto* i = as<IndexExpr>(e)) return root_local(i->object);
    if (auto* s = as<SliceExpr>(e)) {  // срез xs[a..b] — корень xs; срез str — копия
        return s->type && s->type->kind == TK::Str ? nullptr : root_local(s->object);
    }
    return nullptr;
}

// В одном вызове `&mut x` не стоит рядом с другой ссылкой в ту же
// переменную: вызываемая функция могла бы изменить x и оставить вторую
// ссылку висячей (Р44, Р11).
void Checker::ref_args(CallExpr* c, const std::vector<ParamDef>& params) {
    // Ссылка аргумента: корень и изменяемость; срез-аргумент — ссылка только
    // для чтения в свою коллекцию.
    struct Ref {
        Local* root = nullptr;
        bool mut = false;
        Span span;
        Expr* place = nullptr;  // путь аргумента (Р66)
    };
    std::vector<Ref> refs;
    // Р62: заимствования внутри аргумента — структуры с полями-ссылками и
    // срезами: их корни читаются, пока идёт вызов.
    std::function<void(Expr*)> borrowed = [&](Expr* e) {
        e = strip_paren(e);
        if (!e) return;
        if (auto* s = as<StructLitExpr>(e)) {
            for (FieldInit& fi : s->fields) borrowed(fi.value ? fi.value : fi.shorthand);
        } else if (auto* l = as<ListLitExpr>(e)) {
            for (Expr* x : l->items) borrowed(x);
        } else if (auto* r = as<RefExpr>(e)) {
            if (is_place(r->operand)) refs.push_back({root_local(r->operand), false, e->span, r->operand});
            else borrowed(r->operand);
        } else if (Local* root = root_local(e); !root && (as<FieldExpr>(e) || as<IndexExpr>(e) || as<SliceExpr>(e))) {
            // Часть литерала — `[S { p: &x }][0..1]`: заимствования его элементов.
            if (auto* f = as<FieldExpr>(e)) borrowed(f->object);
            else if (auto* x = as<IndexExpr>(e)) borrowed(x->object);
            else borrowed(static_cast<SliceExpr*>(e)->object);
        } else if ((e->conv && e->conv->kind == TK::Slice) || as<SliceExpr>(e)) {
            refs.push_back({root, false, e->span, e});
        }
    };
    for (size_t i = 0; i < c->args.size() && i < params.size(); i++) {
        Expr* a = c->args[i];
        if (auto* r = as<RefExpr>(a)) {
            if (params[i].mode != ParamMode::Value) refs.push_back({root_local(r->operand), r->mut, a->span, r->operand});
            if (!is_place(r->operand)) borrowed(r->operand);
        } else if (params[i].type && params[i].type->kind == TK::Slice) {
            Local* root = root_local(a);  // `(xs[1..])` — тоже корень xs
            if (root) refs.push_back({root, false, a->span, a});
            else borrowed(a);  // `[S { ... }]`, `[S { ... }][0..1]` — заимствования внутри
        } else if (params[i].type && (c_borrowing(params[i].type) || params[i].type->kind == TK::Optional)) {
            borrowed(a);
        }
    }
    for (size_t i = 0; i < refs.size(); i++) {
        for (size_t j = 0; j < refs.size(); j++) {
            if (i == j || !refs[i].root || refs[i].root != refs[j].root || !refs[i].mut) continue;
            if (j < i && refs[j].mut) continue;  // пара двух &mut — одно сообщение
            if (!overlap(refs[i].place, refs[j].place)) continue;  // Р66: разные поля и постоянные индексы
            error(Code::E0502, refs[j].span, "'" + refs[i].root->name + "' is passed as '&mut' together with "
                                                 "another reference into it; pass a copy of the value instead");
        }
    }
}

// --- константы (Р42: константные выражения)

const ConstValue& Checker::const_value(ConstDef& c) {
    if (c.state == 2) return c.value;
    if (c.state == 1) {
        error(Code::E0421, c.span, "constant '" + c.name + "' depends on itself");
        c.state = 2;
        return c.value;
    }
    c.state = 1;
    Expr* value = const_cast<ConstItem*>(c.node)->value;
    const Type* t = c.type ? c.type : err();
    auto scalar = [&](const Type* x) { return is_numeric(x) || x->kind == TK::Bool || x->kind == TK::Str; };
    if (t->kind != TK::Error && !scalar(t) && !(t->kind == TK::List && scalar(t->elem))) {
        error(Code::E0421, c.node->type ? c.node->type->span : c.span,
              "a constant must be a number, a bool, a str or a List of them, found '" + tn(t) + "'");
        if (value) infer(value, nullptr);
    } else if (value) {
        size_t before = diags_.error_count();
        check(value, t);
        if (diags_.error_count() == before && t->kind != TK::Error) {
            ConstValue v;
            if (const_eval(value, t, v)) c.value = v;
        }
    }
    c.state = 2;
    return c.value;
}

namespace {

int64_t as_signed(uint64_t v, const Type* t) {
    int bits = int_bits(t);
    if (bits == 64) return static_cast<int64_t>(v);
    uint64_t m = 1ull << (bits - 1);
    v &= (1ull << bits) - 1;
    return static_cast<int64_t>((v ^ m) - m);
}

bool in_range(int64_t sv, uint64_t uv, bool use_signed, const Type* t) {
    int bits = int_bits(t);
    if (is_signed(t)) {
        if (!use_signed && uv > static_cast<uint64_t>(INT64_MAX)) return false;
        int64_t v = use_signed ? sv : static_cast<int64_t>(uv);
        if (bits == 64) return true;
        int64_t lim = static_cast<int64_t>(1ull << (bits - 1));
        return v >= -lim && v < lim;
    }
    if (use_signed && sv < 0) return false;
    uint64_t v = use_signed ? static_cast<uint64_t>(sv) : uv;
    return bits == 64 || v < (1ull << bits);
}

// Значение f32 округляется на каждом шаге — как при исполнении.
double round_to(double v, const Type* t) {
    return t && t->kind == TK::F32 ? static_cast<double>(static_cast<float>(v)) : v;
}

}  // namespace

// Размер значения в шагах бюджета: копия константы стоит столько же.
static uint64_t const_size(const ConstValue& v) {
    uint64_t n = v.s.size();
    for (const ConstValue& x : v.items) n += 1 + const_size(x);
    return n;
}

bool Checker::const_spend(Expr* e, uint64_t steps) {
    const_work_ += steps;
    if (const_work_ <= kConstEvalBudget) return true;
    if (!const_over_) {
        const_over_ = true;
        error(Code::E0421, e->span, "constant evaluation exceeds the budget of " + std::to_string(kConstEvalBudget) +
                                        " steps per module (a step costs 1 plus the bytes of the strings it makes)");
    }
    return false;
}

bool Checker::const_eval(Expr* e, const Type* want, ConstValue& out) {
    const Type* t = e->type;
    if (!const_spend(e, 1)) return false;
    auto fail = [&](const std::string& why) {
        error(Code::E0421, e->span, why);
        return false;
    };
    ConstValue v;
    switch (e->kind) {
        case ExprKind::Int:
            if (t && is_float(t)) {
                v.kind = ConstValue::Kind::Float;
                v.f = round_to(static_cast<double>(static_cast<IntExpr*>(e)->value), t);
            } else {
                v.kind = ConstValue::Kind::Int;
                v.i = static_cast<IntExpr*>(e)->value;
            }
            break;
        case ExprKind::Float: v.kind = ConstValue::Kind::Float; v.f = round_to(static_cast<FloatExpr*>(e)->value, t); break;
        case ExprKind::Bool: v.kind = ConstValue::Kind::Bool; v.b = static_cast<BoolExpr*>(e)->value; break;
        case ExprKind::Str:
            if (!const_spend(e, static_cast<StrExpr*>(e)->value.size())) return false;
            v.kind = ConstValue::Kind::Str;
            v.s = static_cast<StrExpr*>(e)->value;
            break;
        case ExprKind::Byte: v.kind = ConstValue::Kind::Int; v.i = static_cast<ByteExpr*>(e)->value; break;
        case ExprKind::Paren:
            if (!const_eval(static_cast<ParenExpr*>(e)->inner, nullptr, v)) return false;
            break;
        case ExprKind::ListLit:
            v.kind = ConstValue::Kind::List;
            for (Expr* item : static_cast<ListLitExpr*>(e)->items) {
                ConstValue x;
                if (!const_eval(item, nullptr, x)) return false;
                v.items.push_back(std::move(x));
            }
            break;
        case ExprKind::Name: case ExprKind::Field: {
            const Res& r = e->kind == ExprKind::Name ? static_cast<NameExpr*>(e)->res : static_cast<FieldExpr*>(e)->res;
            if (r.kind != ResKind::Const) return fail("a constant may only use literals, constants and operators");
            ConstDef& c = const_cast<ConstDef&>(*r.cn);
            const ConstValue& cv = c.module == &m_ ? const_value(c) : c.value;
            if (cv.kind == ConstValue::Kind::None || !const_spend(e, const_size(cv))) return false;
            v = cv;
            break;
        }
        case ExprKind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            Expr* inner = u->operand;
            while (auto* p = as<ParenExpr>(inner)) inner = p->inner;
            if (u->op == UnOp::Neg && as<IntExpr>(inner) && t && is_integer(t)) {
                // `-128` проверен по диапазону как литерал целиком.
                v.kind = ConstValue::Kind::Int;
                v.i = 0 - static_cast<IntExpr*>(inner)->value;
                break;
            }
            ConstValue a;
            if (!const_eval(u->operand, nullptr, a)) return false;
            v = a;
            if (u->op == UnOp::Not) {
                v.b = !a.b;
            } else if (a.kind == ConstValue::Kind::Float) {
                v.f = -a.f;
            } else if (u->op == UnOp::BitNot) {
                v.i = ~a.i;
                if (!is_signed(t) && int_bits(t) < 64) v.i &= (1ull << int_bits(t)) - 1;
            } else {  // -целое
                int64_t s = as_signed(a.i, t);
                if (s == static_cast<int64_t>(int_bits(t) == 64 ? INT64_MIN : -(1ll << (int_bits(t) - 1)))) {
                    return fail("constant arithmetic overflows '" + tn(t) + "'");
                }
                v.i = static_cast<uint64_t>(-s);
            }
            break;
        }
        case ExprKind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            ConstValue x, y;
            if (!const_eval(b->lhs, nullptr, x) || !const_eval(b->rhs, nullptr, y)) return false;
            const Type* ot = b->operand ? b->operand : t;
            if (b->op == BinOp::And || b->op == BinOp::Or) {
                v.kind = ConstValue::Kind::Bool;
                v.b = b->op == BinOp::And ? (x.b && y.b) : (x.b || y.b);
                break;
            }
            if (x.kind == ConstValue::Kind::Str) {
                if (b->op == BinOp::Add) {
                    if (!const_spend(e, x.s.size() + y.s.size())) return false;
                    v.kind = ConstValue::Kind::Str;
                    v.s = x.s + y.s;
                } else {
                    v.kind = ConstValue::Kind::Bool;
                    int c = x.s.compare(y.s);
                    v.b = b->op == BinOp::Eq ? c == 0 : b->op == BinOp::Ne ? c != 0 : b->op == BinOp::Lt ? c < 0
                        : b->op == BinOp::Le ? c <= 0 : b->op == BinOp::Gt ? c > 0 : c >= 0;
                }
                break;
            }
            if (x.kind == ConstValue::Kind::Float || y.kind == ConstValue::Kind::Float) {
                double a = x.kind == ConstValue::Kind::Float ? x.f : static_cast<double>(x.i);
                double c = y.kind == ConstValue::Kind::Float ? y.f : static_cast<double>(y.i);
                bool single = ot && ot->kind == TK::F32;
                float sa = static_cast<float>(a), sc = static_cast<float>(c);
                switch (b->op) {
                    case BinOp::Add: v.f = single ? static_cast<double>(sa + sc) : a + c; break;
                    case BinOp::Sub: v.f = single ? static_cast<double>(sa - sc) : a - c; break;
                    case BinOp::Mul: v.f = single ? static_cast<double>(sa * sc) : a * c; break;
                    case BinOp::Div: v.f = single ? static_cast<double>(sa / sc) : a / c; break;
                    default: {
                        v.kind = ConstValue::Kind::Bool;
                        v.b = b->op == BinOp::Eq ? a == c : b->op == BinOp::Ne ? a != c : b->op == BinOp::Lt ? a < c
                            : b->op == BinOp::Le ? a <= c : b->op == BinOp::Gt ? a > c : a >= c;
                        out = v;
                        return true;
                    }
                }
                v.kind = ConstValue::Kind::Float;
                break;
            }
            if (x.kind == ConstValue::Kind::Bool) {
                v.kind = ConstValue::Kind::Bool;
                v.b = b->op == BinOp::Eq ? x.b == y.b : x.b != y.b;
                break;
            }
            bool sg = is_signed(ot);
            int64_t sa = as_signed(x.i, ot), sb = as_signed(y.i, ot);
            uint64_t ua = x.i, ub = y.i;
            int64_t sr = 0;
            uint64_t ur = 0;
            bool ovf = false;
            v.kind = ConstValue::Kind::Int;
            switch (b->op) {
                case BinOp::Add: ovf = sg ? __builtin_add_overflow(sa, sb, &sr) : __builtin_add_overflow(ua, ub, &ur); break;
                case BinOp::Sub: ovf = sg ? __builtin_sub_overflow(sa, sb, &sr) : __builtin_sub_overflow(ua, ub, &ur); break;
                case BinOp::Mul: ovf = sg ? __builtin_mul_overflow(sa, sb, &sr) : __builtin_mul_overflow(ua, ub, &ur); break;
                case BinOp::Div: case BinOp::Rem:
                    if ((sg && sb == 0) || (!sg && ub == 0)) return fail("constant division by zero");
                    if (sg && sb == -1 && sa == INT64_MIN) return fail("constant arithmetic overflows '" + tn(ot) + "'");
                    if (sg) sr = b->op == BinOp::Div ? sa / sb : sa % sb;
                    else ur = b->op == BinOp::Div ? ua / ub : ua % ub;
                    break;
                case BinOp::AddWrap: ur = ua + ub; sg = false; break;
                case BinOp::SubWrap: ur = ua - ub; sg = false; break;
                case BinOp::MulWrap: ur = ua * ub; sg = false; break;
                case BinOp::BitAnd: ur = ua & ub; sg = false; break;
                case BinOp::BitOr: ur = ua | ub; sg = false; break;
                case BinOp::BitXor: ur = ua ^ ub; sg = false; break;
                case BinOp::Shl: case BinOp::Shr: {
                    int64_t n = as_signed(y.i, b->rhs->type ? b->rhs->type : ot);
                    if (n < 0 || n >= int_bits(ot)) return fail("constant shift by " + std::to_string(n) + " is out of range");
                    if (b->op == BinOp::Shl) ur = ua << n;
                    else if (is_signed(ot)) ur = static_cast<uint64_t>(sa >> n);
                    else ur = (ua & (int_bits(ot) == 64 ? ~0ull : (1ull << int_bits(ot)) - 1)) >> n;
                    sg = false;
                    break;
                }
                default: {  // сравнения
                    v.kind = ConstValue::Kind::Bool;
                    bool lt = sg ? sa < sb : ua < ub, eq = sg ? sa == sb : ua == ub;
                    v.b = b->op == BinOp::Eq ? eq : b->op == BinOp::Ne ? !eq : b->op == BinOp::Lt ? lt
                        : b->op == BinOp::Le ? lt || eq : b->op == BinOp::Gt ? !lt && !eq : !lt;
                    out = v;
                    return true;
                }
            }
            if (ovf) return fail("constant arithmetic overflows '" + tn(ot) + "'");
            bool wrapping = b->op == BinOp::AddWrap || b->op == BinOp::SubWrap || b->op == BinOp::MulWrap ||
                            b->op == BinOp::BitAnd || b->op == BinOp::BitOr || b->op == BinOp::BitXor ||
                            b->op == BinOp::Shl || b->op == BinOp::Shr;
            if (wrapping) {
                int bits = int_bits(ot);
                if (bits < 64) ur &= (1ull << bits) - 1;
                v.i = is_signed(ot) ? static_cast<uint64_t>(as_signed(ur, ot)) : ur;
            } else {
                if (!in_range(sr, ur, sg, ot)) return fail("constant arithmetic overflows '" + tn(ot) + "'");
                v.i = sg ? static_cast<uint64_t>(sr) : ur;
            }
            break;
        }
        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            if ((c->call != CallKind::Convert && c->call != CallKind::Wrap) || c->args.size() != 1) {
                return fail("a constant may only call conversions T(x)");
            }
            ConstValue a;
            if (!const_eval(c->args[0], nullptr, a)) return false;
            const Type* from = c->args[0]->conv ? c->args[0]->conv : c->args[0]->type;
            const Type* to = c->target;
            if (to->kind == TK::Str) {
                v.kind = ConstValue::Kind::Str;
                if (a.kind == ConstValue::Kind::Bool) v.s = a.b ? "true" : "false";
                else if (a.kind == ConstValue::Kind::Float) v.s = format_float(a.f, from->kind == TK::F32);
                else if (a.kind == ConstValue::Kind::Str) v.s = a.s;
                else v.s = is_signed(from) ? std::to_string(as_signed(a.i, from)) : std::to_string(a.i);
                if (!const_spend(e, v.s.size())) return false;
            } else if (is_float(to)) {
                v.kind = ConstValue::Kind::Float;
                v.f = a.kind == ConstValue::Kind::Float ? a.f
                      : is_signed(from) ? static_cast<double>(as_signed(a.i, from)) : static_cast<double>(a.i);
                if (to->kind == TK::F32) v.f = static_cast<double>(static_cast<float>(v.f));
            } else {
                v.kind = ConstValue::Kind::Int;
                if (a.kind == ConstValue::Kind::Float) {
                    if (!(a.f > -9.3e18 && a.f < 1.85e19)) return fail("constant conversion is out of range");
                    double tr = a.f < 0 ? -static_cast<double>(static_cast<uint64_t>(-a.f)) : a.f;
                    int64_t sv = static_cast<int64_t>(tr);
                    uint64_t uv = a.f < 0 ? 0 : static_cast<uint64_t>(tr);
                    if (!in_range(sv, uv, a.f < 0, to)) return fail("constant conversion is out of range of '" + tn(to) + "'");
                    v.i = a.f < 0 ? static_cast<uint64_t>(sv) : uv;
                } else if (c->call == CallKind::Wrap) {
                    int bits = int_bits(to);
                    uint64_t r = a.i;
                    if (bits < 64) r &= (1ull << bits) - 1;
                    v.i = is_signed(to) ? static_cast<uint64_t>(as_signed(r, to)) : r;
                } else {
                    bool sg = is_signed(from);
                    int64_t sv = as_signed(a.i, from);
                    if (!in_range(sv, a.i, sg, to)) return fail("constant conversion is out of range of '" + tn(to) + "'");
                    v.i = a.i;
                }
            }
            break;
        }
        default: return fail("a constant may only use literals, constants, operators and conversions T(x)");
    }
    (void)want;
    out = v;
    return true;
}

}  // namespace dp::sema
