// Эталонный интерпретатор Core IR (docs/ir/README.md, «Интерпретатор»).
//
// Значение хранит свои части целиком: копия — глубокая, move забирает и
// оставляет пустое значение. Пустое значение — то, что освобождено или
// ещё не записано; читать его — ошибка понижения. Кадры вызовов лежат в
// своём стеке, а не в стеке C++: глубина рекурсии программы ограничена
// (kMaxFrames), переполнение — ловушка.
#include "ir/interp.h"

#include <algorithm>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <deque>
#include <tuple>

#include "support/format.h"
#include "support/utf8.h"

namespace dp::ir {

using sema::TK;

namespace {

constexpr size_t kMaxFrames = 1u << 15;
constexpr size_t kMaxLen = 1u << 26;  // элементов List или байтов str — больше: «нет памяти»

struct Value {
    enum class K : uint8_t { Empty, Int, Float, Bool, Str, Seq, Slice };
    K k = K::Empty;
    bool clean = false;  // объединение: поле tag только читалось — байты верны и для него
    uint32_t tag = 0;  // Seq: вариант перечисления; ?T — 0 нет, 1 есть; E!T — 0 успех, 1 ошибка
    uint64_t i = 0;    // Int: каноническая запись (знаковое — с расширением знака); Bool
    double f = 0;
    std::string s;
    std::vector<Value> items;     // Seq: элементы List, поля, данные варианта
    const Value* base = nullptr;  // Slice: List
    int64_t lo = 0, hi = 0;       // Slice: границы в base
};

Value int_value(uint64_t i) {
    Value v;
    v.k = Value::K::Int;
    v.i = i;
    return v;
}

Value bool_value(bool b) {
    Value v;
    v.k = Value::K::Bool;
    v.i = b ? 1 : 0;
    return v;
}

Value float_value(double f) {
    Value v;
    v.k = Value::K::Float;
    v.f = f;
    return v;
}

Value str_value(std::string s) {
    Value v;
    v.k = Value::K::Str;
    v.s = std::move(s);
    return v;
}

Value seq_value(uint32_t tag);

// --- объединения (Р62): байты раскладки C, поле — их прочтение.
//
// Значение объединения — Seq: items — поля, s — байты (размер объединения).
// tag — 0: байты верны, поля прочтены из них; n > 0 — верно поле n - 1;
// clean — оно только читалось, байты верны и для него, иначе устарели.
// Выбор поля записывает изменённое поле в байты и читает из них
// выбранное: прочтение в обе стороны теряло бы биты (сигнальный NaN f32
// идёт через double, байт bool 2 — через true). Раскладка — как у C на целях dpl (64 бита,
// little-endian): указатель — 8 байт.

uint64_t c_align(const Type* t);

uint64_t c_size(const Type* t) {
    if (sema::is_integer(t)) return static_cast<uint64_t>(sema::int_bits(t) / 8);
    switch (t->kind) {
        case TK::F32: return 4;
        case TK::F64: return 8;
        case TK::Bool: return 1;
        case TK::Array: return t->len * c_size(t->elem);
        case TK::Struct: {
            uint64_t size = 0, align = c_align(t);
            for (const sema::FieldDef& f : t->sdef->fields) {
                uint64_t fs = c_size(f.type), fa = c_align(f.type);
                if (t->sdef->union_) {
                    size = std::max(size, fs);
                } else {
                    size = (size + fa - 1) / fa * fa + fs;
                }
            }
            return (size + align - 1) / align * align;
        }
        default: return 8;  // дескриптор, указатель на функцию
    }
}

uint64_t c_align(const Type* t) {
    if (t->kind == TK::Array) return c_align(t->elem);
    if (t->kind == TK::Struct) {
        uint64_t a = 1;
        for (const sema::FieldDef& f : t->sdef->fields) a = std::max(a, c_align(f.type));
        return a;
    }
    return c_size(t);
}

void union_sync(Value& u, const Type* t);

void encode(const Value& v0, const Type* t, std::string& bytes, uint64_t off) {
    if (sema::is_integer(t) || t->kind == TK::Bool || t->kind == TK::Handle || t->kind == TK::CFn) {
        uint64_t n = c_size(t);
        for (uint64_t k = 0; k < n; k++) bytes[off + k] = static_cast<char>((v0.i >> (8 * k)) & 0xff);
        return;
    }
    if (t->kind == TK::F32) {
        float f = static_cast<float>(v0.f);
        std::memcpy(&bytes[off], &f, 4);
        return;
    }
    if (t->kind == TK::F64) {
        std::memcpy(&bytes[off], &v0.f, 8);
        return;
    }
    if (t->kind == TK::Array) {
        uint64_t es = c_size(t->elem);
        for (uint64_t k = 0; k < t->len && k < v0.items.size(); k++) encode(v0.items[k], t->elem, bytes, off + k * es);
        return;
    }
    if (t->kind == TK::Struct && t->sdef->union_) {
        Value v = v0;
        union_sync(v, t);
        bytes.replace(off, v.s.size(), v.s);
        return;
    }
    if (t->kind == TK::Struct) {
        uint64_t at = 0;
        for (size_t k = 0; k < t->sdef->fields.size() && k < v0.items.size(); k++) {
            const Type* ft = t->sdef->fields[k].type;
            uint64_t fa = c_align(ft);
            at = (at + fa - 1) / fa * fa;
            encode(v0.items[k], ft, bytes, off + at);
            at += c_size(ft);
        }
    }
}

Value decode(const std::string& bytes, uint64_t off, const Type* t) {
    if (sema::is_integer(t) || t->kind == TK::Handle || t->kind == TK::CFn) {
        uint64_t n = c_size(t), v = 0;
        for (uint64_t k = 0; k < n; k++) v |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[off + k])) << (8 * k);
        if (sema::is_signed(t) && n < 8 && (v >> (8 * n - 1)) & 1) v |= ~uint64_t{0} << (8 * n);  // знак
        return int_value(v);
    }
    if (t->kind == TK::Bool) return bool_value(bytes[off] != 0);
    if (t->kind == TK::F32) {
        float f;
        std::memcpy(&f, &bytes[off], 4);
        return float_value(static_cast<double>(f));
    }
    if (t->kind == TK::F64) {
        double f;
        std::memcpy(&f, &bytes[off], 8);
        return float_value(f);
    }
    Value v = seq_value(0);
    if (t->kind == TK::Array) {
        uint64_t es = c_size(t->elem);
        for (uint64_t k = 0; k < t->len; k++) v.items.push_back(decode(bytes, off + k * es, t->elem));
        return v;
    }
    if (t->kind == TK::Struct && t->sdef->union_) {
        v.s = bytes.substr(off, c_size(t));
        for (const sema::FieldDef& f : t->sdef->fields) v.items.push_back(decode(v.s, 0, f.type));
        return v;
    }
    uint64_t at = 0;
    for (const sema::FieldDef& f : t->sdef->fields) {
        uint64_t fa = c_align(f.type);
        at = (at + fa - 1) / fa * fa;
        v.items.push_back(decode(bytes, off + at, f.type));
        at += c_size(f.type);
    }
    return v;
}

// Байты объединения — по верному полю (tag).
void union_sync(Value& u, const Type* t) {
    if (u.s.size() != c_size(t)) u.s.assign(c_size(t), '\0');
    if (u.tag > 0 && u.tag <= u.items.size() && !u.clean) {
        encode(u.items[u.tag - 1], t->sdef->fields[u.tag - 1].type, u.s, 0);
    }
}

// Выбор поля field для чтения или записи (write): верное — оно,
// прочтённое из байтов; после записи байты под ним устарели.
void union_select(Value& u, const Type* t, uint32_t field, bool write) {
    if (u.tag != field + 1) {
        union_sync(u, t);
        u.items[field] = decode(u.s, 0, t->sdef->fields[field].type);
        u.tag = field + 1;
        u.clean = true;
    }
    if (write) u.clean = false;
}

// Ноль типа (Р62): пропущенное поле `@c`.
Value zero_value(const Type* t) {
    if (sema::is_float(t)) return float_value(0);
    if (sema::is_integer(t)) return int_value(0);
    switch (t->kind) {
        case TK::Bool: return bool_value(false);
        case TK::Str: return str_value("");
        case TK::Array: {
            Value v = seq_value(0);
            for (uint64_t i = 0; i < t->len; i++) v.items.push_back(zero_value(t->elem));
            return v;
        }
        case TK::Struct: {
            Value v = seq_value(0);
            for (const sema::FieldDef& f : t->sdef->fields) v.items.push_back(zero_value(f.type));
            if (t->sdef->union_) v.s.assign(c_size(t), '\0');
            return v;
        }
        default: return seq_value(0);  // ?T — none, срез — пустой
    }
}

Value seq_value(uint32_t tag) {
    Value v;
    v.k = Value::K::Seq;
    v.tag = tag;
    return v;
}

// Элементы List или среза.
size_t seq_len(const Value& v) {
    return v.k == Value::K::Slice ? static_cast<size_t>(v.hi - v.lo) : v.items.size();
}

const Value& seq_at(const Value& v, size_t i) {
    return v.k == Value::K::Slice ? v.base->items[static_cast<size_t>(v.lo) + i] : v.items[i];
}

// Map (Р54) — Seq с парами по порядку вставки: items[2i] — ключ, items[2i + 1] —
// значение. Ключ — целое, bool или str: сравнение без типа. Поиск — перебором:
// интерпретатор только для тестов (Р43).
bool key_equal(const Value& a, const Value& b) { return a.k == b.k && (a.k == Value::K::Str ? a.s == b.s : a.i == b.i); }

// Позиция пары ключа в Map или -1.
int64_t map_find(const Value& m, const Value& key) {
    for (size_t i = 0; i + 1 < m.items.size(); i += 2) {
        if (key_equal(m.items[i], key)) return static_cast<int64_t>(i / 2);
    }
    return -1;
}

// Место: значение или байт строки.
struct Ref {
    Value* v = nullptr;
    int64_t byte = -1;
};

struct Frame {
    uint32_t fn = 0;
    std::vector<Value> locals;
    std::vector<Ref> refs;  // параметры-ссылки
    uint32_t block = 0;
    uint32_t inst = 0;
};

// --- целые

uint64_t mask(int bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }

uint64_t canon(uint64_t v, const Type* t) {
    int bits = sema::int_bits(t);
    if (bits >= 64) return v;
    v &= mask(bits);
    if (sema::is_signed(t) && ((v >> (bits - 1)) & 1)) v |= ~mask(bits);
    return v;
}

bool fits_signed(int64_t v, int bits) {
    if (bits >= 64) return true;
    int64_t lim = static_cast<int64_t>(1ull << (bits - 1));
    return v >= -lim && v < lim;
}

bool fits_unsigned(uint64_t v, int bits) { return bits >= 64 || v <= mask(bits); }

std::string int_text(uint64_t v, const Type* t) {
    return sema::is_signed(t) ? std::to_string(static_cast<int64_t>(v)) : std::to_string(v);
}

const char* bin_symbol(BinKind k) {
    switch (k) {
        case BinKind::Add: return "+";
        case BinKind::Sub: return "-";
        case BinKind::Mul: return "*";
        case BinKind::Div: return "/";
        case BinKind::Rem: return "%";
        default: return "?";
    }
}

// --- сравнения (expr.compare)

bool equal(const Value& a, const Value& b, const Type* t);
bool less(const Value& a, const Value& b, const Type* t);

const Type* payload_type(const Type* t, uint32_t variant, size_t k) {
    const sema::VariantDef& v = t->edef->variants[variant];
    return v.form == ast::VariantForm::Tuple ? v.types[k] : v.fields[k].type;
}

bool equal(const Value& a, const Value& b, const Type* t) {
    switch (t->kind) {
        case TK::Bool: return a.i == b.i;
        case TK::F32: case TK::F64: return a.f == b.f;
        case TK::Str: return a.s == b.s;
        case TK::List: case TK::Slice: {
            size_t n = seq_len(a);
            if (n != seq_len(b)) return false;
            for (size_t i = 0; i < n; i++) {
                if (!equal(seq_at(a, i), seq_at(b, i), t->elem)) return false;
            }
            return true;
        }
        case TK::Optional:
            return a.tag == b.tag && (a.tag == 0 || equal(a.items[0], b.items[0], t->elem));
        case TK::PoolHandle: return a.items[0].i == b.items[0].i && a.items[1].i == b.items[1].i;
        case TK::Struct:
            for (size_t k = 0; k < t->sdef->fields.size(); k++) {
                if (!equal(a.items[k], b.items[k], t->sdef->fields[k].type)) return false;
            }
            return true;
        case TK::Enum:
            if (a.tag != b.tag) return false;
            for (size_t k = 0; k < a.items.size(); k++) {
                if (!equal(a.items[k], b.items[k], payload_type(t, a.tag, k))) return false;
            }
            return true;
        default: return a.i == b.i;  // целые
    }
}

// Лексикографически: первая часть, где одна меньше другой, решает.
template <class At>
bool less_parts(size_t n, At at) {
    for (size_t k = 0; k < n; k++) {
        auto [x, y, t] = at(k);
        if (less(*x, *y, t)) return true;
        if (less(*y, *x, t)) return false;
    }
    return false;
}

bool less(const Value& a, const Value& b, const Type* t) {
    switch (t->kind) {
        case TK::Bool: return a.i < b.i;
        case TK::F32: case TK::F64: return a.f < b.f;
        case TK::Str: return a.s < b.s;  // по байтам без знака
        case TK::List: case TK::Slice: {
            size_t na = seq_len(a), nb = seq_len(b), n = na < nb ? na : nb;
            for (size_t i = 0; i < n; i++) {
                if (less(seq_at(a, i), seq_at(b, i), t->elem)) return true;
                if (less(seq_at(b, i), seq_at(a, i), t->elem)) return false;
            }
            return na < nb;
        }
        case TK::Struct:
            return less_parts(t->sdef->fields.size(), [&](size_t k) {
                return std::tuple{&a.items[k], &b.items[k], t->sdef->fields[k].type};
            });
        case TK::Enum:
            if (a.tag != b.tag) return a.tag < b.tag;
            return less_parts(a.items.size(), [&](size_t k) {
                return std::tuple{&a.items[k], &b.items[k], payload_type(t, a.tag, k)};
            });
        default:
            if (sema::is_signed(t)) return static_cast<int64_t>(a.i) < static_cast<int64_t>(b.i);
            return a.i < b.i;
    }
}

// Pool (Р67): последовательность слотов; у слота tag — жив (1) или нет,
// i — поколение, items — значение живого. Хэндл — [номер, поколение].
constexpr uint64_t kGenLimit = UINT32_MAX;  // слот с таким поколением выведен из оборота

Value handle_value(size_t index, uint64_t gen) {
    Value h = seq_value(0);
    h.items.push_back(int_value(index));
    h.items.push_back(int_value(gen));
    return h;
}

// Номер живого слота хэндла h или -1, если хэндл устарел.
int64_t pool_find(const Value& p, const Value& h) {
    uint64_t i = h.items[0].i;
    if (i >= p.items.size()) return -1;
    const Value& s = p.items[i];
    return s.tag == 1 && s.i == h.items[1].i ? static_cast<int64_t>(i) : -1;
}

size_t pool_live(const Value& p) {
    size_t n = 0;
    for (const Value& s : p.items) n += s.tag;
    return n;
}

// Освобождение живого слота: поколение растёт, на пределе слот выведен.
void pool_release(Value& s) {
    s.tag = 0;
    s.items.clear();
    s.i++;
}

// Устойчивая сортировка слиянием: при любом сравнении (и с NaN) даёт
// перестановку и заканчивается.
template <class T, class Less>
void merge_sort(std::vector<T>& v, Less lt) {
    size_t n = v.size();
    std::vector<T> tmp(n);
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = lt(v[j], v[i]) ? std::move(v[j++]) : std::move(v[i++]);
            while (i < mid) tmp[k++] = std::move(v[i++]);
            while (j < hi) tmp[k++] = std::move(v[j++]);
        }
        v.swap(tmp);
    }
}

void merge_sort(std::vector<Value>& v, const Type* t) {
    merge_sort(v, [t](const Value& a, const Value& b) { return less(a, b, t); });
}

// --- встроенное (builtin.functions)

bool is_space(uint64_t b) { return b == ' ' || b == '\t' || b == '\n' || b == '\r'; }
bool is_digit(uint64_t b) { return b >= '0' && b <= '9'; }
bool is_alpha(uint64_t b) { return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z'); }

// `parse_int`: десятичное целое со знаком (+ или -), без пробелов; вне i64 — нет значения.
bool parse_int(const std::string& s, int64_t& out) {
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        negative = s[i] == '-';
        i++;
    }
    if (i == s.size()) return false;
    uint64_t limit = negative ? static_cast<uint64_t>(INT64_MAX) + 1 : static_cast<uint64_t>(INT64_MAX);
    uint64_t v = 0;
    for (; i < s.size(); i++) {
        if (!is_digit(static_cast<uint8_t>(s[i]))) return false;
        uint64_t d = static_cast<uint64_t>(s[i] - '0');
        if (v > (limit - d) / 10) return false;
        v = v * 10 + d;
    }
    out = negative ? static_cast<int64_t>(0 - v) : static_cast<int64_t>(v);
    return true;
}

class Interp {
public:
    Interp(const Program& p, const SourceMap& sources, const std::vector<std::string>& args)
        : p_(p), sources_(sources), args_(args) {}

    RunResult run() {
        if (p_.main_fn < 0) {
            internal("the program has no main function");
            return res_;
        }
        Frame fr;
        fr.fn = static_cast<uint32_t>(p_.main_fn);
        fr.locals.resize(p_.fns[fr.fn].locals.size());
        frames_.push_back(std::move(fr));
        while (!frames_.empty() && !stop_) step();
        std::fflush(stdout);
        return res_;
    }

private:
    const Program& p_;
    const SourceMap& sources_;
    const std::vector<std::string>& args_;  // аргументы программы без её имени (os.args)
    std::deque<Frame> frames_;
    bool stop_ = false;
    RunResult res_;
    Span span_;

    const Function& fn(const Frame& fr) const { return p_.fns[fr.fn]; }

    void internal(const std::string& msg) {
        if (stop_) return;
        stop_ = true;
        res_.internal = true;
        res_.message = "interpreter: " + msg;
        if (!frames_.empty()) res_.message += " (in " + fn(frames_.back()).name + ")";
    }

    void trap(const std::string& msg) {
        if (stop_) return;
        stop_ = true;
        res_.exit_code = 101;
        std::fflush(stdout);
        std::string where;
        if (span_.file < sources_.size()) {
            const SourceFile& f = sources_.file(span_.file);
            LineCol lc = f.line_col(span_.begin);
            where = f.path() + ":" + std::to_string(lc.line) + ":" + std::to_string(lc.col) + ": ";
        }
        std::string line = where + "trap: " + msg + "\n";
        std::fwrite(line.data(), 1, line.size(), stderr);
    }

    // --- места и операнды

    // Место pl; write — его меняют (присваивание, `&mut`, изменяющий метод).
    Ref resolve(Frame& fr, const Place& pl, bool write = false) {
        const Function& f = fn(fr);
        const Local& l = f.locals[pl.local];
        Ref r;
        if (l.kind == LocalKind::ParamRef || l.kind == LocalKind::ParamMut) r = fr.refs[pl.local];
        else r.v = &fr.locals[pl.local];
        for (const Proj& pr : pl.proj) {
            if (stop_) return Ref{};
            if (r.byte >= 0) {
                internal("projection of a str byte");
                return Ref{};
            }
            Value& v = *r.v;
            if (v.k == Value::K::Empty) {
                internal("use of an empty value of %" + std::to_string(pl.local));
                return Ref{};
            }
            switch (pr.kind) {
                case ProjKind::Field: {
                    if (v.k != Value::K::Seq || pr.index >= v.items.size()) {
                        internal("field of a non-struct");
                        return Ref{};
                    }
                    // Р62: поле объединения — прочтение его байтов.
                    Place prefix{pl.local, std::vector<Proj>(pl.proj.begin(), pl.proj.begin() + (&pr - pl.proj.data()))};
                    const Type* st = place_type(p_, f, prefix);
                    if (st && st->kind == TK::Struct && st->sdef->union_) union_select(v, st, pr.index, write);
                    r.v = &v.items[pr.index];
                    break;
                }
                case ProjKind::Index: {
                    const Value& iv = fr.locals[pr.index];
                    if (iv.k != Value::K::Int) {
                        internal("index is not an integer");
                        return Ref{};
                    }
                    int64_t i = static_cast<int64_t>(iv.i);
                    size_t n = v.k == Value::K::Str ? v.s.size() : seq_len(v);
                    if (i < 0 || static_cast<uint64_t>(i) >= n) {
                        trap("index " + std::to_string(i) + " is out of bounds for length " + std::to_string(n));
                        return Ref{};
                    }
                    if (v.k == Value::K::Str) r.byte = i;
                    else if (v.k == Value::K::Slice) r.v = const_cast<Value*>(&seq_at(v, static_cast<size_t>(i)));
                    else r.v = &v.items[static_cast<size_t>(i)];
                    break;
                }
                case ProjKind::Payload:
                    if (v.k != Value::K::Seq || v.tag != pr.index || pr.sub >= v.items.size()) {
                        internal("payload of another variant");
                        return Ref{};
                    }
                    r.v = &v.items[pr.sub];
                    break;
                case ProjKind::MapValue: {
                    if (v.k != Value::K::Seq) {
                        internal("value of a non-Map");
                        return Ref{};
                    }
                    int64_t i = map_find(v, fr.locals[pr.index]);
                    if (i < 0) {
                        trap("key is not in the map");
                        return Ref{};
                    }
                    r.v = &v.items[2 * static_cast<size_t>(i) + 1];
                    break;
                }
                case ProjKind::PoolValue: {  // Р67
                    int64_t i = v.k == Value::K::Seq ? pool_find(v, fr.locals[pr.index]) : -1;
                    if (i < 0) {
                        trap("stale handle: its slot was removed or reused");
                        return Ref{};
                    }
                    r.v = &v.items[static_cast<size_t>(i)].items[0];
                    break;
                }
                case ProjKind::PoolSlot: {  // живой слот из обхода
                    const Value& iv = fr.locals[pr.index];
                    if (v.k != Value::K::Seq || iv.k != Value::K::Int || iv.i >= v.items.size() ||
                        v.items[iv.i].tag != 1) {
                        internal("pool slot is not live");
                        return Ref{};
                    }
                    r.v = &v.items[iv.i].items[0];
                    break;
                }
                case ProjKind::MapKeyAt: case ProjKind::MapValueAt: {  // номер пары из MapOrder
                    const Value& iv = fr.locals[pr.index];
                    if (v.k != Value::K::Seq || iv.k != Value::K::Int || iv.i >= v.items.size() / 2) {
                        internal("pair number outside the map");
                        return Ref{};
                    }
                    r.v = &v.items[2 * static_cast<size_t>(iv.i) + (pr.kind == ProjKind::MapValueAt ? 1 : 0)];
                    break;
                }
                case ProjKind::Unwrap: case ProjKind::ResultValue: case ProjKind::ResultError: {
                    uint32_t want = pr.kind == ProjKind::ResultValue ? 0 : 1;
                    if (v.k != Value::K::Seq || v.tag != want || v.items.empty()) {
                        internal("value of an empty optional or of the other result case");
                        return Ref{};
                    }
                    r.v = &v.items[0];
                    break;
                }
            }
        }
        return r;
    }

    Value constant(const Const& c) {
        switch (c.kind) {
            case Const::Kind::Int: return int_value(c.i);
            case Const::Kind::Float: return float_value(c.f);
            case Const::Kind::Bool: return bool_value(c.b);
            case Const::Kind::Str: return str_value(p_.strings[c.str]);
            case Const::Kind::Fn: return int_value(c.fn);  // адрес функции — её номер (Р61)
        }
        return Value{};
    }

    // Значение для чтения (без копии); константа и байт — в scratch.
    const Value& peek(Frame& fr, const Operand& o, Value& scratch) {
        if (o.kind == OpKind::Const) {
            scratch = constant(o.c);
            return scratch;
        }
        Ref r = resolve(fr, o.place);
        if (stop_) return scratch;
        if (r.byte >= 0) {
            scratch = int_value(static_cast<uint8_t>(r.v->s[static_cast<size_t>(r.byte)]));
            return scratch;
        }
        if (r.v->k == Value::K::Empty) internal("read of an empty value of %" + std::to_string(o.place.local));
        return *r.v;
    }

    // Значение для сохранения: копия места или забранное временное.
    Value take(Frame& fr, const Operand& o) {
        if (o.kind == OpKind::Const) return constant(o.c);
        Ref r = resolve(fr, o.place);
        if (stop_) return Value{};
        if (r.byte >= 0) return int_value(static_cast<uint8_t>(r.v->s[static_cast<size_t>(r.byte)]));
        if (r.v->k == Value::K::Empty) {
            internal("read of an empty value of %" + std::to_string(o.place.local));
            return Value{};
        }
        if (o.kind == OpKind::Copy) return *r.v;
        Value v = std::move(*r.v);
        *r.v = Value{};
        return v;
    }

    const Type* op_type(const Frame& fr, const Operand& o) { return operand_type(p_, fn(fr), o); }

    // --- шаг исполнения

    void step() {
        Frame& fr = frames_.back();
        const Function& f = fn(fr);
        if (fr.block >= f.blocks.size()) return internal("jump to a missing block");
        const Block& b = f.blocks[fr.block];
        if (fr.inst >= b.insts.size()) return term(fr, b.term);
        const Inst& in = b.insts[fr.inst];
        span_ = in.span;
        if (in.kind == InstKind::Drop) {
            Ref r = resolve(fr, in.dst, true);
            if (!stop_) *r.v = Value{};
            fr.inst++;
            return;
        }
        if (in.rv.kind == RvKind::Call || in.rv.kind == RvKind::CallPtr) return call(fr, in.rv);  // продолжение — при возврате
        Value v = rvalue(fr, in.rv);
        if (stop_) return;
        if (in.kind == InstKind::Assign) {
            if (in.canon) v = canonical(v);
            Ref r = resolve(fr, in.dst, true);
            if (stop_) return;
            *r.v = std::move(v);
        }
        fr.inst++;
    }

    // Р56: NaN — положительный тихий без полезной нагрузки, остальное — как есть.
    static Value canonical(const Value& v) {
        if (!std::isnan(v.f)) return v;
        uint64_t bits = 0x7ff8000000000000ull;
        double d = 0;
        std::memcpy(&d, &bits, sizeof d);
        return float_value(d);
    }

    void call(Frame& fr, const Rvalue& rv) {
        if (frames_.size() >= kMaxFrames) return trap("stack overflow: calls are nested too deeply");
        uint32_t target = rv.fn;
        if (rv.kind == RvKind::CallPtr) {  // Р61: значение — номер функции `@export`
            Value scratch;
            const Value& v = peek(fr, rv.ops[0], scratch);
            if (stop_) return;
            if (v.k != Value::K::Int || v.i >= p_.fns.size()) return internal("pointer call of a non-function value");
            target = static_cast<uint32_t>(v.i);
        }
        const Function& g = p_.fns[target];
        if (g.is_extern) {  // Р59: интерпретатор исполняет только dpl
            return internal("cannot call C function '" + g.c_name +
                            "': the reference interpreter runs only dpl code; build the program with 'dpl build'");
        }
        Frame nf;
        nf.fn = target;
        nf.locals.resize(g.locals.size());
        nf.refs.resize(g.param_count);
        for (size_t i = 0; i < rv.args.size(); i++) {
            const Arg& a = rv.args[i];
            if (a.kind == Arg::Kind::Value) {
                nf.locals[i] = take(fr, a.op);
            } else {
                nf.refs[i] = resolve(fr, a.place, a.kind == Arg::Kind::Mut);
                if (!stop_ && nf.refs[i].byte < 0 && nf.refs[i].v->k == Value::K::Empty) {
                    internal("reference to an empty value");
                }
            }
            if (stop_) return;
        }
        frames_.push_back(std::move(nf));
    }

    void finish_return(Value v) {
        Frame& fr = frames_.back();
        const Function& f = fn(fr);
        for (size_t i = 0; i < f.locals.size(); i++) {
            const Local& l = f.locals[i];
            if (l.kind == LocalKind::ParamRef || l.kind == LocalKind::ParamMut || !needs_drop(l.type)) continue;
            if (fr.locals[i].k != Value::K::Empty) {
                return internal("%" + std::to_string(i) + (l.name.empty() ? "" : " '" + l.name + "'") +
                                " still holds a value at return");
            }
        }
        const Type* ret = f.ret;
        bool det = f.det;
        frames_.pop_back();
        if (frames_.empty()) return finish_main(v, ret);
        Frame& caller = frames_.back();
        const Inst& in = fn(caller).blocks[caller.block].insts[caller.inst];
        if (in.kind == InstKind::Assign) {
            // Через указатель — обёртка `@export`: результат функции класса D
            // уходит в C каноническим (Р56).
            bool wrapper = in.rv.kind == RvKind::CallPtr && det && sema::is_float(ret);
            Value w = in.canon || wrapper ? canonical(v) : v;
            Ref r = resolve(caller, in.dst, true);
            if (stop_) return;
            *r.v = std::move(w);
        }
        caller.inst++;
    }

    void finish_main(const Value& v, const Type* ret) {
        if (ret->kind != TK::Result || v.tag == 0) return;
        std::fflush(stdout);
        const Type* e = ret->err;
        std::string name = e->edef->name + "." + e->edef->variants[v.items[0].tag].name;
        std::string line = "error: main failed with " + name + "\n";
        std::fwrite(line.data(), 1, line.size(), stderr);
        res_.exit_code = 1;
    }

    void term(Frame& fr, const Term& t) {
        span_ = t.span;
        switch (t.kind) {
            case TermKind::Jump:
                fr.block = t.then_b;
                fr.inst = 0;
                break;
            case TermKind::Branch: {
                Value s;
                const Value& c = peek(fr, t.cond, s);
                if (stop_) return;
                fr.block = c.i ? t.then_b : t.else_b;
                fr.inst = 0;
                break;
            }
            case TermKind::Return: {
                Value v;
                if (t.has_value) v = take(fr, t.value);
                if (stop_) return;
                finish_return(std::move(v));
                break;
            }
            case TermKind::Trap: trap(t.message); break;
            case TermKind::Unreachable: internal("reached code marked unreachable"); break;
            case TermKind::None: internal("block without a terminator"); break;
        }
    }

    // --- правые части

    Value rvalue(Frame& fr, const Rvalue& rv) {
        switch (rv.kind) {
            case RvKind::Use: return take(fr, rv.ops[0]);
            case RvKind::Binary: return binary(fr, rv);
            case RvKind::Unary: return unary(fr, rv);
            case RvKind::Cast: return cast(fr, rv);
            case RvKind::MakeStruct: case RvKind::MakeVariant: case RvKind::MakeList: case RvKind::MakeSome:
            case RvKind::MakeNone: case RvKind::MakeOk: case RvKind::MakeErr: {
                uint32_t tag = rv.kind == RvKind::MakeVariant ? rv.variant
                               : rv.kind == RvKind::MakeSome || rv.kind == RvKind::MakeErr ? 1 : 0;
                Value v = seq_value(tag);
                for (const Operand& o : rv.ops) v.items.push_back(take(fr, o));
                if (v.items.size() > kMaxLen) trap("out of memory");
                if (rv.kind == RvKind::MakeStruct && rv.type->kind == TK::Struct && rv.type->sdef->union_) {
                    v.s.assign(c_size(rv.type), '\0');  // Р62: байты — нули, верно заданное поле
                    v.tag = rv.variant;
                }
                return v;
            }
            case RvKind::MakeMap: case RvKind::MakePool: return seq_value(0);
            case RvKind::Zero: return zero_value(rv.type);
            case RvKind::AddrOf: {  // Р62: ссылку читает только C, а его интерпретатор не вызывает — копия
                Ref r = resolve(fr, rv.place);
                if (stop_) return Value{};
                return r.byte >= 0 ? int_value(static_cast<uint64_t>(r.byte)) : *r.v;
            }
            case RvKind::SliceOf: return slice(fr, rv);
            case RvKind::Intrinsic: return intrinsic(fr, rv);
            case RvKind::Tag: case RvKind::IsSome: case RvKind::IsOk: {
                Ref r = resolve(fr, rv.place);
                if (stop_) return Value{};
                if (r.v->k != Value::K::Seq) {
                    internal("test of an empty value");
                    return Value{};
                }
                if (rv.kind == RvKind::Tag) return int_value(r.v->tag);
                return bool_value(rv.kind == RvKind::IsSome ? r.v->tag == 1 : r.v->tag == 0);
            }
            case RvKind::Call: case RvKind::CallPtr: break;  // исполняется в step()
        }
        internal("unknown rvalue");
        return Value{};
    }

    Value binary(Frame& fr, const Rvalue& rv) {
        Value sa, sb;
        const Value& a = peek(fr, rv.ops[0], sa);
        const Value& b = peek(fr, rv.ops[1], sb);
        if (stop_) return Value{};
        const Type* t = op_type(fr, rv.ops[0]);
        switch (rv.bin) {
            case BinKind::Eq: return bool_value(equal(a, b, t));
            case BinKind::Ne: return bool_value(!equal(a, b, t));
            case BinKind::Lt: return bool_value(less(a, b, t));
            case BinKind::Le: return bool_value(!less(b, a, t) && (equal(a, b, t) || less(a, b, t)));
            case BinKind::Gt: return bool_value(less(b, a, t));
            case BinKind::Ge: return bool_value(!less(a, b, t) && (equal(a, b, t) || less(b, a, t)));
            case BinKind::Concat:
                if (a.s.size() + b.s.size() > kMaxLen) {
                    trap("out of memory");
                    return Value{};
                }
                return str_value(a.s + b.s);
            default: break;
        }
        if (sema::is_float(t)) {
            bool single = t->kind == TK::F32;
            float x = static_cast<float>(a.f), y = static_cast<float>(b.f);
            double r = 0;
            switch (rv.bin) {
                case BinKind::Add: r = single ? static_cast<double>(x + y) : a.f + b.f; break;
                case BinKind::Sub: r = single ? static_cast<double>(x - y) : a.f - b.f; break;
                case BinKind::Mul: r = single ? static_cast<double>(x * y) : a.f * b.f; break;
                case BinKind::Div: r = single ? static_cast<double>(x / y) : a.f / b.f; break;
                default: internal("integer operation on floats"); break;
            }
            return float_value(r);
        }
        return int_op(rv.bin, a.i, b.i, t, op_type(fr, rv.ops[1]));
    }

    // Проверяемая арифметика (Р53): точный результат в типе t — true и r.
    static bool checked(Intrinsic k, uint64_t a, uint64_t b, const Type* t, uint64_t& r) {
        int bits = sema::int_bits(t);
        if (sema::is_signed(t)) {
            int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b), sr = 0;
            int64_t min = bits >= 64 ? INT64_MIN : -static_cast<int64_t>(1ull << (bits - 1));
            bool o = false;
            switch (k) {
                case Intrinsic::CheckedAdd: o = __builtin_add_overflow(sa, sb, &sr); break;
                case Intrinsic::CheckedSub: o = __builtin_sub_overflow(sa, sb, &sr); break;
                case Intrinsic::CheckedMul: o = __builtin_mul_overflow(sa, sb, &sr); break;
                case Intrinsic::CheckedDiv:
                    if (sb == 0 || (sa == min && sb == -1)) return false;
                    sr = sa / sb;
                    break;
                default:  // CheckedNeg
                    if (sa == min) return false;
                    sr = -sa;
                    break;
            }
            if (o || !fits_signed(sr, bits)) return false;
            r = static_cast<uint64_t>(sr);
            return true;
        }
        bool o = false;
        switch (k) {
            case Intrinsic::CheckedAdd: o = __builtin_add_overflow(a, b, &r); break;
            case Intrinsic::CheckedSub: o = __builtin_sub_overflow(a, b, &r); break;
            case Intrinsic::CheckedMul: o = __builtin_mul_overflow(a, b, &r); break;
            default:  // CheckedDiv; checked_neg у беззнаковых не проходит проверку типов
                if (b == 0) return false;
                r = a / b;
                break;
        }
        return !o && fits_unsigned(r, bits);
    }

    Value int_op(BinKind k, uint64_t a, uint64_t b, const Type* t, const Type* bt) {
        bool sg = sema::is_signed(t);
        int bits = sema::int_bits(t);
        int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
        auto overflow = [&] {
            trap(std::string("integer overflow in '") + bin_symbol(k) + "' on '" + sema::type_name(t) + "'");
            return Value{};
        };
        switch (k) {
            case BinKind::Add: case BinKind::Sub: case BinKind::Mul: {
                if (sg) {
                    int64_t r = 0;
                    bool o = k == BinKind::Add ? __builtin_add_overflow(sa, sb, &r)
                             : k == BinKind::Sub ? __builtin_sub_overflow(sa, sb, &r)
                                                 : __builtin_mul_overflow(sa, sb, &r);
                    if (o || !fits_signed(r, bits)) return overflow();
                    return int_value(static_cast<uint64_t>(r));
                }
                uint64_t r = 0;
                bool o = k == BinKind::Add ? __builtin_add_overflow(a, b, &r)
                         : k == BinKind::Sub ? __builtin_sub_overflow(a, b, &r)
                                             : __builtin_mul_overflow(a, b, &r);
                if (o || !fits_unsigned(r, bits)) return overflow();
                return int_value(r);
            }
            case BinKind::Div: case BinKind::Rem: {
                if (b == 0) {
                    trap("division by zero");
                    return Value{};
                }
                if (sg) {
                    int64_t min = bits >= 64 ? INT64_MIN : -static_cast<int64_t>(1ull << (bits - 1));
                    if (sa == min && sb == -1) {
                        if (k == BinKind::Rem) return int_value(0);  // остаток 0, переполнения нет
                        return overflow();
                    }
                    return int_value(static_cast<uint64_t>(k == BinKind::Div ? sa / sb : sa % sb));
                }
                return int_value(k == BinKind::Div ? a / b : a % b);
            }
            case BinKind::AddWrap: return int_value(canon(a + b, t));
            case BinKind::SubWrap: return int_value(canon(a - b, t));
            case BinKind::MulWrap: return int_value(canon(a * b, t));
            case BinKind::BitAnd: return int_value(canon(a & b, t));
            case BinKind::BitOr: return int_value(canon(a | b, t));
            case BinKind::BitXor: return int_value(canon(a ^ b, t));
            case BinKind::Shl: case BinKind::Shr: {
                bool neg = sema::is_signed(bt) && sb < 0;
                if (neg || b >= static_cast<uint64_t>(bits)) {
                    trap("shift count " + int_text(b, bt) + " is out of range for '" + sema::type_name(t) + "'");
                    return Value{};
                }
                if (k == BinKind::Shl) return int_value(canon(a << b, t));
                if (sg) return int_value(static_cast<uint64_t>(sa >> b));
                return int_value(a >> b);
            }
            default: break;
        }
        internal("unknown integer operation");
        return Value{};
    }

    Value unary(Frame& fr, const Rvalue& rv) {
        Value s;
        const Value& a = peek(fr, rv.ops[0], s);
        if (stop_) return Value{};
        const Type* t = rv.type;
        switch (rv.un) {
            case UnKind::Not: return bool_value(!a.i);
            case UnKind::BitNot: return int_value(canon(~a.i, t));
            case UnKind::Canon: return canonical(float_value(a.f));  // Р56
            case UnKind::Neg:
                if (sema::is_float(t)) return float_value(-a.f);
                if (canon(a.i, t) == canon(1ull << (sema::int_bits(t) - 1), t)) {
                    trap("integer overflow in '-' on '" + sema::type_name(t) + "'");
                    return Value{};
                }
                return int_value(canon(0 - a.i, t));
        }
        return Value{};
    }

    Value cast(Frame& fr, const Rvalue& rv) {
        Value s;
        const Value& a = peek(fr, rv.ops[0], s);
        if (stop_) return Value{};
        const Type* from = op_type(fr, rv.ops[0]);
        const Type* to = rv.type;
        switch (rv.cast) {
            case CastKind::Widen: case CastKind::RefUp: return a;
            case CastKind::Wrap: return int_value(canon(a.i, to));
            case CastKind::ToStr:
                if (from->kind == TK::Bool) return str_value(a.i ? "true" : "false");
                if (sema::is_float(from)) return str_value(format_float(a.f, from->kind == TK::F32));
                return str_value(int_text(a.i, from));
            case CastKind::Check: break;
        }
        auto fail = [&](const std::string& v) {
            trap("value " + v + " does not fit '" + sema::type_name(to) + "'");
            return Value{};
        };
        if (sema::is_integer(from) && sema::is_integer(to)) {
            int bits = sema::int_bits(to);
            bool ok;
            if (sema::is_signed(from)) {
                int64_t v = static_cast<int64_t>(a.i);
                ok = sema::is_signed(to) ? fits_signed(v, bits) : v >= 0 && fits_unsigned(static_cast<uint64_t>(v), bits);
            } else {
                ok = sema::is_signed(to) ? a.i <= mask(bits - 1) : fits_unsigned(a.i, bits);
            }
            if (!ok) return fail(int_text(a.i, from));
            return int_value(canon(a.i, to));
        }
        if (sema::is_integer(from)) {  // в плавающее: одно округление к ближайшему
            bool sg = sema::is_signed(from);
            int64_t sv = static_cast<int64_t>(a.i);
            if (to->kind == TK::F32) {
                return float_value(static_cast<double>(sg ? static_cast<float>(sv) : static_cast<float>(a.i)));
            }
            return float_value(sg ? static_cast<double>(sv) : static_cast<double>(a.i));
        }
        double x = a.f;
        if (sema::is_float(to)) {
            if (to->kind == TK::F32 && std::isfinite(x) && std::fabs(x) > static_cast<double>(FLT_MAX)) {
                return fail(format_float(x, false));
            }
            return float_value(to->kind == TK::F32 ? static_cast<double>(static_cast<float>(x)) : x);
        }
        // Из плавающего в целое: отбрасывание дробной части к нулю.
        if (std::isnan(x)) {
            trap("NaN does not fit '" + sema::type_name(to) + "'");
            return Value{};
        }
        double tr = std::trunc(x);
        int bits = sema::int_bits(to);
        if (sema::is_signed(to)) {
            double lim = std::ldexp(1.0, bits - 1);
            if (!(tr >= -lim && tr < lim)) return fail(format_float(x, from->kind == TK::F32));
            return int_value(canon(static_cast<uint64_t>(static_cast<int64_t>(tr)), to));
        }
        double lim = std::ldexp(1.0, bits);
        if (!(tr > -1.0 && tr < lim)) return fail(format_float(x, from->kind == TK::F32));
        return int_value(static_cast<uint64_t>(tr));
    }

    Value slice(Frame& fr, const Rvalue& rv) {
        Value s0, s1;
        int64_t lo = static_cast<int64_t>(peek(fr, rv.ops[0], s0).i);
        int64_t hi = rv.ops.size() > 1 ? static_cast<int64_t>(peek(fr, rv.ops[1], s1).i) : -1;
        Ref r = resolve(fr, rv.place);
        if (stop_) return Value{};
        if (r.byte >= 0 || r.v->k == Value::K::Empty) {
            internal("slice of a non-sequence");
            return Value{};
        }
        const Value& base = *r.v;
        int64_t n = static_cast<int64_t>(base.k == Value::K::Str ? base.s.size() : seq_len(base));
        if (rv.ops.size() < 2) hi = n;
        if (lo < 0 || lo > hi || hi > n) {
            trap("slice " + std::to_string(lo) + ".." + std::to_string(hi) + " is out of bounds for length " +
                 std::to_string(n));
            return Value{};
        }
        if (base.k == Value::K::Str) {
            // Строка остаётся UTF-8: концы — на границах символов.
            auto inside = [&](int64_t i) { return i < n && (static_cast<uint8_t>(base.s[static_cast<size_t>(i)]) & 0xC0) == 0x80; };
            if (inside(lo) || inside(hi)) {
                trap("slice " + std::to_string(lo) + ".." + std::to_string(hi) +
                     " of a str does not fall on character boundaries");
                return Value{};
            }
            return str_value(base.s.substr(static_cast<size_t>(lo), static_cast<size_t>(hi - lo)));
        }
        Value v;
        v.k = Value::K::Slice;
        if (base.k == Value::K::Slice) {
            v.base = base.base;
            v.lo = base.lo + lo;
            v.hi = base.lo + hi;
        } else {
            v.base = &base;
            v.lo = lo;
            v.hi = hi;
        }
        return v;
    }

    Value intrinsic(Frame& fr, const Rvalue& rv) {
        if (rv.intrinsic == Intrinsic::Probe) {  // проверки места без чтения
            resolve(fr, rv.place);
            return Value{};
        }
        // Операнды — копии: изменяющий метод не должен видеть их через себя.
        std::vector<Value> ops;
        for (size_t i = 0; i < rv.ops.size(); i++) {
            const Operand& o = rv.ops[i];
            if (o.kind == OpKind::Move) {
                ops.push_back(take(fr, o));
            } else if (rv.has_place) {
                Value s;
                ops.push_back(peek(fr, o, s));
            } else {
                ops.emplace_back();
            }
            if (stop_) return Value{};
        }
        // Читающие встроенные — без копий.
        auto arg = [&](size_t i, Value& s) -> const Value& {
            if (rv.has_place || rv.ops[i].kind == OpKind::Move) return ops[i];
            return peek(fr, rv.ops[i], s);
        };
        Value s0, s1;
        switch (rv.intrinsic) {
            case Intrinsic::Len: {
                const Value& a = arg(0, s0);
                if (op_type(fr, rv.ops[0])->kind == TK::Map) return int_value(a.items.size() / 2);
                if (op_type(fr, rv.ops[0])->kind == TK::Pool) return int_value(pool_live(a));
                return int_value(a.k == Value::K::Str ? a.s.size() : seq_len(a));
            }
            case Intrinsic::MapGet: case Intrinsic::MapContains: {
                const Value& m = arg(0, s0);
                int64_t i = map_find(m, arg(1, s1));
                if (rv.intrinsic == Intrinsic::MapContains) return bool_value(i >= 0);
                Value out = seq_value(i >= 0 ? 1 : 0);
                if (i >= 0) out.items.push_back(m.items[2 * static_cast<size_t>(i) + 1]);
                return out;
            }
            case Intrinsic::PoolGet: case Intrinsic::PoolContains: {
                const Value& p = arg(0, s0);
                int64_t i = pool_find(p, arg(1, s1));
                if (rv.intrinsic == Intrinsic::PoolContains) return bool_value(i >= 0);
                Value out = seq_value(i >= 0 ? 1 : 0);
                if (i >= 0) out.items.push_back(p.items[static_cast<size_t>(i)].items[0]);
                return out;
            }
            case Intrinsic::PoolHandles: {
                const Value& p = arg(0, s0);
                Value out = seq_value(0);
                for (size_t i = 0; i < p.items.size(); i++) {
                    if (p.items[i].tag == 1) out.items.push_back(handle_value(i, p.items[i].i));
                }
                return out;
            }
            case Intrinsic::PoolSlots: return int_value(arg(0, s0).items.size());
            case Intrinsic::PoolLive: {
                const Value& p = arg(0, s0);
                uint64_t i = arg(1, s1).i;
                return bool_value(i < p.items.size() && p.items[i].tag == 1);
            }
            case Intrinsic::PoolAdd: case Intrinsic::PoolRemove: case Intrinsic::PoolClear: case Intrinsic::PoolReserve:
                return pool_method(fr, rv, ops);
            case Intrinsic::CArenaMark: return int_value(0);  // Р62: арена — только у сборки
            case Intrinsic::CArenaRelease: return Value{};
            case Intrinsic::MapKeys: {
                const Value& m = arg(0, s0);
                Value out = seq_value(0);
                for (size_t i = 0; i < m.items.size(); i += 2) out.items.push_back(m.items[i]);
                merge_sort(out.items, op_type(fr, rv.ops[0])->key);  // ключи по возрастанию
                return out;
            }
            case Intrinsic::MapOrder: {  // номера пар по возрастанию ключа: та же сортировка
                const Value& m = arg(0, s0);
                const Type* kt = op_type(fr, rv.ops[0])->key;
                std::vector<size_t> pos(m.items.size() / 2);
                for (size_t i = 0; i < pos.size(); i++) pos[i] = i;
                merge_sort(pos, [&](size_t a, size_t b) { return less(m.items[2 * a], m.items[2 * b], kt); });
                Value out = seq_value(0);
                for (size_t i : pos) out.items.push_back(int_value(i));
                return out;
            }
            case Intrinsic::MapSet: case Intrinsic::MapRemove: case Intrinsic::MapClear: case Intrinsic::MapReserve:
                return map_method(fr, rv, ops);
            case Intrinsic::Repeat: {
                const Value& n = arg(1, s1);
                const Type* nt = op_type(fr, rv.ops[1]);
                if (sema::is_signed(nt) && static_cast<int64_t>(n.i) < 0) {
                    trap("repeat count " + int_text(n.i, nt) + " is negative");
                    return Value{};
                }
                if (n.i > kMaxLen) {
                    trap("out of memory");
                    return Value{};
                }
                Value v = seq_value(0);
                const Value& x = arg(0, s0);
                v.items.assign(static_cast<size_t>(n.i), x);
                return v;
            }
            case Intrinsic::Min: case Intrinsic::Max: {
                const Value& a = arg(0, s0);
                const Value& b = arg(1, s1);
                const Type* t = rv.type;
                bool a_less = less(a, b, t);
                return rv.intrinsic == Intrinsic::Min ? (a_less ? a : b) : (a_less ? b : a);
            }
            case Intrinsic::Abs: {
                const Value& a = arg(0, s0);
                const Type* t = rv.type;
                if (sema::is_float(t)) return float_value(std::fabs(a.f));
                int64_t v = static_cast<int64_t>(a.i);
                if (canon(a.i, t) == canon(1ull << (sema::int_bits(t) - 1), t)) {
                    trap("integer overflow in 'abs' on '" + sema::type_name(t) + "'");
                    return Value{};
                }
                return int_value(canon(static_cast<uint64_t>(v < 0 ? -v : v), t));
            }
            case Intrinsic::CheckedAdd: case Intrinsic::CheckedSub: case Intrinsic::CheckedMul:
            case Intrinsic::CheckedDiv: case Intrinsic::CheckedNeg: {
                const Type* t = rv.type->elem;
                Value out = seq_value(0);
                uint64_t r = 0;
                bool neg = rv.intrinsic == Intrinsic::CheckedNeg;
                if (checked(rv.intrinsic, arg(0, s0).i, neg ? 0 : arg(1, s1).i, t, r)) {
                    out.tag = 1;
                    out.items.push_back(int_value(r));
                }
                return out;
            }
            case Intrinsic::ParseInt: {
                int64_t v = 0;
                Value out = seq_value(0);
                if (parse_int(arg(0, s0).s, v)) {
                    out.tag = 1;
                    out.items.push_back(int_value(static_cast<uint64_t>(v)));
                }
                return out;
            }
            case Intrinsic::IsDigit: return bool_value(is_digit(arg(0, s0).i));
            case Intrinsic::IsAlpha: return bool_value(is_alpha(arg(0, s0).i));
            case Intrinsic::IsSpace: return bool_value(is_space(arg(0, s0).i));
            case Intrinsic::IsAlnum: {
                uint64_t b = arg(0, s0).i;
                return bool_value(is_alpha(b) || is_digit(b));
            }
            case Intrinsic::Print: case Intrinsic::Println: case Intrinsic::Eprintln: {
                const Value& a = arg(0, s0);
                FILE* out = rv.intrinsic == Intrinsic::Eprintln ? stderr : stdout;
                if (out == stderr) std::fflush(stdout);
                std::fwrite(a.s.data(), 1, a.s.size(), out);
                if (rv.intrinsic != Intrinsic::Print) std::fputc('\n', out);
                return Value{};
            }
            case Intrinsic::ListContains: {
                const Value& xs = arg(0, s0);
                const Value& x = arg(1, s1);
                const Type* elem = op_type(fr, rv.ops[0])->elem;
                for (size_t i = 0; i < seq_len(xs); i++) {
                    if (equal(seq_at(xs, i), x, elem)) return bool_value(true);
                }
                return bool_value(false);
            }
            case Intrinsic::StrTrim: {
                const std::string& s = arg(0, s0).s;
                size_t a = 0, b = s.size();
                while (a < b && is_space(static_cast<uint8_t>(s[a]))) a++;
                while (b > a && is_space(static_cast<uint8_t>(s[b - 1]))) b--;
                return str_value(s.substr(a, b - a));
            }
            case Intrinsic::StrFind: {
                size_t p = arg(0, s0).s.find(arg(1, s1).s);
                Value out = seq_value(0);
                if (p != std::string::npos) {
                    out.tag = 1;
                    out.items.push_back(int_value(p));
                }
                return out;
            }
            case Intrinsic::StrStartsWith: {
                const std::string& s = arg(0, s0).s;
                const std::string& pre = arg(1, s1).s;
                return bool_value(s.compare(0, pre.size(), pre) == 0 && s.size() >= pre.size());
            }
            case Intrinsic::StrSplit: {
                const std::string& s = arg(0, s0).s;
                const std::string& sep = arg(1, s1).s;
                if (sep.empty()) {
                    trap("split separator is empty");
                    return Value{};
                }
                Value out = seq_value(0);
                size_t a = 0;
                for (;;) {
                    size_t p = s.find(sep, a);
                    if (p == std::string::npos) {
                        out.items.push_back(str_value(s.substr(a)));
                        return out;
                    }
                    out.items.push_back(str_value(s.substr(a, p - a)));
                    a = p + sep.size();
                }
            }
            case Intrinsic::ArgsLen: return int_value(args_.size());
            case Intrinsic::ArgAt: {
                int64_t i = static_cast<int64_t>(arg(0, s0).i);
                if (i < 0 || static_cast<uint64_t>(i) >= args_.size()) {
                    trap("index " + std::to_string(i) + " is out of bounds for length " + std::to_string(args_.size()));
                    return Value{};
                }
                return str_value(args_[static_cast<size_t>(i)]);
            }
            case Intrinsic::ReadFile: return read_file(fr, rv, ops[0].s);
            default: break;
        }
        return list_method(fr, rv, ops);
    }

    // __read_file (Р51): 0 и текст в месте; иначе 1 + номер варианта os.IoError
    // (NotFound, PermissionDenied, InvalidUtf8, Other); место не меняется.
    Value read_file(Frame& fr, const Rvalue& rv, const std::string& path) {
        int code = 0;
        std::string text;
        FILE* f = path.find('\0') == std::string::npos ? std::fopen(path.c_str(), "rb") : nullptr;
        if (!f) {
            code = errno == ENOENT || errno == ENOTDIR ? 1 : errno == EACCES || errno == EPERM ? 2 : 4;
            if (path.find('\0') != std::string::npos) code = 1;
        } else {
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
                text.append(buf, n);
                if (text.size() > kMaxLen) {
                    std::fclose(f);
                    trap("out of memory");
                    return Value{};
                }
            }
            if (std::ferror(f)) code = 4;  // например, каталог: EISDIR
            std::fclose(f);
            for (size_t i = 0; code == 0 && i < text.size();) {
                uint32_t cp = 0;
                uint32_t len = decode_utf8(text, i, cp);
                if (len == 0) code = 3;
                i += len ? len : 1;
            }
        }
        if (code == 0) {
            Ref r = resolve(fr, rv.place, true);
            if (stop_) return Value{};
            *r.v = str_value(std::move(text));
        }
        return int_value(static_cast<uint64_t>(code));
    }

    // `reserve(n)` (Р68): ёмкость не видна, проверяется только n.
    void reserve(const Value& n) {
        int64_t k = static_cast<int64_t>(n.i);
        if (k < 0) trap("reserve count " + std::to_string(k) + " is negative");
        else if (n.i > kMaxLen) trap("out of memory");
    }

    Value pool_method(Frame& fr, const Rvalue& rv, std::vector<Value>& ops) {
        Ref r = resolve(fr, rv.place, true);
        if (stop_) return Value{};
        if (r.byte >= 0 || r.v->k != Value::K::Seq) {
            internal("pool method on a non-Pool");
            return Value{};
        }
        std::vector<Value>& slots = r.v->items;
        switch (rv.intrinsic) {
            case Intrinsic::PoolAdd: {  // свободный слот с наименьшим номером, иначе новый
                size_t i = 0;
                while (i < slots.size() && (slots[i].tag == 1 || slots[i].i >= kGenLimit)) i++;
                if (i == slots.size()) {
                    if (slots.size() >= kMaxLen) {
                        trap("out of memory");
                        return Value{};
                    }
                    Value s = seq_value(0);
                    s.i = 1;
                    slots.push_back(std::move(s));
                }
                slots[i].tag = 1;
                slots[i].items.clear();
                slots[i].items.push_back(std::move(ops[0]));
                return handle_value(i, slots[i].i);
            }
            case Intrinsic::PoolRemove: {
                int64_t i = pool_find(*r.v, ops[0]);
                Value out = seq_value(i >= 0 ? 1 : 0);
                if (i >= 0) {
                    out.items.push_back(std::move(slots[static_cast<size_t>(i)].items[0]));
                    pool_release(slots[static_cast<size_t>(i)]);
                }
                return out;
            }
            case Intrinsic::PoolClear:
                for (Value& s : slots) {
                    if (s.tag == 1) pool_release(s);
                }
                return Value{};
            case Intrinsic::PoolReserve: reserve(ops[0]); return Value{};
            default: break;
        }
        internal("unknown intrinsic");
        return Value{};
    }

    Value map_method(Frame& fr, const Rvalue& rv, std::vector<Value>& ops) {
        Ref r = resolve(fr, rv.place, true);
        if (stop_) return Value{};
        if (r.byte >= 0 || r.v->k != Value::K::Seq) {
            internal("map method on a non-Map");
            return Value{};
        }
        std::vector<Value>& items = r.v->items;
        switch (rv.intrinsic) {
            case Intrinsic::MapSet: {
                int64_t i = map_find(*r.v, ops[0]);
                if (i >= 0) {
                    items[2 * static_cast<size_t>(i) + 1] = std::move(ops[1]);
                    return Value{};
                }
                if (items.size() / 2 >= kMaxLen) {
                    trap("out of memory");
                    return Value{};
                }
                items.push_back(std::move(ops[0]));
                items.push_back(std::move(ops[1]));
                return Value{};
            }
            case Intrinsic::MapRemove: {
                int64_t i = map_find(*r.v, ops[0]);
                Value out = seq_value(i >= 0 ? 1 : 0);
                if (i >= 0) {
                    auto at = items.begin() + 2 * i;
                    out.items.push_back(std::move(at[1]));
                    items.erase(at, at + 2);
                }
                return out;
            }
            case Intrinsic::MapClear: items.clear(); return Value{};
            case Intrinsic::MapReserve: reserve(ops[0]); return Value{};
            default: break;
        }
        internal("unknown intrinsic");
        return Value{};
    }

    Value list_method(Frame& fr, const Rvalue& rv, std::vector<Value>& ops) {
        Ref r = resolve(fr, rv.place, true);
        if (stop_) return Value{};
        if (r.byte >= 0 || r.v->k != Value::K::Seq) {
            internal("list method on a non-List");
            return Value{};
        }
        std::vector<Value>& items = r.v->items;
        auto position = [&](const Value& v, size_t limit, const char* what) -> int64_t {
            int64_t i = static_cast<int64_t>(v.i);
            if (i < 0 || static_cast<uint64_t>(i) > limit) {
                trap(std::string(what) + " " + std::to_string(i) + " is out of bounds for length " +
                     std::to_string(items.size()));
                return -1;
            }
            return i;
        };
        switch (rv.intrinsic) {
            case Intrinsic::ListPush:
                if (items.size() >= kMaxLen) {
                    trap("out of memory");
                    return Value{};
                }
                items.push_back(std::move(ops[0]));
                return Value{};
            case Intrinsic::ListPop: {
                Value out = seq_value(0);
                if (!items.empty()) {
                    out.tag = 1;
                    out.items.push_back(std::move(items.back()));
                    items.pop_back();
                }
                return out;
            }
            case Intrinsic::ListInsert: {
                int64_t i = position(ops[0], items.size(), "insert position");
                if (i < 0) return Value{};
                if (items.size() >= kMaxLen) {
                    trap("out of memory");
                    return Value{};
                }
                items.insert(items.begin() + i, std::move(ops[1]));
                return Value{};
            }
            case Intrinsic::ListRemove: {
                if (items.empty() || static_cast<int64_t>(ops[0].i) < 0 || ops[0].i >= items.size()) {
                    trap("index " + std::to_string(static_cast<int64_t>(ops[0].i)) + " is out of bounds for length " +
                         std::to_string(items.size()));
                    return Value{};
                }
                Value v = std::move(items[ops[0].i]);
                items.erase(items.begin() + static_cast<int64_t>(ops[0].i));
                return v;
            }
            case Intrinsic::ListClear: items.clear(); return Value{};
            case Intrinsic::ListReserve: reserve(ops[0]); return Value{};
            case Intrinsic::ListSort: {
                const Type* t = place_type(p_, fn(fr), rv.place);
                merge_sort(items, t->elem);
                return Value{};
            }
            default: break;
        }
        internal("unknown intrinsic");
        return Value{};
    }
};

}  // namespace

RunResult run(const Program& p, const SourceMap& sources, const std::vector<std::string>& args) {
    return Interp(p, sources, args).run();
}

}  // namespace dp::ir
