// Верификатор Core IR.
#include "ir/verify.h"

namespace dp::ir {

using sema::TK;

namespace {

class Verifier {
public:
    Verifier(const Program& p, const Function& f, std::vector<std::string>& errors)
        : p_(p), f_(f), errors_(errors) {}

    void run() {
        if (f_.param_count > f_.locals.size()) return fail("more parameters than locals");
        for (size_t i = 0; i < f_.locals.size(); i++) {
            const Local& l = f_.locals[i];
            bool param = l.kind == LocalKind::Param || l.kind == LocalKind::ParamRef || l.kind == LocalKind::ParamMut;
            if (param != (i < f_.param_count)) fail("local %" + std::to_string(i) + " has a wrong kind");
            if (!l.type) fail("local %" + std::to_string(i) + " has no type");
        }
        if (!f_.ret) return fail("no result type");
        if (f_.is_extern) {  // функция C: только сигнатура (Р59)
            if (!f_.blocks.empty()) fail("extern function with blocks");
            if (f_.c_name.empty()) fail("extern function without a C name");
            return;
        }
        if (f_.blocks.empty()) return fail("no blocks");
        for (size_t b = 0; b < f_.blocks.size(); b++) {
            block_ = b;
            for (const Inst& i : f_.blocks[b].insts) inst(i);
            term(f_.blocks[b].term);
        }
    }

private:
    const Program& p_;
    const Function& f_;
    std::vector<std::string>& errors_;
    size_t block_ = 0;

    void fail(const std::string& msg) { errors_.push_back(f_.name + ": bb" + std::to_string(block_) + ": " + msg); }

    const Type* prim(TK k) { return p_.types->prim(k); }

    const Type* place(const Place& pl) {
        const Type* t = place_type(p_, f_, pl);
        if (!t) fail("invalid place %" + std::to_string(pl.local));
        return t;
    }

    // Место, в которое можно писать: переменная, временное, параметр &mut;
    // путь не идёт через элемент среза или байт строки.
    bool writable(const Place& pl) {
        if (pl.local >= f_.locals.size()) return false;
        LocalKind k = f_.locals[pl.local].kind;
        // Данные Mutex (Р70) пишут в блоке `lock` и через `&`: доступ
        // исключителен по захвату, а не по ссылке.
        bool locked = false;
        for (const Proj& pr : pl.proj) locked = locked || pr.kind == ProjKind::MutexValue;
        if (k != LocalKind::Var && k != LocalKind::Temp && k != LocalKind::ParamMut && !locked) return false;
        const Type* t = f_.locals[pl.local].type;
        for (size_t i = 0; i < pl.proj.size() && t; i++) {
            if (pl.proj[i].kind == ProjKind::Index && (t->kind == TK::Slice || t->kind == TK::Str)) return false;
            if (pl.proj[i].kind == ProjKind::MapKeyAt) return false;  // ключ пары не меняют
            if (pl.proj[i].kind == ProjKind::Field && t->kind == TK::PoolHandle) return false;  // Р67
            Place prefix{pl.local, std::vector<Proj>(pl.proj.begin(), pl.proj.begin() + static_cast<long>(i) + 1)};
            t = place_type(p_, f_, prefix);
        }
        return true;
    }

    // Сигнатура функции g — та же, что у типа `extern fn(...)` (Р61).
    static bool fits(const Function& g, const sema::CFnSig& sig) {
        if (g.param_count != sig.params.size() || g.ret != sig.ret) return false;
        for (uint32_t i = 0; i < g.param_count; i++) {
            LocalKind want = sig.params[i].first == ast::ParamMode::Value ? LocalKind::Param
                             : sig.params[i].first == ast::ParamMode::Ref ? LocalKind::ParamRef
                                                                          : LocalKind::ParamMut;
            if (g.locals[i].kind != want || g.locals[i].type != sig.params[i].second) return false;
        }
        return true;
    }

    // stored — значение сохраняется (там допустим move).
    const Type* operand(const Operand& o, bool stored) {
        if (o.kind == OpKind::Const) {
            const Type* t = o.c.type;
            if (!t) {
                fail("constant without a type");
                return nullptr;
            }
            bool ok = (o.c.kind == Const::Kind::Int && sema::is_integer(t)) ||
                      (o.c.kind == Const::Kind::Float && sema::is_float(t)) ||
                      (o.c.kind == Const::Kind::Bool && t->kind == TK::Bool) ||
                      (o.c.kind == Const::Kind::Str && t->kind == TK::Str && o.c.str < p_.strings.size()) ||
                      (o.c.kind == Const::Kind::Fn && t->kind == TK::CFn && o.c.fn < p_.fns.size() &&
                       p_.fns[o.c.fn].exported && fits(p_.fns[o.c.fn], *t->sig));
            if (!ok) fail("constant does not match its type");
            return t;
        }
        if (o.kind == OpKind::Move) {
            if (!stored) fail("move where the value is only read");
            if (o.place.local < f_.locals.size() && f_.locals[o.place.local].kind != LocalKind::Temp) {
                fail("move from a non-temporary %" + std::to_string(o.place.local));
            }
        }
        return place(o.place);
    }

    void same(const Type* got, const Type* want, const char* what) {
        if (got && want && got != want) {
            fail(std::string(what) + ": '" + sema::type_name(got) + "' where '" + sema::type_name(want) + "' is expected");
        }
    }

    void count(const Rvalue& rv, size_t n) {
        if (rv.ops.size() != n) fail("wrong number of operands");
    }

    bool numeric(const Type* t) { return t && sema::is_numeric(t); }
    bool integer(const Type* t) { return t && sema::is_integer(t); }

    void binary(const Rvalue& rv) {
        count(rv, 2);
        if (rv.ops.size() != 2) return;
        const Type* a = operand(rv.ops[0], false);
        const Type* b = operand(rv.ops[1], false);
        if (!a || !b) return;
        switch (rv.bin) {
            case BinKind::Add: case BinKind::Sub: case BinKind::Mul: case BinKind::Div:
                if (!numeric(a)) fail("arithmetic on a non-number");
                same(b, a, "operand");
                same(rv.type, a, "result");
                break;
            case BinKind::Rem: case BinKind::AddWrap: case BinKind::SubWrap: case BinKind::MulWrap:
            case BinKind::BitAnd: case BinKind::BitOr: case BinKind::BitXor:
                if (!integer(a)) fail("integer operation on a non-integer");
                same(b, a, "operand");
                same(rv.type, a, "result");
                break;
            case BinKind::Shl: case BinKind::Shr:
                if (!integer(a) || !integer(b)) fail("shift of non-integers");
                same(rv.type, a, "result");
                break;
            case BinKind::Eq: case BinKind::Ne: case BinKind::Lt: case BinKind::Le: case BinKind::Gt: case BinKind::Ge:
                same(b, a, "operand");
                same(rv.type, prim(TK::Bool), "result");
                if (a->kind == TK::Result) fail("comparison of results");
                break;
            case BinKind::Concat:
                same(a, prim(TK::Str), "operand");
                same(b, prim(TK::Str), "operand");
                same(rv.type, prim(TK::Str), "result");
                break;
        }
    }

    void cast(const Rvalue& rv) {
        count(rv, 1);
        if (rv.ops.empty()) return;
        const Type* from = operand(rv.ops[0], false);
        const Type* to = rv.type;
        if (!from) return;
        bool ok = false;
        switch (rv.cast) {
            case CastKind::Widen:
                if (integer(from) && integer(to)) {
                    bool sf = sema::is_signed(from), st = sema::is_signed(to);
                    int bf = sema::int_bits(from), bt = sema::int_bits(to);
                    ok = sf == st ? bf < bt : (!sf && st && bf < bt);
                } else {
                    ok = from->kind == TK::F32 && to->kind == TK::F64;
                }
                break;
            case CastKind::Check: ok = numeric(from) && numeric(to) && from != to; break;
            case CastKind::Wrap: ok = integer(from) && integer(to) && from != to; break;
            case CastKind::ToStr:
                ok = to->kind == TK::Str && (numeric(from) || from->kind == TK::Bool);
                break;
            case CastKind::RefUp:  // Р62
                ok = from->kind == TK::Ref && to->kind == TK::Ref && sema::c_prefix(to->elem, from->elem);
                break;
        }
        if (!ok) fail("invalid cast from '" + sema::type_name(from) + "' to '" + sema::type_name(to) + "'");
    }

    const Type* list_receiver(const Rvalue& rv) {
        if (!rv.has_place) {
            fail("list method without a receiver");
            return nullptr;
        }
        const Type* t = place(rv.place);
        if (!t || t->kind != TK::List) {
            fail("list method on a non-List");
            return nullptr;
        }
        if (!writable(rv.place)) fail("list method on a read-only place");
        return t;
    }

    const Type* map_receiver(const Rvalue& rv) {
        if (!rv.has_place) {
            fail("map method without a receiver");
            return nullptr;
        }
        const Type* t = place(rv.place);
        if (!t || t->kind != TK::Map) {
            fail("map method on a non-Map");
            return nullptr;
        }
        if (!writable(rv.place)) fail("map method on a read-only place");
        return t;
    }

    // Получатель Atomic[T] (Р70): место, можно и `&` — общий доступ.
    const Type* atomic_receiver(const Rvalue& rv) {
        const Type* t = rv.has_place ? place(rv.place) : nullptr;
        if (!t || t->kind != TK::Atomic) {
            fail("atomic operation on a non-Atomic");
            return nullptr;
        }
        return t;
    }

    const Type* pool_receiver(const Rvalue& rv) {
        if (!rv.has_place) {
            fail("pool method without a receiver");
            return nullptr;
        }
        const Type* t = place(rv.place);
        if (!t || t->kind != TK::Pool) {
            fail("pool method on a non-Pool");
            return nullptr;
        }
        if (!writable(rv.place)) fail("pool method on a read-only place");
        return t;
    }

    // Читающий метод Pool: первый операнд — Pool.
    const Type* pool_operand(const Type* t) {
        if (t && t->kind != TK::Pool) {
            fail("pool method on a non-Pool");
            return nullptr;
        }
        return t;
    }

    // Хэндл пула p (Р67): Handle[T] того же T.
    void handle_of(const Type* h, const Type* p, const char* what) {
        if (h && p && (h->kind != TK::PoolHandle || h->elem != p->elem)) {
            fail(std::string(what) + " is not a handle of the pool");
        }
    }

    // Читающий метод Map: первый операнд — Map.
    const Type* map_operand(const Type* t) {
        if (t && t->kind != TK::Map) {
            fail("map method on a non-Map");
            return nullptr;
        }
        return t;
    }

    void intrinsic(const Rvalue& rv) {
        auto op = [&](size_t i, bool stored = false) -> const Type* {
            return i < rv.ops.size() ? operand(rv.ops[i], stored) : nullptr;
        };
        auto args = [&](size_t n) { count(rv, n); };
        const Type* t0 = nullptr;
        switch (rv.intrinsic) {
            case Intrinsic::Len:
                args(1);
                t0 = op(0);
                if (t0 && t0->kind != TK::List && t0->kind != TK::Slice && t0->kind != TK::Str && t0->kind != TK::Map &&
                    t0->kind != TK::Pool && t0->kind != TK::Array) {
                    fail("len of a non-sequence");
                }
                same(rv.type, prim(TK::I64), "result");
                break;
            case Intrinsic::Repeat:
                args(2);
                t0 = op(0, true);
                if (!integer(op(1))) fail("repeat count is not an integer");
                if (rv.type->kind != TK::List || rv.type->elem != t0) fail("repeat result is not a List of the value");
                break;
            case Intrinsic::Min: case Intrinsic::Max:
                args(2);
                t0 = op(0);
                if (!numeric(t0)) fail("min/max of non-numbers");
                same(op(1), t0, "operand");
                same(rv.type, t0, "result");
                break;
            case Intrinsic::Abs:
                args(1);
                t0 = op(0);
                if (!t0 || !((integer(t0) && sema::is_signed(t0)) || sema::is_float(t0))) fail("abs of an unsigned value");
                same(rv.type, t0, "result");
                break;
            case Intrinsic::CheckedAdd: case Intrinsic::CheckedSub: case Intrinsic::CheckedMul:
            case Intrinsic::CheckedDiv:
                args(2);
                t0 = op(0);
                if (!integer(t0)) fail("checked arithmetic on non-integers");
                same(op(1), t0, "operand");
                if (rv.type->kind != TK::Optional || rv.type->elem != t0) fail("checked arithmetic result is not ?T");
                break;
            case Intrinsic::CheckedNeg:
                args(1);
                t0 = op(0);
                if (!integer(t0) || !sema::is_signed(t0)) fail("checked_neg of an unsigned value");
                if (rv.type->kind != TK::Optional || rv.type->elem != t0) fail("checked_neg result is not ?T");
                break;
            case Intrinsic::ParseInt:
                args(1);
                same(op(0), prim(TK::Str), "operand");
                if (rv.type->kind != TK::Optional || rv.type->elem != prim(TK::I64)) fail("parse_int result is not ?i64");
                break;
            case Intrinsic::IsDigit: case Intrinsic::IsAlpha: case Intrinsic::IsSpace: case Intrinsic::IsAlnum:
                args(1);
                same(op(0), prim(TK::U8), "operand");
                same(rv.type, prim(TK::Bool), "result");
                break;
            case Intrinsic::Print: case Intrinsic::Println: case Intrinsic::Eprintln:
                args(1);
                same(op(0), prim(TK::Str), "operand");
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::ListPush:
                args(1);
                if (const Type* l = list_receiver(rv)) same(op(0, true), l->elem, "operand");
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::ListPop:
                args(0);
                if (const Type* l = list_receiver(rv)) {
                    if (rv.type->kind != TK::Optional || rv.type->elem != l->elem) fail("pop result is not ?T");
                }
                break;
            case Intrinsic::ListInsert:
                args(2);
                same(op(0), prim(TK::I64), "operand");
                if (const Type* l = list_receiver(rv)) same(op(1, true), l->elem, "operand");
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::ListRemove:
                args(1);
                same(op(0), prim(TK::I64), "operand");
                if (const Type* l = list_receiver(rv)) same(rv.type, l->elem, "result");
                break;
            case Intrinsic::ListClear: case Intrinsic::ListSort:
                args(0);
                list_receiver(rv);
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::ListReserve:
                args(1);
                same(op(0), prim(TK::I64), "operand");
                list_receiver(rv);
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::MapReserve:
                args(1);
                same(op(0), prim(TK::I64), "operand");
                map_receiver(rv);
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::ListContains:
                args(2);
                t0 = op(0);
                if (t0 && t0->kind != TK::List && t0->kind != TK::Slice) fail("contains on a non-List");
                else if (t0) same(op(1), t0->elem, "operand");
                same(rv.type, prim(TK::Bool), "result");
                break;
            case Intrinsic::MapSet:
                args(2);
                if (const Type* m = map_receiver(rv)) {
                    same(op(0), m->key, "operand");
                    same(op(1, true), m->elem, "operand");
                }
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::MapRemove:
                args(1);
                if (const Type* m = map_receiver(rv)) {
                    same(op(0), m->key, "operand");
                    if (rv.type->kind != TK::Optional || rv.type->elem != m->elem) fail("remove result is not ?V");
                }
                break;
            case Intrinsic::MapClear:
                args(0);
                map_receiver(rv);
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::MapGet: case Intrinsic::MapContains:
                args(2);
                if (const Type* m = map_operand(op(0))) {
                    same(op(1), m->key, "operand");
                    if (rv.intrinsic == Intrinsic::MapContains) same(rv.type, prim(TK::Bool), "result");
                    else if (rv.type->kind != TK::Optional || rv.type->elem != m->elem) fail("get result is not ?V");
                }
                break;
            case Intrinsic::CArenaMark:  // Р62
                args(0);
                same(rv.type, prim(TK::I64), "result");
                break;
            case Intrinsic::CArenaRelease:
                args(1);
                same(op(0), prim(TK::I64), "arena mark");
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::Probe:
                args(0);
                same(rv.type, prim(TK::Void), "result");
                if (!rv.has_place) fail("probe without a place");
                else place(rv.place);
                break;
            case Intrinsic::MapKeys:
                args(1);
                if (const Type* m = map_operand(op(0))) {
                    if (rv.type->kind != TK::List || rv.type->elem != m->key) fail("keys result is not List[K]");
                }
                break;
            case Intrinsic::PoolAdd:
                args(1);
                if (const Type* p = pool_receiver(rv)) {
                    same(op(0, true), p->elem, "operand");
                    handle_of(rv.type, p, "add result");
                }
                break;
            case Intrinsic::PoolRemove:
                args(1);
                if (const Type* p = pool_receiver(rv)) {
                    handle_of(op(0), p, "operand");
                    if (rv.type->kind != TK::Optional || rv.type->elem != p->elem) fail("remove result is not ?T");
                }
                break;
            case Intrinsic::PoolClear:
                args(0);
                pool_receiver(rv);
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::PoolReserve:
                args(1);
                same(op(0), prim(TK::I64), "operand");
                pool_receiver(rv);
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::PoolGet: case Intrinsic::PoolContains:
                args(2);
                if (const Type* p = pool_operand(op(0))) {
                    handle_of(op(1), p, "operand");
                    if (rv.intrinsic == Intrinsic::PoolContains) same(rv.type, prim(TK::Bool), "result");
                    else if (rv.type->kind != TK::Optional || rv.type->elem != p->elem) fail("get result is not ?T");
                }
                break;
            case Intrinsic::PoolHandles:
                args(1);
                if (const Type* p = pool_operand(op(0))) {
                    if (rv.type->kind != TK::List) fail("handles result is not a List");
                    else handle_of(rv.type->elem, p, "handles element");
                }
                break;
            case Intrinsic::AtomicNew:
                args(1);
                t0 = op(0);
                if (rv.type->kind != TK::Atomic || rv.type->elem != t0) fail("atomic of a wrong type");
                break;
            case Intrinsic::AtomicLoad:
                args(0);
                if (const Type* a = atomic_receiver(rv)) same(rv.type, a->elem, "result");
                break;
            case Intrinsic::AtomicStore:
                args(1);
                if (const Type* a = atomic_receiver(rv)) same(op(0), a->elem, "operand");
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::AtomicAdd: case Intrinsic::AtomicSub: case Intrinsic::AtomicSwap:
                args(1);
                if (const Type* a = atomic_receiver(rv)) {
                    same(op(0), a->elem, "operand");
                    same(rv.type, a->elem, "result");
                    if (rv.intrinsic != Intrinsic::AtomicSwap && !integer(a->elem)) fail("atomic arithmetic on a bool");
                }
                break;
            case Intrinsic::AtomicCas:
                args(2);
                if (const Type* a = atomic_receiver(rv)) {
                    same(op(0), a->elem, "operand");
                    same(op(1), a->elem, "operand");
                }
                same(rv.type, prim(TK::Bool), "result");
                break;
            case Intrinsic::MutexNew:
                args(1);
                t0 = op(0, true);
                if (rv.type->kind != TK::Mutex || rv.type->elem != t0) fail("mutex of a wrong type");
                break;
            case Intrinsic::ChannelNew:
                args(1);
                same(op(0), prim(TK::I64), "capacity");
                if (rv.type->kind != TK::Channel) fail("channel of a wrong type");
                break;
            case Intrinsic::ChannelSend: case Intrinsic::ChannelRecv: case Intrinsic::ChannelTryRecv:
            case Intrinsic::ChannelClose: {
                const Type* ch = rv.has_place ? place(rv.place) : nullptr;
                if (!ch || ch->kind != TK::Channel) {
                    fail("channel operation on a non-Channel");
                    break;
                }
                if (rv.intrinsic == Intrinsic::ChannelSend) {
                    args(1);
                    same(op(0, true), ch->elem, "operand");
                    same(rv.type, prim(TK::Void), "result");
                } else if (rv.intrinsic == Intrinsic::ChannelClose) {
                    args(0);
                    same(rv.type, prim(TK::Void), "result");
                } else {
                    args(0);
                    if (rv.type->kind != TK::Optional || rv.type->elem != ch->elem) fail("recv result is not ?T");
                }
                break;
            }
            case Intrinsic::TasksBegin: case Intrinsic::TasksEnd:
                args(0);
                same(rv.type, prim(TK::Void), "result");
                break;
            case Intrinsic::MutexLock: case Intrinsic::MutexUnlock: {
                args(0);
                const Type* m = rv.has_place ? place(rv.place) : nullptr;
                if (!m || m->kind != TK::Mutex) fail("lock of a non-Mutex");
                same(rv.type, prim(TK::Void), "result");
                break;
            }
            case Intrinsic::PoolSlots:
                args(1);
                pool_operand(op(0));
                same(rv.type, prim(TK::I64), "result");
                break;
            case Intrinsic::PoolLive:
                args(2);
                pool_operand(op(0));
                same(op(1), prim(TK::I64), "operand");
                same(rv.type, prim(TK::Bool), "result");
                break;
            case Intrinsic::MapOrder:
                args(1);
                if (map_operand(op(0)) && (rv.type->kind != TK::List || rv.type->elem->kind != TK::I64)) {
                    fail("order result is not List[i64]");
                }
                break;
            case Intrinsic::StrTrim:
                args(1);
                same(op(0), prim(TK::Str), "operand");
                same(rv.type, prim(TK::Str), "result");
                break;
            case Intrinsic::StrFind:
                args(2);
                same(op(0), prim(TK::Str), "operand");
                same(op(1), prim(TK::Str), "operand");
                if (rv.type->kind != TK::Optional || rv.type->elem != prim(TK::I64)) fail("find result is not ?i64");
                break;
            case Intrinsic::StrStartsWith:
                args(2);
                same(op(0), prim(TK::Str), "operand");
                same(op(1), prim(TK::Str), "operand");
                same(rv.type, prim(TK::Bool), "result");
                break;
            case Intrinsic::StrSplit:
                args(2);
                same(op(0), prim(TK::Str), "operand");
                same(op(1), prim(TK::Str), "operand");
                if (rv.type->kind != TK::List || rv.type->elem != prim(TK::Str)) fail("split result is not List[str]");
                break;
            case Intrinsic::ArgsLen:
                args(0);
                same(rv.type, prim(TK::I64), "result");
                break;
            case Intrinsic::ArgAt:
                args(1);
                same(op(0), prim(TK::I64), "operand");
                same(rv.type, prim(TK::Str), "result");
                break;
            case Intrinsic::ReadFile:
                args(1);
                same(op(0), prim(TK::Str), "operand");
                same(rv.type, prim(TK::I32), "result");
                if (!rv.has_place) {
                    fail("read_file without a place for the text");
                } else {
                    same(place(rv.place), prim(TK::Str), "read_file place");
                    if (!writable(rv.place)) fail("read_file into a read-only place");
                }
                break;
        }
        if (rv.has_place && rv.intrinsic != Intrinsic::ListPush && rv.intrinsic != Intrinsic::ListPop &&
            rv.intrinsic != Intrinsic::ReadFile &&
            rv.intrinsic != Intrinsic::ListInsert && rv.intrinsic != Intrinsic::ListRemove &&
            rv.intrinsic != Intrinsic::ListClear && rv.intrinsic != Intrinsic::ListSort &&
            rv.intrinsic != Intrinsic::ListReserve && rv.intrinsic != Intrinsic::MapReserve &&
            rv.intrinsic != Intrinsic::PoolAdd && rv.intrinsic != Intrinsic::PoolRemove &&
            rv.intrinsic != Intrinsic::PoolClear && rv.intrinsic != Intrinsic::PoolReserve &&
            rv.intrinsic != Intrinsic::MapSet && rv.intrinsic != Intrinsic::MapRemove &&
            rv.intrinsic != Intrinsic::MapClear && rv.intrinsic != Intrinsic::Probe &&
            (rv.intrinsic < Intrinsic::AtomicLoad || rv.intrinsic > Intrinsic::AtomicCas) &&
            rv.intrinsic != Intrinsic::MutexLock && rv.intrinsic != Intrinsic::MutexUnlock &&
            (rv.intrinsic < Intrinsic::ChannelSend || rv.intrinsic > Intrinsic::ChannelClose)) {
            fail("receiver place on a method that only reads");
        }
    }

    void rvalue(const Rvalue& rv) {
        const Type* t = rv.type;
        if (!t) return fail("rvalue without a type");
        auto stored = [&](size_t i, const Type* want) {
            if (i < rv.ops.size()) same(operand(rv.ops[i], true), want, "operand");
        };
        switch (rv.kind) {
            case RvKind::Use: count(rv, 1); stored(0, t); break;
            case RvKind::Binary: binary(rv); break;
            case RvKind::Unary: {
                count(rv, 1);
                const Type* a = rv.ops.empty() ? nullptr : operand(rv.ops[0], false);
                if (!a) break;
                same(t, a, "result");
                bool ok = rv.un == UnKind::Not      ? a->kind == TK::Bool
                          : rv.un == UnKind::BitNot ? integer(a)
                          : rv.un == UnKind::Canon  ? sema::is_float(a)
                                                    : (integer(a) && sema::is_signed(a)) || sema::is_float(a);
                if (!ok) fail("invalid unary operand");
                break;
            }
            case RvKind::Cast: cast(rv); break;
            case RvKind::MakeStruct:
                if (t->kind != TK::Struct) return fail("struct value of a non-struct type");
                count(rv, t->sdef->fields.size());
                for (size_t i = 0; i < t->sdef->fields.size(); i++) stored(i, t->sdef->fields[i].type);
                break;
            case RvKind::MakeVariant: {
                if (t->kind != TK::Enum || rv.variant >= t->edef->variants.size()) return fail("invalid variant");
                const sema::VariantDef& v = t->edef->variants[rv.variant];
                size_t n = v.form == ast::VariantForm::Tuple ? v.types.size() : v.fields.size();
                count(rv, n);
                for (size_t i = 0; i < n; i++) {
                    stored(i, v.form == ast::VariantForm::Tuple ? v.types[i] : v.fields[i].type);
                }
                break;
            }
            case RvKind::MakeList:
                if (t->kind != TK::List && t->kind != TK::Array) return fail("list value of a non-List type");
                if (t->kind == TK::Array) count(rv, t->len);  // Р62: ровно N
                for (size_t i = 0; i < rv.ops.size(); i++) stored(i, t->elem);
                break;
            case RvKind::MakeMap:
                if (t->kind != TK::Map) return fail("map value of a non-Map type");
                count(rv, 0);
                break;
            case RvKind::MakePool:
                if (t->kind != TK::Pool) return fail("pool value of a non-Pool type");
                count(rv, 0);
                break;
            case RvKind::MakeSome:
                if (t->kind != TK::Optional) return fail("some of a non-optional type");
                count(rv, 1);
                stored(0, t->elem);
                break;
            case RvKind::MakeNone:
                if (t->kind != TK::Optional) fail("none of a non-optional type");
                count(rv, 0);
                break;
            case RvKind::MakeOk:
                if (t->kind != TK::Result) return fail("ok of a non-result type");
                if (t->elem->kind == TK::Void) {
                    count(rv, 0);
                } else {
                    count(rv, 1);
                    stored(0, t->elem);
                }
                break;
            case RvKind::MakeErr:
                if (t->kind != TK::Result || !t->err) return fail("err of a non-result type");
                count(rv, 1);
                stored(0, t->err);
                break;
            case RvKind::SliceOf: {
                if (!rv.has_place) return fail("slice without a place");
                const Type* base = place(rv.place);
                if (!base) break;
                if (base->kind == TK::Str) same(t, prim(TK::Str), "result");
                else if (base->kind == TK::List || base->kind == TK::Slice || base->kind == TK::Array) {
                    if (t->kind != TK::Slice || t->elem != base->elem) fail("slice of a wrong type");
                } else {
                    fail("slice of a non-sequence");
                }
                if (rv.ops.empty() || rv.ops.size() > 2) fail("wrong number of slice bounds");
                for (const Operand& o : rv.ops) same(operand(o, false), prim(TK::I64), "slice bound");
                break;
            }
            case RvKind::CallPtr: {  // Р61: ops[0] — указатель, режимы и типы — по сигнатуре
                if (rv.ops.size() != 1) return fail("pointer call without a callee");
                const Type* ft = operand(rv.ops[0], false);
                if (!ft) break;
                if (ft->kind != TK::CFn) return fail("pointer call of a non-function value");
                const sema::CFnSig& sig = *ft->sig;
                same(t, sig.ret, "result");
                if (rv.args.size() != sig.params.size()) return fail("wrong number of arguments to a pointer call");
                for (size_t i = 0; i < rv.args.size(); i++) {
                    const Arg& a = rv.args[i];
                    Arg::Kind want = sig.params[i].first == ast::ParamMode::Value ? Arg::Kind::Value
                                     : sig.params[i].first == ast::ParamMode::Ref ? Arg::Kind::Ref
                                                                                  : Arg::Kind::Mut;
                    if (a.kind != want) {
                        fail("argument " + std::to_string(i) + " of a pointer call has a wrong mode");
                        continue;
                    }
                    if (a.kind == Arg::Kind::Value) {
                        same(operand(a.op, true), sig.params[i].second, "argument");
                    } else {
                        same(place(a.place), sig.params[i].second, "argument");
                        if (a.kind == Arg::Kind::Mut && !writable(a.place)) fail("'&mut' argument of a read-only place");
                    }
                }
                break;
            }
            case RvKind::Call: case RvKind::Spawn: {
                if (rv.fn >= p_.fns.size()) return fail("call of an unknown function");
                const Function& g = p_.fns[rv.fn];
                same(t, g.ret, "result");
                if (rv.kind == RvKind::Spawn && (g.is_extern || g.ret->kind != TK::Void)) {
                    fail("a task must be a dpl function without a result");
                }
                if (rv.args.size() != g.param_count) return fail("wrong number of arguments to " + g.name);
                for (size_t i = 0; i < rv.args.size(); i++) {
                    const Arg& a = rv.args[i];
                    const Local& prm = g.locals[i];
                    Arg::Kind want = prm.kind == LocalKind::Param ? Arg::Kind::Value
                                     : prm.kind == LocalKind::ParamRef ? Arg::Kind::Ref
                                                                       : Arg::Kind::Mut;
                    if (a.kind != want) {
                        fail("argument " + std::to_string(i) + " of " + g.name + " has a wrong mode");
                        continue;
                    }
                    if (a.kind == Arg::Kind::Value) {
                        same(operand(a.op, true), prm.type, "argument");
                    } else if (const Type* pt = place(a.place); a.kind == Arg::Kind::Ref && sema::c_prefix(prm.type, pt)) {
                        // Р62: `&S` в `&B` по общему началу
                    } else {
                        same(pt, prm.type, "argument");
                        if (a.kind == Arg::Kind::Mut && !writable(a.place)) fail("'&mut' argument of a read-only place");
                    }
                }
                break;
            }
            case RvKind::ParFor: {  // Р69: тело — функция (v, ссылки…) без результата
                same(t, prim(TK::Void), "result");
                if (rv.fn >= p_.fns.size()) return fail("parallel loop of an unknown function");
                const Function& g = p_.fns[rv.fn];
                if (g.is_extern || g.ret->kind != TK::Void) fail("parallel loop body must be a dpl function without a result");
                if (rv.ops.size() != 2) return fail("parallel loop needs two bounds");
                if (rv.args.size() + 1 != g.param_count || g.param_count == 0) {
                    return fail("wrong number of arguments to " + g.name);
                }
                const Type* bt = operand(rv.ops[0], false);
                same(operand(rv.ops[1], false), bt, "bound");
                if (bt && !sema::is_integer(bt)) fail("parallel loop over a non-integer range");
                if (g.locals[0].kind != LocalKind::Param) fail("parallel loop body takes the index by value");
                same(g.locals[0].type, bt, "index");
                for (size_t i = 0; i < rv.args.size(); i++) {
                    const Arg& a = rv.args[i];
                    const Local& prm = g.locals[i + 1];
                    Arg::Kind want = prm.kind == LocalKind::ParamRef ? Arg::Kind::Ref
                                     : prm.kind == LocalKind::ParamMut ? Arg::Kind::Mut
                                                                       : Arg::Kind::Value;
                    if (a.kind != want || a.kind == Arg::Kind::Value) {
                        fail("argument " + std::to_string(i + 1) + " of " + g.name + " has a wrong mode");
                        continue;
                    }
                    same(place(a.place), prm.type, "argument");
                    if (a.kind == Arg::Kind::Mut && !writable(a.place)) fail("'&mut' argument of a read-only place");
                }
                break;
            }
            case RvKind::Intrinsic: intrinsic(rv); break;
            case RvKind::Tag: {
                const Type* x = rv.has_place ? place(rv.place) : nullptr;
                if (!x || x->kind != TK::Enum) fail("tag of a non-enum");
                same(t, prim(TK::U32), "result");
                break;
            }
            case RvKind::AddrOf: {  // Р62: ссылка на место
                count(rv, 0);
                if (!rv.has_place) return fail("address without a place");
                const Type* pt = place(rv.place);
                if (pt && (t->kind != TK::Ref || t->elem != pt)) fail("address of a wrong type");
                break;
            }
            case RvKind::Zero:  // Р62: ноль — у типов, где он есть
                count(rv, 0);
                if (!sema::zeroable(t)) fail("zero of a type without a zero");
                break;
            case RvKind::IsSome: case RvKind::IsOk: {
                const Type* x = rv.has_place ? place(rv.place) : nullptr;
                TK want = rv.kind == RvKind::IsSome ? TK::Optional : TK::Result;
                if (!x || x->kind != want) fail("test of a wrong type");
                same(t, prim(TK::Bool), "result");
                break;
            }
        }
    }

    void inst(const Inst& i) {
        switch (i.kind) {
            case InstKind::Assign: {
                const Type* t = place(i.dst);
                if (!writable(i.dst)) fail("assignment to a read-only place %" + std::to_string(i.dst.local));
                rvalue(i.rv);
                same(i.rv.type, t, "assignment");
                if (i.canon && !sema::is_float(i.rv.type)) fail("canon assignment of a non-float value");
                break;
            }
            case InstKind::Eval:
                if (i.canon) fail("canon on an evaluation");
                rvalue(i.rv);
                break;
            case InstKind::Drop: {
                if (!i.dst.proj.empty() || i.dst.local >= f_.locals.size()) return fail("drop of a non-variable");
                const Local& l = f_.locals[i.dst.local];
                if (l.kind == LocalKind::ParamRef || l.kind == LocalKind::ParamMut) fail("drop of a reference");
                if (!needs_drop(l.type)) fail("drop of a value without memory");
                break;
            }
        }
    }

    void target(uint32_t b) {
        if (b >= f_.blocks.size()) fail("jump to a missing block bb" + std::to_string(b));
    }

    void term(const Term& t) {
        switch (t.kind) {
            case TermKind::None: fail("block without a terminator"); break;
            case TermKind::Jump: target(t.then_b); break;
            case TermKind::Branch:
                same(operand(t.cond, false), prim(TK::Bool), "condition");
                target(t.then_b);
                target(t.else_b);
                break;
            case TermKind::Return:
                if (t.has_value != (f_.ret->kind != TK::Void)) fail("return value does not match the result type");
                if (t.has_value) same(operand(t.value, true), f_.ret, "return value");
                break;
            case TermKind::Trap: case TermKind::Unreachable: break;
        }
    }
};

}  // namespace

std::vector<std::string> verify(const Program& p) {
    std::vector<std::string> errors;
    for (const Function& f : p.fns) Verifier(p, f, errors).run();
    if (p.main_fn >= static_cast<int>(p.fns.size())) errors.push_back("main function index is out of range");
    return errors;
}

}  // namespace dp::ir
