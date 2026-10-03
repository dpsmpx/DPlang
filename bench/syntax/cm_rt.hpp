// Опорный заголовок для C++, который выдаёт переводчик вариантов
// синтаксиса (bench/syntax/cm.py). Одноразовый прототип v0.0, не runtime
// языка: проверки здесь лишь повторяют правила ядра (Р11, Р36) настолько,
// чтобы тесты эталонного набора ловили нарушения.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace cm {

[[noreturn]] inline void trap(const char* what) {
    std::fprintf(stderr, "ловушка: %s\n", what);
    std::abort();
}

inline void check(bool ok, const char* what) {
    if (!ok) trap(what);
}

// --- ошибки и «значение или ошибка» ---

struct Err {
    const char* tag;
};

inline bool tag_eq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }
inline bool err_is(const Err& e, const char* tag) { return tag_eq(e.tag, tag); }

template <class T>
struct [[nodiscard]] Result {
    const char* err = nullptr;
    T value{};
    Result(T v) : value(std::move(v)) {}
    Result(Err e) : err(e.tag) {}
    bool ok() const { return err == nullptr; }
};

template <>
struct [[nodiscard]] Result<void> {
    const char* err = nullptr;
    Result() {}
    Result(Err e) : err(e.tag) {}
    bool ok() const { return err == nullptr; }
};

template <class R>
bool err_is(const R& r, const char* tag) { return tag_eq(r.err, tag); }

// --- целые: проверка переполнения, деление, сдвиги, приведение ---

template <class A, class B>
using CT = std::common_type_t<A, B>;

template <class A, class B>
auto add(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        CT<A, B> r;
        if (__builtin_add_overflow((CT<A, B>)a, (CT<A, B>)b, &r)) trap("переполнение при +");
        return r;
    } else {
        return a + b;
    }
}

template <class A, class B>
auto sub(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        CT<A, B> r;
        if (__builtin_sub_overflow((CT<A, B>)a, (CT<A, B>)b, &r)) trap("переполнение при -");
        return r;
    } else {
        return a - b;
    }
}

template <class A, class B>
auto mul(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        CT<A, B> r;
        if (__builtin_mul_overflow((CT<A, B>)a, (CT<A, B>)b, &r)) trap("переполнение при *");
        return r;
    } else {
        return a * b;
    }
}

template <class A, class B>
auto div(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        using T = CT<A, B>;
        if ((T)b == 0) trap("деление на ноль");
        if constexpr (std::is_signed_v<T>)
            if ((T)a == std::numeric_limits<T>::min() && (T)b == -1) trap("переполнение при /");
        return (T)((T)a / (T)b);
    } else {
        return a / b;
    }
}

template <class A, class B>
auto mod(const A& a, const B& b) {
    using T = CT<A, B>;
    static_assert(std::is_integral_v<T>, "% только для целых");
    if ((T)b == 0) trap("деление на ноль");
    if constexpr (std::is_signed_v<T>)
        if ((T)a == std::numeric_limits<T>::min() && (T)b == -1) return (T)0;
    return (T)((T)a % (T)b);
}

template <class A, class B>
auto wadd(const A& a, const B& b) {
    using T = CT<A, B>;
    using U = std::make_unsigned_t<T>;
    return (T)(U)((U)(T)a + (U)(T)b);
}

template <class A, class B>
auto wsub(const A& a, const B& b) {
    using T = CT<A, B>;
    using U = std::make_unsigned_t<T>;
    return (T)(U)((U)(T)a - (U)(T)b);
}

template <class A, class B>
auto wmul(const A& a, const B& b) {
    using T = CT<A, B>;
    using U = std::make_unsigned_t<T>;
    return (T)(U)((U)(T)a * (U)(T)b);
}

template <class A, class B>
A shl(const A& a, const B& n) {
    static_assert(std::is_integral_v<A> && std::is_integral_v<B>, "сдвиг только для целых");
    if (std::cmp_less(n, 0) || std::cmp_greater_equal(n, sizeof(A) * 8)) trap("сдвиг на ширину типа");
    using U = std::make_unsigned_t<A>;
    return (A)(U)((U)a << n);
}

template <class A, class B>
A shr(const A& a, const B& n) {
    static_assert(std::is_integral_v<A> && std::is_integral_v<B>, "сдвиг только для целых");
    if (std::cmp_less(n, 0) || std::cmp_greater_equal(n, sizeof(A) * 8)) trap("сдвиг на ширину типа");
    return (A)(a >> n);
}

template <class A>
A neg(const A& a) {
    if constexpr (std::is_integral_v<A>) {
        if constexpr (std::is_signed_v<A>) {
            if (a == std::numeric_limits<A>::min()) trap("переполнение при -");
        } else {
            if (a != 0) trap("минус у беззнакового");
        }
        return (A)(-a);
    } else {
        return -a;
    }
}

template <class A>
A bnot(const A& a) { return (A)~a; }

template <class T, class X>
T cast(const X& x) {
    if constexpr (std::is_same_v<T, X>) {
        return x;
    } else if constexpr (std::is_integral_v<T> && std::is_integral_v<X>) {
        if (!std::in_range<T>(x)) trap("значение не помещается в тип");
        return (T)x;
    } else if constexpr (std::is_integral_v<T> && std::is_floating_point_v<X>) {
        if (std::isnan(x)) trap("NaN в целое");
        long double t = std::trunc((long double)x);
        if (t < (long double)std::numeric_limits<T>::min() || t > (long double)std::numeric_limits<T>::max())
            trap("значение не помещается в тип");
        return (T)t;
    } else {
        return (T)x;
    }
}

template <class T, class X>
T wrap(const X& x) {
    static_assert(std::is_integral_v<T> && std::is_integral_v<X>, "wrap только для целых");
    return (T)(std::make_unsigned_t<T>)x;
}

template <class A>
A abs(const A& a) { return a < 0 ? neg(a) : a; }

template <class A, class B>
auto min(const A& a, const B& b) { return (CT<A, B>)a < (CT<A, B>)b ? (CT<A, B>)a : (CT<A, B>)b; }

template <class A, class B>
auto max(const A& a, const B& b) { return (CT<A, B>)a < (CT<A, B>)b ? (CT<A, B>)b : (CT<A, B>)a; }

// --- коллекции и строки ---

template <class C>
int64_t len(const C& c) { return (int64_t)c.size(); }

template <class C, class I>
void bounds(const C& c, I i) {
    static_assert(std::is_integral_v<I>, "индекс — целое");
    if (std::cmp_less(i, 0) || std::cmp_greater_equal(i, c.size())) trap("индекс за границей");
}

template <class T, class I>
T& at(std::vector<T>& c, I i) { bounds(c, i); return c[(size_t)i]; }
template <class T, class I>
const T& at(const std::vector<T>& c, I i) { bounds(c, i); return c[(size_t)i]; }
template <class T, class I>
const T& at(std::span<const T> c, I i) { bounds(c, i); return c[(size_t)i]; }
template <class I>
uint8_t at(const std::string& s, I i) { bounds(s, i); return (uint8_t)s[(size_t)i]; }

template <class C, class A, class B>
void range_ok(const C& c, A a, B b) {
    if (std::cmp_less(a, 0) || std::cmp_greater(a, b) || std::cmp_greater(b, c.size())) trap("срез за границей");
}

template <class T, class A, class B>
std::span<const T> slice(const std::vector<T>& c, A a, B b) { range_ok(c, a, b); return std::span<const T>(c.data() + a, (size_t)(b - a)); }
template <class T, class A, class B>
std::span<const T> slice(std::span<const T> c, A a, B b) { range_ok(c, a, b); return c.subspan((size_t)a, (size_t)(b - a)); }
template <class A, class B>
std::string slice(const std::string& s, A a, B b) { range_ok(s, a, b); return s.substr((size_t)a, (size_t)(b - a)); }

template <class T, class N>
std::vector<T> repeat(const T& v, N n) {
    if (std::cmp_less(n, 0)) trap("repeat: отрицательная длина");
    return std::vector<T>((size_t)n, v);
}

inline std::span<const uint8_t> each(const std::string& s) { return {(const uint8_t*)s.data(), s.size()}; }
template <class C>
const C& each(const C& c) { return c; }

template <class X>
std::string to_str(const X& x) {
    if constexpr (std::is_same_v<X, std::string>) return x;
    else if constexpr (std::is_same_v<X, bool>) return x ? "true" : "false";
    else return std::to_string(x);
}

template <class T, class V>
void m_push(std::vector<T>& c, const V& v) { c.push_back(v); }
template <class T>
std::optional<T> m_pop(std::vector<T>& c) {
    if (c.empty()) return std::nullopt;
    T v = std::move(c.back());
    c.pop_back();
    return v;
}
template <class T, class I, class V>
void m_insert(std::vector<T>& c, I i, const V& v) {
    if (std::cmp_less(i, 0) || std::cmp_greater(i, c.size())) trap("индекс за границей");
    c.insert(c.begin() + (ptrdiff_t)i, v);
}
template <class T, class I>
T m_remove(std::vector<T>& c, I i) {
    bounds(c, i);
    T v = std::move(c[(size_t)i]);
    c.erase(c.begin() + (ptrdiff_t)i);
    return v;
}
template <class T>
void m_clear(std::vector<T>& c) { c.clear(); }
template <class T>
void m_sort(std::vector<T>& c) { std::stable_sort(c.begin(), c.end()); }
template <class C, class V>
bool m_contains(const C& c, const V& v) {
    for (const auto& x : c)
        if (x == v) return true;
    return false;
}

inline bool is_space(uint8_t b) { return b == ' ' || b == '\t' || b == '\n' || b == '\r'; }
inline bool is_digit(uint8_t b) { return b >= '0' && b <= '9'; }
inline bool is_alpha(uint8_t b) { return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z'); }
inline bool is_alnum(uint8_t b) { return is_alpha(b) || is_digit(b); }

inline std::string m_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space((uint8_t)s[a])) a++;
    while (b > a && is_space((uint8_t)s[b - 1])) b--;
    return s.substr(a, b - a);
}
inline std::optional<int64_t> m_find(const std::string& s, const std::string& sub) {
    size_t p = s.find(sub);
    if (p == std::string::npos) return std::nullopt;
    return (int64_t)p;
}
inline bool m_starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
inline std::vector<std::string> m_split(const std::string& s, const std::string& sep) {
    if (sep.empty()) trap("split: пустой разделитель");
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t p = s.find(sep, a);
        if (p == std::string::npos) { out.push_back(s.substr(a)); return out; }
        out.push_back(s.substr(a, p - a));
        a = p + sep.size();
    }
}

// Десятичное целое со знаком, без пробелов; нет значения — не число или
// не помещается в i64.
inline std::optional<int64_t> parse_int(const std::string& s) {
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { negative = s[i] == '-'; i++; }
    if (i == s.size()) return std::nullopt;
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t v = 0;
    for (; i < s.size(); i++) {
        if (!is_digit((uint8_t)s[i])) return std::nullopt;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (limit - d) / 10) return std::nullopt;
        v = v * 10 + d;
    }
    return negative ? (int64_t)(0 - v) : (int64_t)v;
}

template <class O, class D>
auto or_else(const O& o, const D& d) { return o.has_value() ? *o : d; }

// --- defer ---

template <class F>
struct Defer {
    F f;
    explicit Defer(F fn) : f(fn) {}
    ~Defer() { f(); }
};

}  // namespace cm
