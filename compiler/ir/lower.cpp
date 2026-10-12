// Понижение типизированного AST в Core IR (docs/ir/README.md, раздел
// «Понижение»).
//
// Выражение даёт операнд: место (copy), константу или временное (move).
// Временное живёт до конца своей инструкции исходника: его drop стоит в
// области инструкции. Переменная освобождается в конце своего блока.
// Выход из области — конец блока, return, break, continue, fail и
// ошибка try — выполняет её очистку: drop переменных и тела defer в
// обратном порядке. Drop идемпотентен (освобождает и обнуляет), поэтому
// освобождать ещё не заполненное или уже отданное (move) значение можно.
#include "ir/lower.h"

#include <algorithm>
#include <map>
#include <unordered_map>

#include "sema/builtins.h"

namespace dp::ir {

using namespace ast;
using sema::TK;

namespace {

bool is_int(const Type* t) { return sema::is_integer(t); }

// Выражение может изменить чужие места: вызов функции или изменяющий
// метод. Тогда операнды-места левее него читаются заранее (порядок
// вычисления — слева направо).
bool has_effect(const Expr* e);

bool any_effect(const std::vector<Expr*>& es) {
    for (const Expr* x : es) {
        if (has_effect(x)) return true;
    }
    return false;
}

bool has_effect(const Expr* e) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Call: {
            auto* c = static_cast<const CallExpr*>(e);
            if (c->call == CallKind::Fn || c->call == CallKind::Ptr) return true;
            if (c->call == CallKind::Method && c->method != Method::Contains && c->method < Method::Trim) return true;
            if (auto* f = as<const FieldExpr>(c->callee); f && has_effect(f->object)) return true;
            return any_effect(c->args);
        }
        case ExprKind::Field: return has_effect(static_cast<const FieldExpr*>(e)->object);
        case ExprKind::Index:
            return has_effect(static_cast<const IndexExpr*>(e)->object) || has_effect(static_cast<const IndexExpr*>(e)->index);
        case ExprKind::Slice: {
            auto* s = static_cast<const SliceExpr*>(e);
            return has_effect(s->object) || has_effect(s->lo) || has_effect(s->hi);
        }
        case ExprKind::Unary: return has_effect(static_cast<const UnaryExpr*>(e)->operand);
        case ExprKind::Binary:
            return has_effect(static_cast<const BinaryExpr*>(e)->lhs) || has_effect(static_cast<const BinaryExpr*>(e)->rhs);
        case ExprKind::Try: return has_effect(static_cast<const TryExpr*>(e)->operand);
        case ExprKind::Paren: return has_effect(static_cast<const ParenExpr*>(e)->inner);
        case ExprKind::Ref: return has_effect(static_cast<const RefExpr*>(e)->operand);
        case ExprKind::StructLit:
            for (const FieldInit& f : static_cast<const StructLitExpr*>(e)->fields) {
                if (has_effect(f.value)) return true;
            }
            return false;
        case ExprKind::VariantLit: {
            auto* v = static_cast<const VariantLitExpr*>(e);
            for (const FieldInit& f : v->fields) {
                if (has_effect(f.value)) return true;
            }
            return any_effect(v->args);
        }
        case ExprKind::ListLit: return any_effect(static_cast<const ListLitExpr*>(e)->items);
        default: return false;
    }
}

const Expr* strip_parens(const Expr* e) {
    while (auto* p = as<const ParenExpr>(e)) e = p->inner;
    return e;
}

class FnLower {
public:
    FnLower(Program& out, sema::Program& prog, const std::map<const sema::FnDef*, uint32_t>& fns,
            std::unordered_map<std::string, uint32_t>& strings, Function& f)
        : out_(out), prog_(prog), tt_(prog.types), fns_(fns), strings_(strings), f_(f) {}

    void run(const sema::FnDef& def) {
        const FnItem* node = def.node;
        f_.def = &def;
        f_.name = def.module->name + "." + def.name;
        f_.det = sema::is_det(def);
        f_.ret = def.ret;
        for (size_t i = 0; i < def.params.size(); i++) {
            const sema::ParamDef& p = def.params[i];
            LocalKind k = p.mode == ParamMode::Value ? LocalKind::Param
                          : p.mode == ParamMode::Ref ? LocalKind::ParamRef
                                                     : LocalKind::ParamMut;
            uint32_t l = add_local(p.type, k, p.name);
            if (i < node->params.size() && node->params[i].local) locals_[node->params[i].local] = l;
        }
        f_.param_count = static_cast<uint32_t>(def.params.size());
        if (def.is_extern) {  // функция C (Р59): только сигнатура
            f_.is_extern = true;
            f_.c_name = def.name;
            f_.slot = def.slot;
            return;
        }
        if (def.exported) {  // функцию зовёт C (Р61)
            f_.exported = true;
            f_.c_name = def.export_name;
        }
        if (def.ret->kind != TK::Void) ret_ = add_local(def.ret, LocalKind::Temp, "");
        cur_ = new_block();
        scopes_.emplace_back();
        for (uint32_t i = 0; i < f_.param_count; i++) {
            if (f_.locals[i].kind == LocalKind::Param && needs_drop(f_.locals[i].type)) {
                scopes_.back().push_back(Cleanup{false, i, nullptr});
            }
        }
        block(node->body);
        Span end = node->body ? Span{node->body->span.file, node->body->span.end - 1, node->body->span.end} : def.span;
        // Конец тела: у функции без результата и у `!void` — возврат, иначе
        // сюда не дойти (E0504).
        if (def.ret->kind == TK::Void) {
            exit_scopes(0, end);
            ret_term(false, end);
        } else if (def.ret->kind == TK::Result && def.ret->elem->kind == TK::Void) {
            Rvalue rv = make(RvKind::MakeOk, def.ret);
            assign(local(ret_), rv, end);
            exit_scopes(0, end);
            ret_term(true, end);
        } else {
            Term t;
            t.kind = TermKind::Unreachable;
            t.span = end;
            terminate(t);
        }
        scopes_.pop_back();
        prune();
    }

private:
    Program& out_;
    sema::Program& prog_;
    sema::TypeTable& tt_;
    const std::map<const sema::FnDef*, uint32_t>& fns_;
    std::unordered_map<std::string, uint32_t>& strings_;
    Function& f_;
    uint32_t cur_ = 0;
    uint32_t ret_ = UINT32_MAX;
    std::map<const sema::Local*, uint32_t> locals_;
    std::map<const sema::Local*, Place> aliases_;  // переменная `for x in &xs` — место xs[i]

    struct Cleanup {
        bool is_defer = false;
        uint32_t local = 0;
        const Stmt* defer = nullptr;
        bool on_error = false;  // errdefer: только на выходе с ошибкой
    };
    // Вид выхода из областей: errdefer выполняется только на ошибке.
    // Dynamic — `return` значения E!T: ошибка или нет, видно во время
    // выполнения по результату.
    enum class Way : uint8_t { Normal, Error, Dynamic };
    std::vector<std::vector<Cleanup>> scopes_;

    struct Loop {
        uint32_t brk = 0, cont = 0;
        size_t depth = 0;  // число областей снаружи тела цикла
    };
    std::vector<Loop> loops_;

    // --- построение

    const Type* prim(TK k) { return tt_.prim(k); }

    uint32_t add_local(const Type* t, LocalKind k, const std::string& name) {
        f_.locals.push_back(Local{name, t, k});
        return static_cast<uint32_t>(f_.locals.size() - 1);
    }

    // Временное: освобождается в конце текущей области.
    uint32_t temp(const Type* t) {
        uint32_t l = add_local(t, LocalKind::Temp, "");
        if (needs_drop(t)) scopes_.back().push_back(Cleanup{false, l, nullptr});
        return l;
    }

    uint32_t var(const sema::Local* sl) {
        uint32_t l = add_local(sl->type, LocalKind::Var, sl->name);
        locals_[sl] = l;
        return l;
    }

    void own(uint32_t l) {
        if (needs_drop(f_.locals[l].type)) scopes_.back().push_back(Cleanup{false, l, nullptr});
    }

    uint32_t new_block() {
        f_.blocks.emplace_back();
        return static_cast<uint32_t>(f_.blocks.size() - 1);
    }

    static Place local(uint32_t l) { return Place{l, {}}; }

    static Place proj(Place p, ProjKind k, uint32_t index = 0, uint32_t sub = 0) {
        p.proj.push_back(Proj{k, index, sub});
        return p;
    }

    void inst(InstKind k, Place dst, Rvalue rv, Span span) {
        bool canon = k == InstKind::Assign && f_.det && makes_float(rv);
        Inst i;
        i.kind = k;
        i.dst = dst;
        i.rv = std::move(rv);
        i.span = span;
        f_.blocks[cur_].insts.push_back(std::move(i));
        if (!canon) return;
        // Класс D: NaN после операции — канонический (Р56).
        Inst c;
        c.kind = InstKind::Assign;
        c.dst = dst;
        c.rv = make(RvKind::Unary, f_.blocks[cur_].insts.back().rv.type);
        c.rv.un = UnKind::Canon;
        c.rv.ops.push_back(copy(std::move(dst)));
        c.span = span;
        f_.blocks[cur_].insts.push_back(std::move(c));
    }

    // Операция, которая может дать NaN с битами процессора: арифметика,
    // смена знака, приведение к плавающему, abs, min и max плавающих.
    static bool makes_float(const Rvalue& rv) {
        if (!rv.type || !sema::is_float(rv.type)) return false;
        switch (rv.kind) {
            case RvKind::Binary: return true;
            case RvKind::Unary: return rv.un == UnKind::Neg;
            case RvKind::Cast: return true;
            case RvKind::Intrinsic:
                return rv.intrinsic == Intrinsic::Abs || rv.intrinsic == Intrinsic::Min || rv.intrinsic == Intrinsic::Max;
            default: return false;
        }
    }

    void assign(Place dst, Rvalue rv, Span span) { inst(InstKind::Assign, std::move(dst), std::move(rv), span); }
    void eval(Rvalue rv, Span span) { inst(InstKind::Eval, Place{}, std::move(rv), span); }

    // Завершает текущий блок; дальше — новый блок (без входов, если в него
    // не перейдут явно).
    void terminate(Term t) {
        f_.blocks[cur_].term = std::move(t);
        cur_ = new_block();
    }

    void jump(uint32_t to, Span span) {
        Term t;
        t.kind = TermKind::Jump;
        t.then_b = to;
        t.span = span;
        terminate(t);
    }

    void branch(Operand c, uint32_t then_b, uint32_t else_b, Span span) {
        Term t;
        t.kind = TermKind::Branch;
        t.cond = rd(std::move(c));
        t.then_b = then_b;
        t.else_b = else_b;
        t.span = span;
        terminate(t);
    }

    void trap(const std::string& message, Span span) {
        Term t;
        t.kind = TermKind::Trap;
        t.message = message;
        t.span = span;
        terminate(t);
    }

    void ret_term(bool value, Span span) {
        Term t;
        t.kind = TermKind::Return;
        t.has_value = value;
        if (value) t.value = Operand{OpKind::Move, local(ret_), {}};
        t.span = span;
        terminate(t);
    }

    // --- операнды и правые части

    static Operand copy(Place p) { return Operand{OpKind::Copy, std::move(p), {}}; }
    static Operand move(Place p) { return Operand{OpKind::Move, std::move(p), {}}; }
    // Операнд для чтения: значение не забирается.
    static Operand rd(Operand o) {
        if (o.kind == OpKind::Move) o.kind = OpKind::Copy;
        return o;
    }

    Operand cint(uint64_t v, const Type* t) {
        Operand o;
        o.c.kind = Const::Kind::Int;
        o.c.i = v;
        o.c.type = t;
        return o;
    }

    Operand cfloat(double v, const Type* t) {
        Operand o;
        o.c.kind = Const::Kind::Float;
        o.c.f = t->kind == TK::F32 ? static_cast<double>(static_cast<float>(v)) : v;
        o.c.type = t;
        return o;
    }

    Operand cbool(bool v) {
        Operand o;
        o.c.kind = Const::Kind::Bool;
        o.c.b = v;
        o.c.type = prim(TK::Bool);
        return o;
    }

    Operand cstr(const std::string& s) {
        Operand o;
        o.c.kind = Const::Kind::Str;
        o.c.type = prim(TK::Str);
        auto [it, fresh] = strings_.try_emplace(s, static_cast<uint32_t>(out_.strings.size()));
        if (fresh) out_.strings.push_back(s);
        o.c.str = it->second;
        return o;
    }

    static Rvalue make(RvKind k, const Type* t) {
        Rvalue rv;
        rv.kind = k;
        rv.type = t;
        return rv;
    }

    static Rvalue use(Operand o, const Type* t) {
        Rvalue rv = make(RvKind::Use, t);
        rv.ops.push_back(std::move(o));
        return rv;
    }

    Rvalue bin(BinKind k, Operand a, Operand b, const Type* t) {
        Rvalue rv = make(RvKind::Binary, t);
        rv.bin = k;
        rv.ops.push_back(rd(std::move(a)));
        rv.ops.push_back(rd(std::move(b)));
        return rv;
    }

    Operand to_temp(Rvalue rv, Span span) {
        const Type* t = rv.type;
        uint32_t l = temp(t);
        assign(local(l), std::move(rv), span);
        return move(local(l));
    }

    const Type* type_of(const Operand& o) { return operand_type(out_, f_, o); }

    bool is_temp_root(const Operand& o) {
        return o.kind != OpKind::Const && f_.locals[o.place.local].kind == LocalKind::Temp;
    }

    // Читает место заранее: дальше в выражении есть действие, которое
    // может его изменить.
    Operand stabilize(Operand o, Span span) {
        if (o.kind == OpKind::Copy && !is_temp_root(o)) return to_temp(use(o, type_of(o)), span);
        return o;
    }

    // Место значения операнда; константа кладётся во временное.
    Place place_of(Operand o, Span span, bool& fresh) {
        if (o.kind == OpKind::Const) {
            o = to_temp(use(o, o.c.type), span);
        }
        fresh = o.kind == OpKind::Move;
        return o.place;
    }

    // --- выражения

    // Значение выражения с неявным преобразованием (Expr::conv).
    Operand expr(const Expr* e) {
        Operand o = raw(e);
        if (e->conv) o = convert(std::move(o), e->type, e->conv, e->span);
        return o;
    }

    // Операнды по порядку: место левее выражения с действием читается сразу.
    std::vector<Operand> exprs(const std::vector<const Expr*>& es) {
        std::vector<Operand> out;
        for (size_t i = 0; i < es.size(); i++) {
            Operand o = expr(es[i]);
            bool later = false;
            for (size_t j = i + 1; j < es.size(); j++) later = later || has_effect(es[j]);
            if (later) o = stabilize(std::move(o), es[i]->span);
            out.push_back(std::move(o));
        }
        return out;
    }

    Operand convert(Operand o, const Type* from, const Type* to, Span span) {
        if (!to || from == to) return o;
        if (from->kind == TK::Ref && to->kind == TK::Ref) {  // Р62: общее начало
            Rvalue rv = make(RvKind::Cast, to);
            rv.cast = CastKind::RefUp;
            rv.ops.push_back(rd(std::move(o)));
            return to_temp(std::move(rv), span);
        }
        if ((is_int(from) && is_int(to)) || (from->kind == TK::F32 && to->kind == TK::F64)) {
            Rvalue rv = make(RvKind::Cast, to);
            rv.cast = CastKind::Widen;
            rv.ops.push_back(rd(std::move(o)));
            return to_temp(std::move(rv), span);
        }
        if ((from->kind == TK::List || from->kind == TK::Array) && to->kind == TK::Slice) {
            bool fresh = false;
            Rvalue rv = make(RvKind::SliceOf, to);
            rv.place = place_of(std::move(o), span, fresh);
            rv.has_place = true;
            rv.ops.push_back(cint(0, prim(TK::I64)));
            return to_temp(std::move(rv), span);
        }
        if (to->kind == TK::Optional && from->kind != TK::Optional) {
            Rvalue rv = make(RvKind::MakeSome, to);
            rv.ops.push_back(convert(std::move(o), from, to->elem, span));
            return to_temp(std::move(rv), span);
        }
        if (to->kind == TK::Result && from->kind != TK::Result) {
            Rvalue rv = make(RvKind::MakeOk, to);
            rv.ops.push_back(convert(std::move(o), from, to->elem, span));
            return to_temp(std::move(rv), span);
        }
        return o;  // других неявных преобразований нет (type.conv)
    }

    static bool is_lvalue(const Expr* e) {
        e = strip_parens(e);
        if (auto* n = as<const NameExpr>(e)) return n->res.kind == ResKind::Local;
        if (auto* f = as<const FieldExpr>(e)) return f->res.kind == ResKind::None && f->field >= 0;
        return e->kind == ExprKind::Index;
    }

    // Место выражения-места: переменная, поле, элемент. Объект, который
    // сам не место, вычисляется во временное (fresh — место во временном).
    Place lvalue(const Expr* e, bool& fresh) {
        e = strip_parens(e);
        if (auto* n = as<const NameExpr>(e)) {
            fresh = false;
            const sema::Local* sl = n->res.local;
            auto a = aliases_.find(sl);
            if (a != aliases_.end()) return a->second;
            return local(locals_.at(sl));
        }
        if (auto* f = as<const FieldExpr>(e)) {
            Place base = object(f->object, fresh);
            return proj(std::move(base), ProjKind::Field, static_cast<uint32_t>(f->field));
        }
        auto* i = static_cast<const IndexExpr*>(e);
        Place base = object(i->object, fresh);
        if (i->object->type->kind == TK::Map) {  // m[k]: ключ во временном (Р54)
            const Type* kt = i->object->type->key;
            uint32_t key = temp(kt);
            assign(local(key), use(rd(expr(i->index)), kt), i->index->span);
            return proj(std::move(base), ProjKind::MapValue, key);
        }
        uint32_t idx = temp(prim(TK::I64));
        assign(local(idx), use(rd(expr(i->index)), prim(TK::I64)), i->index->span);
        return proj(std::move(base), ProjKind::Index, idx);
    }

    Place object(const Expr* e, bool& fresh) {
        if (is_lvalue(e) && !e->conv) return lvalue(e, fresh);
        return place_of(expr(e), e->span, fresh);
    }

    Operand raw(const Expr* e) {
        switch (e->kind) {
            case ExprKind::Int: {
                auto* i = static_cast<const IntExpr*>(e);
                if (sema::is_float(e->type)) return cfloat(static_cast<double>(i->value), e->type);
                return cint(i->value, e->type);
            }
            case ExprKind::Float: return cfloat(static_cast<const FloatExpr*>(e)->value, e->type);
            case ExprKind::Str: return cstr(static_cast<const StrExpr*>(e)->value);
            case ExprKind::Byte: return cint(static_cast<const ByteExpr*>(e)->value, prim(TK::U8));
            case ExprKind::Bool: return cbool(static_cast<const BoolExpr*>(e)->value);
            case ExprKind::None: return to_temp(make(RvKind::MakeNone, e->type), e->span);
            case ExprKind::Name: {
                auto* n = static_cast<const NameExpr*>(e);
                if (n->res.kind == ResKind::Const) return const_operand(n->res.cn->value, n->res.cn->type, e->span);
                if (n->res.kind == ResKind::Fn) return fn_operand(n->res.fn, e->type);
                bool fresh = false;
                return copy(lvalue(e, fresh));
            }
            case ExprKind::Field: {
                auto* f = static_cast<const FieldExpr*>(e);
                if (f->res.kind == ResKind::Const) return const_operand(f->res.cn->value, f->res.cn->type, e->span);
                if (f->res.kind == ResKind::Fn) return fn_operand(f->res.fn, e->type);
                if (f->res.kind == ResKind::Variant) {
                    Rvalue rv = make(RvKind::MakeVariant, e->type);
                    rv.variant = f->res.index;
                    return to_temp(std::move(rv), e->span);
                }
                bool fresh = false;
                Place p = lvalue(e, fresh);
                return fresh ? move(std::move(p)) : copy(std::move(p));
            }
            case ExprKind::Index: {
                bool fresh = false;
                Place p = lvalue(e, fresh);
                return fresh ? move(std::move(p)) : copy(std::move(p));
            }
            case ExprKind::Slice: return slice(static_cast<const SliceExpr*>(e));
            case ExprKind::Call: {
                Rvalue rv;
                uint32_t mark = UINT32_MAX;
                if (!call(static_cast<const CallExpr*>(e), rv, &mark)) return cbool(false);  // assert: значения нет
                if (rv.type->kind == TK::Void) {
                    eval(std::move(rv), e->span);
                    release(mark, e->span);
                    return cbool(false);
                }
                if (rv.kind == RvKind::Use) return std::move(rv.ops[0]);
                Operand out = to_temp(std::move(rv), e->span);
                release(mark, e->span);
                return out;
            }
            case ExprKind::Unary: return unary(static_cast<const UnaryExpr*>(e));
            case ExprKind::Binary: return binary(static_cast<const BinaryExpr*>(e));
            case ExprKind::Try: return try_expr(static_cast<const TryExpr*>(e));
            case ExprKind::Paren: return expr(static_cast<const ParenExpr*>(e)->inner);
            case ExprKind::StructLit: return struct_lit(static_cast<const StructLitExpr*>(e));
            case ExprKind::VariantLit: return variant_lit(static_cast<const VariantLitExpr*>(e));
            case ExprKind::ListLit: {
                auto* l = static_cast<const ListLitExpr*>(e);
                if (e->type->kind == TK::Map) return to_temp(make(RvKind::MakeMap, e->type), e->span);
                Rvalue rv = make(RvKind::MakeList, e->type);
                std::vector<const Expr*> items(l->items.begin(), l->items.end());
                rv.ops = exprs(items);
                return to_temp(std::move(rv), e->span);
            }
            case ExprKind::Ref: {  // Р62: значение `&T` — адрес места или временного литерала
                auto* r = static_cast<const RefExpr*>(e);
                bool fresh = false;
                Rvalue rv = make(RvKind::AddrOf, e->type);
                rv.place = object(r->operand, fresh);
                rv.has_place = true;
                return to_temp(std::move(rv), e->span);
            }
            case ExprKind::Error: break;
        }
        return cbool(false);  // проверка типов не пропускает сюда
    }

    // Функция `@export` как значение (Р61): константа-адрес.
    Operand fn_operand(const sema::FnDef* def, const Type* t) {
        Operand o;
        o.c.kind = Const::Kind::Fn;
        o.c.fn = fns_.at(def);
        o.c.type = t;
        return o;
    }

    Operand const_operand(const sema::ConstValue& v, const Type* t, Span span) {
        switch (v.kind) {
            case sema::ConstValue::Kind::Int:
                if (sema::is_float(t)) return cfloat(static_cast<double>(v.i), t);
                return cint(v.i, t);
            case sema::ConstValue::Kind::Float: return cfloat(v.f, t);
            case sema::ConstValue::Kind::Bool: return cbool(v.b);
            case sema::ConstValue::Kind::Str: return cstr(v.s);
            case sema::ConstValue::Kind::List: {
                Rvalue rv = make(RvKind::MakeList, t);
                for (const sema::ConstValue& item : v.items) rv.ops.push_back(const_operand(item, t->elem, span));
                return to_temp(std::move(rv), span);
            }
            case sema::ConstValue::Kind::None: break;
        }
        return cint(0, t);
    }

    Operand unary(const UnaryExpr* u) {
        const Expr* inner = strip_parens(u->operand);
        if (u->op == UnOp::Neg) {
            if (auto* i = as<const IntExpr>(inner)) {
                if (sema::is_float(u->type)) return cfloat(-static_cast<double>(i->value), u->type);
                return cint(0 - i->value, u->type);
            }
            if (auto* fl = as<const FloatExpr>(inner)) return cfloat(-fl->value, u->type);
        }
        Rvalue rv = make(RvKind::Unary, u->type);
        rv.un = u->op == UnOp::Neg ? UnKind::Neg : u->op == UnOp::Not ? UnKind::Not : UnKind::BitNot;
        rv.ops.push_back(rd(expr(u->operand)));
        return to_temp(std::move(rv), u->span);
    }

    static BinKind bin_kind(BinOp op) {
        switch (op) {
            case BinOp::Add: return BinKind::Add;
            case BinOp::Sub: return BinKind::Sub;
            case BinOp::Mul: return BinKind::Mul;
            case BinOp::Div: return BinKind::Div;
            case BinOp::Rem: return BinKind::Rem;
            case BinOp::AddWrap: return BinKind::AddWrap;
            case BinOp::SubWrap: return BinKind::SubWrap;
            case BinOp::MulWrap: return BinKind::MulWrap;
            case BinOp::BitAnd: return BinKind::BitAnd;
            case BinOp::BitOr: return BinKind::BitOr;
            case BinOp::BitXor: return BinKind::BitXor;
            case BinOp::Shl: return BinKind::Shl;
            case BinOp::Shr: return BinKind::Shr;
            case BinOp::Eq: return BinKind::Eq;
            case BinOp::Ne: return BinKind::Ne;
            case BinOp::Lt: return BinKind::Lt;
            case BinOp::Le: return BinKind::Le;
            case BinOp::Gt: return BinKind::Gt;
            case BinOp::Ge: return BinKind::Ge;
            default: return BinKind::Add;
        }
    }

    Operand binary(const BinaryExpr* b) {
        if (b->op == BinOp::And || b->op == BinOp::Or) {
            // Короткое вычисление: правая часть — только если левой мало.
            uint32_t res = temp(prim(TK::Bool));
            assign(local(res), use(rd(expr(b->lhs)), prim(TK::Bool)), b->lhs->span);
            uint32_t rhs_b = new_block(), done = new_block();
            if (b->op == BinOp::And) branch(copy(local(res)), rhs_b, done, b->op_span);
            else branch(copy(local(res)), done, rhs_b, b->op_span);
            cur_ = rhs_b;
            assign(local(res), use(rd(expr(b->rhs)), prim(TK::Bool)), b->rhs->span);
            jump(done, b->op_span);
            cur_ = done;
            return move(local(res));
        }
        if (b->op == BinOp::Coalesce) {
            // `a ?? b`: b вычисляется, только если у a нет значения.
            bool fresh = false;
            Place a = place_of(expr(b->lhs), b->lhs->span, fresh);
            uint32_t res = temp(b->type);
            Rvalue test = make(RvKind::IsSome, prim(TK::Bool));
            test.place = a;
            test.has_place = true;
            Operand some = to_temp(std::move(test), b->op_span);
            uint32_t some_b = new_block(), none_b = new_block(), done = new_block();
            branch(some, some_b, none_b, b->op_span);
            cur_ = some_b;
            Place inner = proj(a, ProjKind::Unwrap);
            assign(local(res), use(fresh ? move(inner) : copy(inner), b->type), b->span);
            jump(done, b->op_span);
            cur_ = none_b;
            assign(local(res), use(expr(b->rhs), b->type), b->rhs->span);
            jump(done, b->op_span);
            cur_ = done;
            return move(local(res));
        }
        std::vector<Operand> ops = exprs({b->lhs, b->rhs});
        BinKind k = bin_kind(b->op);
        if (k == BinKind::Add && b->operand && b->operand->kind == TK::Str) k = BinKind::Concat;
        Rvalue rv = bin(k, std::move(ops[0]), std::move(ops[1]), b->type);
        Span at = b->op_span;
        return to_temp(std::move(rv), at);
    }

    Operand slice(const SliceExpr* s) {
        bool fresh = false;
        Rvalue rv = make(RvKind::SliceOf, s->type);
        rv.place = object(s->object, fresh);
        rv.has_place = true;
        rv.ops.push_back(s->lo ? rd(expr(s->lo)) : cint(0, prim(TK::I64)));
        if (s->hi) rv.ops.push_back(rd(expr(s->hi)));
        return to_temp(std::move(rv), s->span);
    }

    // `try e`: значение при успехе, иначе — возврат той же ошибки.
    Operand try_expr(const TryExpr* t) {
        bool fresh = false;
        Place r = place_of(expr(t->operand), t->operand->span, fresh);
        Rvalue test = make(RvKind::IsOk, prim(TK::Bool));
        test.place = r;
        test.has_place = true;
        Operand ok = to_temp(std::move(test), t->span);
        uint32_t ok_b = new_block(), err_b = new_block();
        branch(ok, ok_b, err_b, t->span);
        cur_ = err_b;
        Rvalue e = make(RvKind::MakeErr, f_.ret);
        Place err = proj(r, ProjKind::ResultError);
        e.ops.push_back(fresh ? move(err) : copy(err));
        assign(local(ret_), std::move(e), t->span);
        exit_scopes(0, t->span, Way::Error);
        ret_term(true, t->span);
        cur_ = ok_b;
        const Type* rt = place_type(out_, f_, r);
        if (rt->elem->kind == TK::Void) return cbool(false);  // `try` у E!void: значения нет
        Place v = proj(r, ProjKind::ResultValue);
        return fresh ? move(std::move(v)) : copy(std::move(v));
    }

    // Значения полей в порядке записи, затем значения по умолчанию;
    // в правой части — по порядку объявления.
    std::vector<Operand> field_values(const std::vector<FieldInit>& inits, const std::vector<sema::FieldDef>& fields,
                                      Span span) {
        std::vector<const Expr*> es;
        for (const FieldInit& fi : inits) es.push_back(fi.value ? fi.value : fi.shorthand);
        std::vector<Operand> given = exprs(es);
        std::vector<Operand> out(fields.size());
        std::vector<char> have(fields.size(), 0);
        for (size_t i = 0; i < inits.size(); i++) {
            for (size_t k = 0; k < fields.size(); k++) {
                if (fields[k].name == inits[i].name) {
                    out[k] = std::move(given[i]);
                    have[k] = 1;
                }
            }
        }
        for (size_t k = 0; k < fields.size(); k++) {
            if (have[k]) continue;
            if (fields[k].default_value) out[k] = expr(fields[k].default_value);
            else out[k] = to_temp(make(RvKind::Zero, fields[k].type), span);  // Р62: ноль поля `@c`
        }
        return out;
    }

    Operand struct_lit(const StructLitExpr* s) {
        if (s->res.kind == ResKind::Variant) {
            const sema::VariantDef& vd = s->res.en->variants[s->res.index];
            Rvalue rv = make(RvKind::MakeVariant, s->type);
            rv.variant = s->res.index;
            rv.ops = field_values(s->fields, vd.fields, s->span);
            return to_temp(std::move(rv), s->span);
        }
        Rvalue rv = make(RvKind::MakeStruct, s->type);
        rv.ops = field_values(s->fields, s->res.st->fields, s->span);
        if (s->res.st->union_ && !s->fields.empty()) {  // Р62: заданное поле объединения — его номер + 1
            const auto& fs = s->res.st->fields;
            for (size_t k = 0; k < fs.size(); k++) {
                if (fs[k].name == s->fields[0].name) rv.variant = static_cast<uint32_t>(k + 1);
            }
        }
        return to_temp(std::move(rv), s->span);
    }

    Operand variant_lit(const VariantLitExpr* v) {
        const sema::VariantDef& vd = v->res.en->variants[v->res.index];
        Rvalue rv = make(RvKind::MakeVariant, v->type);
        rv.variant = v->res.index;
        if (v->form == VariantForm::Tuple) {
            std::vector<const Expr*> es(v->args.begin(), v->args.end());
            rv.ops = exprs(es);
        } else if (v->form == VariantForm::Record) {
            rv.ops = field_values(v->fields, vd.fields, v->span);
        }
        return to_temp(std::move(rv), v->span);
    }

    // --- вызовы

    // Арена C после вызова (Р62): всё, что выделено после отметки.
    void release(uint32_t mark, Span span) {
        if (mark == UINT32_MAX) return;
        Rvalue rv = intrinsic(Intrinsic::CArenaRelease, prim(TK::Void));
        rv.ops.push_back(copy(local(mark)));
        eval(std::move(rv), span);
    }

    Rvalue intrinsic(Intrinsic k, const Type* t) {
        Rvalue rv = make(RvKind::Intrinsic, t);
        rv.intrinsic = k;
        return rv;
    }

    // Правая часть вызова; false — вызов уже понижен целиком (assert).
    // Параметр, которому нужна арена C на время вызова (Р62): заимствующая
    // структура, срез строк или заимствующих структур, `?&T`.
    static bool arena_param(const Type* t) {
        if (!t) return false;
        if (sema::c_borrowing(t)) return true;
        if (t->kind == TK::Slice) return t->elem->kind == TK::Str || sema::c_borrowing(t->elem);
        return t->kind == TK::Optional && t->elem->kind == TK::Ref;
    }

    // mark — отметка арены C (Р62), если вызову она нужна: освободить после вызова.
    bool call(const CallExpr* c, Rvalue& rv, uint32_t* mark = nullptr) {
        std::vector<const Expr*> args(c->args.begin(), c->args.end());
        switch (c->call) {
            case CallKind::Fn: case CallKind::Ptr: {
                std::vector<std::pair<ParamMode, const Type*>> params;
                if (c->call == CallKind::Fn) {
                    for (const sema::ParamDef& p : c->fn->params) params.emplace_back(p.mode, p.type);
                } else {
                    params = c->callee->type->sig->params;
                }
                bool arena = false;
                for (const auto& p : params) arena = arena || arena_param(p.second);
                if (arena && mark) {
                    *mark = temp(prim(TK::I64));
                    assign(local(*mark), intrinsic(Intrinsic::CArenaMark, prim(TK::I64)), c->span);
                }
                if (c->call == CallKind::Fn) {
                    rv = make(RvKind::Call, c->fn->ret);
                    rv.fn = fns_.at(c->fn);
                } else {  // через указатель (Р61): значение вызываемого — первым
                    rv = make(RvKind::CallPtr, c->callee->type->sig->ret);
                    rv.ops.push_back(rd(expr(c->callee)));
                }
                for (size_t i = 0; i < args.size(); i++) {
                    Arg a;
                    auto* r = as<const RefExpr>(args[i]);
                    bool by_ref = i < params.size() && params[i].first != ParamMode::Value;
                    if (r && by_ref) {
                        bool fresh = false;
                        a.kind = r->mut ? Arg::Kind::Mut : Arg::Kind::Ref;
                        a.place = object(r->operand, fresh);  // Р62: `&S { ... }` — временное
                    } else {
                        a.op = expr(args[i]);
                        bool later = false;
                        for (size_t j = i + 1; j < args.size(); j++) later = later || has_effect(args[j]);
                        if (later) a.op = stabilize(std::move(a.op), args[i]->span);
                    }
                    rv.args.push_back(std::move(a));
                }
                return true;
            }
            case CallKind::Builtin: {
                auto b = static_cast<sema::BuiltinFn>(c->builtin);
                if (b == sema::BuiltinFn::Assert) {
                    Operand cond = rd(expr(args[0]));
                    uint32_t ok_b = new_block(), fail_b = new_block();
                    branch(std::move(cond), ok_b, fail_b, c->span);
                    cur_ = fail_b;
                    trap("assertion failed", c->span);
                    cur_ = ok_b;
                    return false;
                }
                static constexpr Intrinsic kMap[] = {
                    Intrinsic::Len,     Intrinsic::Repeat,  Intrinsic::Min,     Intrinsic::Max,     Intrinsic::Abs,
                    Intrinsic::ParseInt, Intrinsic::IsDigit, Intrinsic::IsAlpha, Intrinsic::IsSpace, Intrinsic::IsAlnum,
                    Intrinsic::Len,     Intrinsic::Print,   Intrinsic::Println, Intrinsic::Eprintln,
                    Intrinsic::CheckedAdd, Intrinsic::CheckedSub, Intrinsic::CheckedMul, Intrinsic::CheckedDiv,
                    Intrinsic::CheckedNeg,
                    Intrinsic::ArgsLen, Intrinsic::ArgAt,   Intrinsic::ReadFile,
                };
                static_assert(sizeof kMap / sizeof kMap[0] == static_cast<size_t>(sema::BuiltinFn::Count));
                rv = intrinsic(kMap[c->builtin], c->type);
                if (b == sema::BuiltinFn::ReadFile) {  // __read_file(path, &mut out): out — изменяемое место
                    rv.ops.push_back(rd(expr(args[0])));
                    bool fresh = false;
                    rv.place = lvalue(static_cast<const RefExpr*>(args[1])->operand, fresh);
                    rv.has_place = true;
                    return true;
                }
                rv.ops = exprs(args);
                for (size_t i = 0; i < rv.ops.size(); i++) {
                    if (!(b == sema::BuiltinFn::Repeat && i == 0)) rv.ops[i] = rd(std::move(rv.ops[i]));
                }
                return true;
            }
            case CallKind::Method: {
                auto* f = static_cast<const FieldExpr*>(c->callee);
                Method m = c->method;
                if (f->object->type->kind == TK::Map) {  // методы Map (Р54)
                    bool changes = m == Method::Remove || m == Method::Clear;
                    rv = intrinsic(m == Method::Get        ? Intrinsic::MapGet
                                   : m == Method::Contains ? Intrinsic::MapContains
                                   : m == Method::Remove   ? Intrinsic::MapRemove
                                   : m == Method::Clear    ? Intrinsic::MapClear
                                                           : Intrinsic::MapKeys,
                                   c->type);
                    if (changes) {
                        bool fresh = false;
                        rv.place = lvalue(f->object, fresh);
                        rv.has_place = true;
                        rv.ops = exprs(args);
                    } else {
                        std::vector<const Expr*> all{f->object};
                        all.insert(all.end(), args.begin(), args.end());
                        rv.ops = exprs(all);
                    }
                    for (Operand& o : rv.ops) o = rd(std::move(o));
                    return true;
                }
                bool mutating = m < Method::Contains;
                static constexpr Intrinsic kMap[] = {
                    Intrinsic::ListPush,  Intrinsic::ListPop,  Intrinsic::ListInsert,    Intrinsic::ListRemove,
                    Intrinsic::ListClear, Intrinsic::ListSort, Intrinsic::ListContains,  Intrinsic::StrTrim,
                    Intrinsic::StrFind,   Intrinsic::StrStartsWith, Intrinsic::StrSplit,
                };
                rv = intrinsic(kMap[static_cast<size_t>(m)], c->type);
                if (mutating) {
                    bool fresh = false;
                    rv.place = lvalue(f->object, fresh);
                    rv.has_place = true;
                    rv.ops = exprs(args);
                    // Значения, которые список сохраняет (push, insert), отдаются целиком.
                    for (size_t i = 0; i < rv.ops.size(); i++) {
                        bool stored = (m == Method::Push && i == 0) || (m == Method::Insert && i == 1);
                        if (!stored) rv.ops[i] = rd(std::move(rv.ops[i]));
                    }
                } else {
                    std::vector<const Expr*> all{f->object};
                    all.insert(all.end(), args.begin(), args.end());
                    rv.ops = exprs(all);
                    for (Operand& o : rv.ops) o = rd(std::move(o));
                }
                return true;
            }
            case CallKind::Convert: case CallKind::Wrap: {
                Operand a = expr(args[0]);
                const Type* from = args[0]->type;
                const Type* to = c->target;
                if (from == to) {
                    rv = use(std::move(a), to);
                    return true;
                }
                rv = make(RvKind::Cast, to);
                rv.cast = c->call == CallKind::Wrap ? CastKind::Wrap : to->kind == TK::Str ? CastKind::ToStr : CastKind::Check;
                rv.ops.push_back(rd(std::move(a)));
                return true;
            }
            case CallKind::Variant: {
                rv = make(RvKind::MakeVariant, c->type);
                rv.variant = c->variant;
                rv.ops = exprs(args);
                return true;
            }
            case CallKind::None: break;
        }
        rv = use(cbool(false), prim(TK::Bool));
        return true;
    }

    // --- области

    void push_scope() { scopes_.emplace_back(); }

    // Очистка одной области: drop и defer в обратном порядке.
    void cleanup(size_t depth, Span span, Way way = Way::Normal) {
        std::vector<Cleanup> items = scopes_[depth];  // defer может открыть новые области
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (!it->is_defer) {
                inst(InstKind::Drop, local(it->local), Rvalue{}, span);
            } else if (!it->on_error || way == Way::Error) {
                stmt(it->defer);
            } else if (way == Way::Dynamic) {  // errdefer — если результат функции ошибка
                Rvalue test = make(RvKind::IsOk, prim(TK::Bool));
                test.place = local(ret_);
                test.has_place = true;
                Operand ok = to_temp(std::move(test), span);
                uint32_t run_b = new_block(), done = new_block();
                branch(ok, done, run_b, span);
                cur_ = run_b;
                stmt(it->defer);
                jump(done, span);
                cur_ = done;
            }
        }
    }

    // Очистка областей глубже depth (выход из них: return, break, continue,
    // fail, ошибка try).
    void exit_scopes(size_t depth, Span span, Way way = Way::Normal) {
        for (size_t d = scopes_.size(); d > depth; d--) cleanup(d - 1, span, way);
    }

    void pop_scope(Span span) {
        cleanup(scopes_.size() - 1, span);
        scopes_.pop_back();
    }

    // --- инструкции

    void block(const ast::Block* b) {
        if (!b) return;
        push_scope();
        for (const Stmt* s : b->stmts) stmt(s);
        Span end{b->span.file, b->span.end > 0 ? b->span.end - 1 : 0, b->span.end};
        pop_scope(end);
    }

    void stmt(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Let: let_stmt(static_cast<const LetStmt*>(s)); break;
            case StmtKind::Assign: assign_stmt(static_cast<const AssignStmt*>(s)); break;
            case StmtKind::Expr: {
                push_scope();
                effect(static_cast<const ExprStmt*>(s)->expr);
                pop_scope(s->span);
                break;
            }
            case StmtKind::If: if_stmt(static_cast<const IfStmt*>(s)); break;
            case StmtKind::While: while_stmt(static_cast<const WhileStmt*>(s)); break;
            case StmtKind::For: for_stmt(static_cast<const ForStmt*>(s)); break;
            case StmtKind::Match: match_stmt(static_cast<const MatchStmt*>(s)); break;
            case StmtKind::Return: return_stmt(static_cast<const ReturnStmt*>(s)); break;
            case StmtKind::Fail: {
                auto* fl = static_cast<const FailStmt*>(s);
                push_scope();
                Rvalue rv = make(RvKind::MakeErr, f_.ret);
                rv.ops.push_back(expr(fl->value));
                assign(local(ret_), std::move(rv), s->span);
                exit_scopes(0, s->span, Way::Error);
                ret_term(true, s->span);
                pop_scope(s->span);
                break;
            }
            case StmtKind::Break: case StmtKind::Continue: {
                const Loop& l = loops_.back();
                exit_scopes(l.depth, s->span);
                jump(s->kind == StmtKind::Break ? l.brk : l.cont, s->span);
                break;
            }
            case StmtKind::Defer: {
                auto* d = static_cast<const DeferStmt*>(s);
                scopes_.back().push_back(Cleanup{true, 0, d->body, d->on_error});
                break;
            }
            case StmtKind::Block: block(static_cast<const BlockStmt*>(s)->block); break;
            case StmtKind::Error: break;
        }
    }

    // Инструкция-вызов: результат, если он есть, освобождается.
    void effect(const Expr* e) {
        if (auto* c = as<const CallExpr>(e)) {
            Rvalue rv;
            uint32_t mark = UINT32_MAX;
            if (call(c, rv, &mark)) eval(std::move(rv), e->span);
            release(mark, e->span);
            return;
        }
        expr(e);
    }

    void let_stmt(const LetStmt* l) {
        if (l->destructure) {
            destructure(l);
            return;
        }
        uint32_t x = l->local ? var(l->local) : add_local(l->value ? l->value->type : prim(TK::Bool), LocalKind::Var, "_");
        if (l->value) {
            push_scope();
            Operand v = expr(l->value);
            assign(local(x), use(std::move(v), f_.locals[x].type), l->span);
            pop_scope(l->span);
        }
        own(x);
    }

    // `let S { f, g: x } = v`: поля значения — в новые переменные.
    void destructure(const LetStmt* l) {
        std::vector<uint32_t> xs;
        push_scope();
        bool fresh = false;
        Place v = place_of(expr(l->value), l->value->span, fresh);
        for (const LetField& f : l->fields) {
            if (!f.local) continue;  // `_`: поле освобождается вместе со значением
            uint32_t x = var(f.local);
            Place p = proj(v, ProjKind::Field, static_cast<uint32_t>(f.index));
            assign(local(x), use(fresh ? move(std::move(p)) : copy(std::move(p)), f_.locals[x].type), f.name_span);
            xs.push_back(x);
        }
        pop_scope(l->span);
        for (uint32_t x : xs) own(x);
    }

    void assign_stmt(const AssignStmt* a) {
        push_scope();
        auto* n = as<const NameExpr>(a->target);
        if (n && n->name == "_" && a->op == AssignOp::Set) {  // `_ = e`: значение освобождается
            effect(a->value);
            pop_scope(a->span);
            return;
        }
        auto* mi = as<const IndexExpr>(strip_parens(a->target));
        if (a->op == AssignOp::Set && mi && mi->object->type->kind == TK::Map) {
            // `m[k] = v` вставляет ключ или заменяет значение (Р54): место
            // отображения и ключ — до значения (expr.order).
            bool mfresh = false;
            Place m = object(mi->object, mfresh);
            const Type* kt = mi->object->type->key;
            uint32_t key = temp(kt);
            assign(local(key), use(rd(expr(mi->index)), kt), mi->index->span);
            Rvalue rv = intrinsic(Intrinsic::MapSet, prim(TK::Void));
            rv.place = std::move(m);
            rv.has_place = true;
            rv.ops.push_back(copy(local(key)));
            rv.ops.push_back(expr(a->value));
            eval(std::move(rv), a->span);
            pop_scope(a->span);
            return;
        }
        bool fresh = false;
        Place p = lvalue(a->target, fresh);
        const Type* t = place_type(out_, f_, p);
        if (a->op == AssignOp::Set) {
            assign(p, use(expr(a->value), t), a->span);
            pop_scope(a->span);
            return;
        }
        // `x op= e` — `x = x op e`; x читается до e.
        Operand cur = copy(p);
        if (has_effect(a->value)) cur = stabilize(std::move(cur), a->target->span);
        Operand rhs = expr(a->value);
        BinKind k = BinKind::Add;
        switch (a->op) {
            case AssignOp::Add: k = t->kind == TK::Str ? BinKind::Concat : BinKind::Add; break;
            case AssignOp::Sub: k = BinKind::Sub; break;
            case AssignOp::Mul: k = BinKind::Mul; break;
            case AssignOp::Div: k = BinKind::Div; break;
            case AssignOp::Rem: k = BinKind::Rem; break;
            case AssignOp::BitAnd: k = BinKind::BitAnd; break;
            case AssignOp::BitOr: k = BinKind::BitOr; break;
            case AssignOp::BitXor: k = BinKind::BitXor; break;
            case AssignOp::Shl: k = BinKind::Shl; break;
            case AssignOp::Shr: k = BinKind::Shr; break;
            case AssignOp::Set: break;
        }
        assign(p, bin(k, std::move(cur), std::move(rhs), t), a->span);
        pop_scope(a->span);
    }

    // Условие: временные условия освобождаются до перехода.
    Operand cond(const Expr* e) {
        push_scope();
        Operand c = rd(expr(e));
        if (c.kind != OpKind::Const && needs_drop(f_.locals[c.place.local].type)) {
            c = rd(to_temp(use(c, prim(TK::Bool)), e->span));  // поле временного: копия до его drop
        }
        pop_scope(e->span);
        return c;
    }

    void if_stmt(const IfStmt* s) {
        uint32_t then_b = new_block(), else_b = new_block(), done = new_block();
        push_scope();
        Place opt;
        bool fresh = false;
        if (s->cond) {
            branch(cond(s->cond), then_b, else_b, s->cond->span);
        } else {
            opt = place_of(expr(s->let_value), s->let_value->span, fresh);
            Rvalue test = make(RvKind::IsSome, prim(TK::Bool));
            test.place = opt;
            test.has_place = true;
            branch(to_temp(std::move(test), s->let_value->span), then_b, else_b, s->let_value->span);
        }
        cur_ = then_b;
        push_scope();
        if (!s->cond && s->let_local) {
            uint32_t v = var(s->let_local);
            Place inner = proj(opt, ProjKind::Unwrap);
            assign(local(v), use(fresh ? move(inner) : copy(inner), s->let_local->type), s->let_span);
            own(v);
        }
        block(s->then);
        pop_scope(s->span);
        jump(done, s->span);
        cur_ = else_b;
        stmt(s->els);
        jump(done, s->span);
        cur_ = done;
        pop_scope(s->span);
    }

    void while_stmt(const WhileStmt* s) {
        uint32_t head = new_block(), body = new_block(), done = new_block();
        jump(head, s->span);
        cur_ = head;
        auto* lit = as<const BoolExpr>(s->cond);
        if (lit && lit->value) jump(body, s->cond->span);
        else branch(cond(s->cond), body, done, s->cond->span);
        cur_ = body;
        loops_.push_back(Loop{done, head, scopes_.size()});
        block(s->body);
        loops_.pop_back();
        jump(head, s->span);
        cur_ = done;
    }

    void for_stmt(const ForStmt* s) {
        push_scope();
        uint32_t head = new_block(), body = new_block(), step = new_block(), done = new_block();
        const Type* i64 = prim(TK::I64);
        if (s->mode == ForMode::Range) {
            // `for i in a..b`: a и b вычисляются раз; i < b, поэтому i + 1 не переполняется.
            const Type* t = s->elem;
            uint32_t i = s->local ? var(s->local) : temp(t);
            assign(local(i), use(rd(expr(s->iter)), t), s->iter->span);
            uint32_t hi = temp(t);
            assign(local(hi), use(rd(expr(s->hi)), t), s->hi->span);
            jump(head, s->span);
            cur_ = head;
            Operand c = to_temp(bin(BinKind::Lt, copy(local(i)), copy(local(hi)), prim(TK::Bool)), s->var_span);
            branch(c, body, done, s->var_span);
            cur_ = body;
            loops_.push_back(Loop{done, step, scopes_.size()});
            block(s->body);
            loops_.pop_back();
            jump(step, s->span);
            cur_ = step;
            assign(local(i), bin(BinKind::AddWrap, copy(local(i)), cint(1, t), t), s->var_span);
            jump(head, s->span);
        } else {
            // `for x in &xs`: x — место xs[k]; длина проверяется на каждом шаге.
            // `for k in &m`: k — элемент снимка m.keys(), ключи по возрастанию (Р54).
            bool fresh = false;
            Place base = object(s->iter, fresh);
            if (s->iter->type->kind == TK::Map) {
                Rvalue keys = intrinsic(Intrinsic::MapKeys, tt_.list(s->iter->type->key));
                keys.ops.push_back(copy(base));
                base = place_of(to_temp(std::move(keys), s->iter->span), s->iter->span, fresh);
            }
            uint32_t k = temp(i64);
            assign(local(k), use(cint(0, i64), i64), s->var_span);
            jump(head, s->span);
            cur_ = head;
            Rvalue len = intrinsic(Intrinsic::Len, i64);
            len.ops.push_back(copy(base));
            Operand n = to_temp(std::move(len), s->iter->span);
            Operand c = to_temp(bin(BinKind::Lt, copy(local(k)), n, prim(TK::Bool)), s->var_span);
            branch(c, body, done, s->var_span);
            cur_ = body;
            if (s->local) aliases_[s->local] = proj(base, ProjKind::Index, k);
            loops_.push_back(Loop{done, step, scopes_.size()});
            block(s->body);
            loops_.pop_back();
            jump(step, s->span);
            cur_ = step;
            assign(local(k), bin(BinKind::AddWrap, copy(local(k)), cint(1, i64), i64), s->var_span);
            jump(head, s->span);
        }
        cur_ = done;
        pop_scope(s->span);
    }

    // --- match

    Operand tag_eq(const Place& p, uint32_t variant, Span span) {
        Rvalue tag = make(RvKind::Tag, prim(TK::U32));
        tag.place = p;
        tag.has_place = true;
        Operand t = to_temp(std::move(tag), span);
        return to_temp(bin(BinKind::Eq, t, cint(variant, prim(TK::U32)), prim(TK::Bool)), span);
    }

    // Проверка образца: при несовпадении — переход в fail, иначе
    // исполнение продолжается в текущем блоке. Имена здесь не связываются.
    void test(const Pat* p, const Place& at, const Type* t, uint32_t fail) {
        auto check = [&](Operand c, bool want) {
            uint32_t next = new_block();
            if (want) branch(std::move(c), next, fail, p->span);
            else branch(std::move(c), fail, next, p->span);
            cur_ = next;
        };
        auto flag = [&](RvKind k) {
            Rvalue rv = make(k, prim(TK::Bool));
            rv.place = at;
            rv.has_place = true;
            return to_temp(std::move(rv), p->span);
        };
        switch (p->kind) {
            case PatKind::Wild: case PatKind::Bind: case PatKind::Error: break;
            case PatKind::Lit: {
                auto* l = static_cast<const LitPat*>(p);
                Operand c;
                if (auto* i = as<const IntExpr>(l->lit)) c = cint(l->negative ? 0 - i->value : i->value, t);
                else if (auto* b = as<const ByteExpr>(l->lit)) c = cint(b->value, t);
                else c = raw(l->lit);
                check(to_temp(bin(BinKind::Eq, copy(at), c, prim(TK::Bool)), p->span), true);
                break;
            }
            case PatKind::None: check(flag(RvKind::IsSome), false); break;
            case PatKind::Wrap: {
                auto* w = static_cast<const WrapPat*>(p);
                if (w->which == WrapKind::Some) {
                    check(flag(RvKind::IsSome), true);
                    test(w->inner, proj(at, ProjKind::Unwrap), t->elem, fail);
                } else if (w->which == WrapKind::Ok) {
                    check(flag(RvKind::IsOk), true);
                    if (t->elem->kind != TK::Void) test(w->inner, proj(at, ProjKind::ResultValue), t->elem, fail);
                } else {
                    check(flag(RvKind::IsOk), false);
                    test(w->inner, proj(at, ProjKind::ResultError), t->err, fail);
                }
                break;
            }
            case PatKind::Variant: {
                auto* v = static_cast<const VariantPat*>(p);
                const sema::VariantDef& vd = v->res.en->variants[v->res.index];
                check(tag_eq(at, v->res.index, p->span), true);
                if (v->form == VariantForm::Tuple) {
                    for (size_t k = 0; k < v->args.size(); k++) {
                        test(v->args[k], proj(at, ProjKind::Payload, v->res.index, static_cast<uint32_t>(k)), vd.types[k],
                             fail);
                    }
                } else if (v->form == VariantForm::Record) {
                    for (const FieldPat& fp : v->fields) {
                        if (!fp.pat) continue;
                        uint32_t k = field_index(vd, fp.name);
                        test(fp.pat, proj(at, ProjKind::Payload, v->res.index, k), vd.fields[k].type, fail);
                    }
                }
                break;
            }
            case PatKind::Or: {
                uint32_t ok = new_block();
                for (const Pat* alt : static_cast<const OrPat*>(p)->alts) {
                    uint32_t next = new_block();
                    test(alt, at, t, next);
                    jump(ok, alt->span);
                    cur_ = next;
                }
                jump(fail, p->span);
                cur_ = ok;
                break;
            }
        }
    }

    static uint32_t field_index(const sema::VariantDef& vd, const std::string& name) {
        for (size_t k = 0; k < vd.fields.size(); k++) {
            if (vd.fields[k].name == name) return static_cast<uint32_t>(k);
        }
        return 0;
    }

    // Связывание имён совпавшего образца: копии частей значения.
    void bind(const Pat* p, const Place& at, const Type* t) {
        auto bind_local = [&](const sema::Local* sl, const Place& from, Span span) {
            if (!sl) return;
            uint32_t v = var(sl);
            assign(local(v), use(copy(from), sl->type), span);
            own(v);
        };
        switch (p->kind) {
            case PatKind::Bind: bind_local(static_cast<const BindPat*>(p)->local, at, p->span); break;
            case PatKind::Wrap: {
                auto* w = static_cast<const WrapPat*>(p);
                if (w->which == WrapKind::Some) bind(w->inner, proj(at, ProjKind::Unwrap), t->elem);
                else if (w->which == WrapKind::Ok && t->elem->kind != TK::Void) bind(w->inner, proj(at, ProjKind::ResultValue), t->elem);
                else if (w->which == WrapKind::Err) bind(w->inner, proj(at, ProjKind::ResultError), t->err);
                break;
            }
            case PatKind::Variant: {
                auto* v = static_cast<const VariantPat*>(p);
                const sema::VariantDef& vd = v->res.en->variants[v->res.index];
                if (v->form == VariantForm::Tuple) {
                    for (size_t k = 0; k < v->args.size(); k++) {
                        bind(v->args[k], proj(at, ProjKind::Payload, v->res.index, static_cast<uint32_t>(k)), vd.types[k]);
                    }
                } else if (v->form == VariantForm::Record) {
                    for (const FieldPat& fp : v->fields) {
                        uint32_t k = field_index(vd, fp.name);
                        Place fpl = proj(at, ProjKind::Payload, v->res.index, k);
                        if (fp.pat) bind(fp.pat, fpl, vd.fields[k].type);
                        else bind_local(fp.local, fpl, fp.name_span);
                    }
                }
                break;
            }
            default: break;  // `_`, литералы, `none`, `|` имён не связывают
        }
    }

    void match_stmt(const MatchStmt* s) {
        push_scope();
        bool fresh = false;
        Place subj = object(s->subject, fresh);
        const Type* t = place_type(out_, f_, subj);
        uint32_t done = new_block();
        for (const MatchArm& a : s->arms) {
            uint32_t next = new_block();
            test(a.pat, subj, t, next);
            push_scope();
            bind(a.pat, subj, t);
            stmt(a.body);
            pop_scope(a.span);
            jump(done, a.span);
            cur_ = next;
        }
        Term u;
        u.kind = TermKind::Unreachable;  // ветви покрывают все значения (E0415)
        u.span = s->span;
        terminate(u);
        cur_ = done;
        pop_scope(s->span);
    }

    void return_stmt(const ReturnStmt* s) {
        push_scope();
        bool value = f_.ret->kind != TK::Void;
        Way way = Way::Normal;
        if (s->value) {
            assign(local(ret_), use(expr(s->value), f_.ret), s->span);
            // Значение E!T без обёртки ok: может быть и ошибкой.
            if (s->value->type && s->value->type->kind == TK::Result && !s->value->conv) way = Way::Dynamic;
        } else if (value) {  // `return` в функции `E!void`
            assign(local(ret_), make(RvKind::MakeOk, f_.ret), s->span);
        }
        exit_scopes(0, s->span, way);
        ret_term(value, s->span);
        pop_scope(s->span);
    }

    // --- недостижимые блоки

    void prune() {
        size_t n = f_.blocks.size();
        std::vector<char> seen(n, 0);
        std::vector<uint32_t> work{0};
        seen[0] = 1;
        while (!work.empty()) {
            uint32_t b = work.back();
            work.pop_back();
            const Term& t = f_.blocks[b].term;
            auto visit = [&](uint32_t to) {
                if (!seen[to]) {
                    seen[to] = 1;
                    work.push_back(to);
                }
            };
            if (t.kind == TermKind::Jump) visit(t.then_b);
            if (t.kind == TermKind::Branch) {
                visit(t.then_b);
                visit(t.else_b);
            }
        }
        std::vector<uint32_t> map(n, 0);
        std::vector<Block> kept;
        for (size_t b = 0; b < n; b++) {
            if (!seen[b]) continue;
            map[b] = static_cast<uint32_t>(kept.size());
            kept.push_back(std::move(f_.blocks[b]));
        }
        for (Block& b : kept) {
            b.term.then_b = map[b.term.then_b];
            b.term.else_b = map[b.term.else_b];
        }
        f_.blocks = std::move(kept);
    }
};

}  // namespace

std::unique_ptr<Program> lower(sema::Program& prog) {
    auto out = std::make_unique<Program>();
    out->types = &prog.types;
    std::map<const sema::FnDef*, uint32_t> fns;
    std::vector<const sema::FnDef*> order;
    for (auto& m : prog.modules) {
        for (Item* it : m->ast->items) {
            if (it->kind != ItemKind::Fn) continue;
            auto s = m->scope.find(it->name);
            if (s == m->scope.end() || s->second.kind != sema::SymKind::Fn || s->second.fn->node != it) continue;
            fns[s->second.fn] = static_cast<uint32_t>(order.size());
            order.push_back(s->second.fn);
            if (m.get() == prog.root && it->name == "main") out->main_fn = static_cast<int>(order.size() - 1);
        }
    }
    out->fns.resize(order.size());
    std::unordered_map<std::string, uint32_t> strings;  // строка → номер в out->strings
    for (size_t i = 0; i < order.size(); i++) FnLower(*out, prog, fns, strings, out->fns[i]).run(*order[i]);
    for (const sema::FnDef* f : order) {
        if (!f->is_extern || f->link.empty()) continue;
        if (std::find(out->links.begin(), out->links.end(), f->link) == out->links.end()) out->links.push_back(f->link);
    }
    return out;
}

}  // namespace dp::ir
