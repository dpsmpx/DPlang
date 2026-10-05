/* Runtime DPlang: ядро без зависимости от платформы (docs/runtime/README.md).
 * Тексты ловушек совпадают с эталонным интерпретатором
 * (compiler/ir/interp.cpp): различающие тесты сравнивают вывод байт в байт. */
#include "dp_rt.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t dp_depth = 0;

/* --- ловушки */

static _Noreturn void dp_die(const char* loc, const char* msg) {
    fflush(stdout);
    if (loc && *loc) fprintf(stderr, "%s: trap: %s\n", loc, msg);
    else fprintf(stderr, "trap: %s\n", msg);
    fflush(stderr);
    _Exit(101);
}

_Noreturn void dp_trap(const char* loc, const char* msg) { dp_die(loc, msg); }

_Noreturn void dp_trapf(const char* loc, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dp_die(loc, buf);
}

_Noreturn void dp_unreachable(const char* where) {
    fflush(stdout);
    fprintf(stderr, "internal error: reached code marked unreachable in %s\n", where);
    fflush(stderr);
    _Exit(3);
}

_Noreturn void dp_overflow(const char* op, const char* tname, const char* loc) {
    dp_trapf(loc, "integer overflow in '%s' on '%s'", op, tname);
}

_Noreturn void dp_bad_shift_s(int64_t n, const char* tname, const char* loc) {
    dp_trapf(loc, "shift count %" PRId64 " is out of range for '%s'", n, tname);
}

_Noreturn void dp_bad_shift_u(uint64_t n, const char* tname, const char* loc) {
    dp_trapf(loc, "shift count %" PRIu64 " is out of range for '%s'", n, tname);
}

/* --- память */

void* dp_alloc(int64_t count, size_t elem, const char* loc) {
    if (count <= 0) return NULL;
    if (elem != 0 && count > DP_MAX_BYTES / (int64_t)elem) dp_trap(loc, "out of memory");
    void* p = malloc((size_t)count * elem);
    if (!p) dp_trap(loc, "out of memory");
    return p;
}

void* dp_realloc(void* p, int64_t count, size_t elem, const char* loc) {
    if (count <= 0) {
        free(p);
        return NULL;
    }
    if (elem != 0 && count > DP_MAX_BYTES / (int64_t)elem) dp_trap(loc, "out of memory");
    void* q = realloc(p, (size_t)count * elem);
    if (!q) dp_trap(loc, "out of memory");
    return q;
}

void dp_free(void* p) { free(p); }

/* --- запись плавающего (Р48)
 *
 * Тот же алгоритм, что compiler/support/format.h: кратчайшая запись,
 * читаемая обратно в то же число (у f32 — в то же f32); обычная форма при
 * десятичном порядке от −4 до 15, без хвоста `.0`, иначе экспоненциальная. */

/* double → float с округлением к ближайшему, как в IEEE 754, без
 * неопределённого поведения C у значений вне диапазона float. */
static float dp_to_f32(double x) {
    if (isnan(x) || isinf(x)) return (float)x;
    double a = fabs(x);
    if (a > (double)FLT_MAX) {
        double half_ulp = ldexp(1.0, 103);
        float r = a < (double)FLT_MAX + half_ulp ? FLT_MAX : INFINITY;
        return x < 0 ? -r : r;
    }
    return (float)x;
}

static void dp_format_float(double v, bool f32, char* out, size_t cap) {
    if (isnan(v)) {
        snprintf(out, cap, "nan");
        return;
    }
    if (isinf(v)) {
        snprintf(out, cap, "%s", v < 0 ? "-inf" : "inf");
        return;
    }
    if (v == 0) {
        snprintf(out, cap, "%s", signbit(v) ? "-0" : "0");
        return;
    }
    char buf[48];
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, v);
        double back = strtod(buf, NULL);
        if (f32 ? dp_to_f32(back) == (float)v : back == v) break;
    }
    /* buf: [-]d[.ddd]e±XX — цифры и десятичный порядок. */
    bool neg = buf[0] == '-';
    char* e = strchr(buf, 'e');
    char digits[24];
    size_t nd = 0;
    for (char* c = buf + (neg ? 1 : 0); c < e; c++) {
        if (*c != '.') digits[nd++] = *c;
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = 0;
    int exp = atoi(e + 1);
    char* o = out;
    char* end = out + cap - 1;
#define DP_PUT(ch) \
    do { \
        if (o < end) *o++ = (ch); \
    } while (0)
    if (neg) DP_PUT('-');
    if (exp < -4 || exp >= 16) {
        DP_PUT(digits[0]);
        if (nd > 1) {
            DP_PUT('.');
            for (size_t i = 1; i < nd; i++) DP_PUT(digits[i]);
        }
        *o = 0;
        snprintf(o, (size_t)(end - o) + 1, "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
        return;
    }
    if (exp < 0) {
        DP_PUT('0');
        DP_PUT('.');
        for (int i = 0; i < -exp - 1; i++) DP_PUT('0');
        for (size_t i = 0; i < nd; i++) DP_PUT(digits[i]);
    } else {
        size_t int_len = (size_t)exp + 1;
        for (size_t i = 0; i < int_len; i++) DP_PUT(i < nd ? digits[i] : '0');
        if (nd > int_len) {
            DP_PUT('.');
            for (size_t i = int_len; i < nd; i++) DP_PUT(digits[i]);
        }
    }
#undef DP_PUT
    *o = 0;
}

/* --- преобразования (trap.convert) */

int64_t dp_cast_s_to_s(int64_t v, int bits, const char* to, const char* loc) {
    if (v < dp_smin(bits) || v > dp_smax(bits)) dp_trapf(loc, "value %" PRId64 " does not fit '%s'", v, to);
    return v;
}

uint64_t dp_cast_s_to_u(int64_t v, int bits, const char* to, const char* loc) {
    if (v < 0 || (uint64_t)v > dp_umax(bits)) dp_trapf(loc, "value %" PRId64 " does not fit '%s'", v, to);
    return (uint64_t)v;
}

int64_t dp_cast_u_to_s(uint64_t v, int bits, const char* to, const char* loc) {
    if (v > (uint64_t)dp_smax(bits)) dp_trapf(loc, "value %" PRIu64 " does not fit '%s'", v, to);
    return (int64_t)v;
}

uint64_t dp_cast_u_to_u(uint64_t v, int bits, const char* to, const char* loc) {
    if (v > dp_umax(bits)) dp_trapf(loc, "value %" PRIu64 " does not fit '%s'", v, to);
    return v;
}

static _Noreturn void dp_float_misfit(double x, bool from_f32, const char* to, const char* loc) {
    char text[64];
    dp_format_float(x, from_f32, text, sizeof text);
    dp_trapf(loc, "value %s does not fit '%s'", text, to);
}

int64_t dp_cast_f_to_s(double x, bool from_f32, int bits, const char* to, const char* loc) {
    if (isnan(x)) dp_trapf(loc, "NaN does not fit '%s'", to);
    double tr = trunc(x);
    double lim = ldexp(1.0, bits - 1);
    if (!(tr >= -lim && tr < lim)) dp_float_misfit(x, from_f32, to, loc);
    return (int64_t)tr;
}

uint64_t dp_cast_f_to_u(double x, bool from_f32, int bits, const char* to, const char* loc) {
    if (isnan(x)) dp_trapf(loc, "NaN does not fit '%s'", to);
    double tr = trunc(x);
    double lim = ldexp(1.0, bits);
    if (!(tr > -1.0 && tr < lim)) dp_float_misfit(x, from_f32, to, loc);
    return (uint64_t)tr;
}

float dp_cast_f64_to_f32(double x, const char* loc) {
    if (isfinite(x) && fabs(x) > (double)FLT_MAX) dp_float_misfit(x, false, "f32", loc);
    return (float)x;
}

/* --- строки */

dp_str dp_str_own(const uint8_t* p, int64_t n, const char* loc) {
    dp_str s = {NULL, 0};
    if (n <= 0) return s;
    s.ptr = (uint8_t*)dp_alloc(n, 1, loc);
    memcpy(s.ptr, p, (size_t)n);
    s.len = n;
    return s;
}

dp_str dp_str_concat(dp_str a, dp_str b, const char* loc) {
    dp_str s = {NULL, 0};
    if (a.len + b.len > DP_MAX_BYTES) dp_trap(loc, "out of memory");
    if (a.len + b.len == 0) return s;
    s.ptr = (uint8_t*)dp_alloc(a.len + b.len, 1, loc);
    if (a.len) memcpy(s.ptr, a.ptr, (size_t)a.len);
    if (b.len) memcpy(s.ptr + a.len, b.ptr, (size_t)b.len);
    s.len = a.len + b.len;
    return s;
}

bool dp_str_eq(dp_str a, dp_str b) { return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, (size_t)a.len) == 0); }

bool dp_str_lt(dp_str a, dp_str b) {
    int64_t n = a.len < b.len ? a.len : b.len;
    int c = n ? memcmp(a.ptr, b.ptr, (size_t)n) : 0;
    if (c != 0) return c < 0;
    return a.len < b.len;
}

uint8_t dp_str_at(dp_str s, int64_t i, const char* loc) {
    dp_check_index(i, s.len, loc);
    return s.ptr[i];
}

dp_str dp_str_sub(dp_str s, int64_t lo, int64_t hi) { return dp_str_own(s.ptr + lo, hi - lo, NULL); }

dp_str dp_str_slice(dp_str s, int64_t lo, int64_t hi, const char* loc) {
    dp_check_slice(lo, hi, s.len, loc);
    /* Строка остаётся UTF-8: концы — на границах символов. */
    bool inside_lo = lo < s.len && (s.ptr[lo] & 0xC0) == 0x80;
    bool inside_hi = hi < s.len && (s.ptr[hi] & 0xC0) == 0x80;
    if (inside_lo || inside_hi) {
        dp_trapf(loc, "slice %" PRId64 "..%" PRId64 " of a str does not fall on character boundaries", lo, hi);
    }
    return dp_str_sub(s, lo, hi);
}

static dp_str dp_str_from_cstr(const char* c) { return dp_str_own((const uint8_t*)c, (int64_t)strlen(c), NULL); }

dp_str dp_str_from_s(int64_t v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%" PRId64, v);
    return dp_str_from_cstr(buf);
}

dp_str dp_str_from_u(uint64_t v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%" PRIu64, v);
    return dp_str_from_cstr(buf);
}

dp_str dp_str_from_bool(bool v) { return dp_str_from_cstr(v ? "true" : "false"); }

dp_str dp_str_from_f(double v, bool f32) {
    char buf[64];
    dp_format_float(v, f32, buf, sizeof buf);
    return dp_str_from_cstr(buf);
}

bool dp_parse_int(dp_str s, int64_t* out) {
    int64_t i = 0;
    bool negative = false;
    if (i < s.len && (s.ptr[i] == '-' || s.ptr[i] == '+')) {
        negative = s.ptr[i] == '-';
        i++;
    }
    if (i == s.len) return false;
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t v = 0;
    for (; i < s.len; i++) {
        if (!dp_is_digit(s.ptr[i])) return false;
        uint64_t d = (uint64_t)(s.ptr[i] - '0');
        if (v > (limit - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = negative ? dp_sbits(0 - v, 64) : (int64_t)v;
    return true;
}

dp_str dp_str_trim(dp_str s) {
    int64_t a = 0, b = s.len;
    while (a < b && dp_is_space(s.ptr[a])) a++;
    while (b > a && dp_is_space(s.ptr[b - 1])) b--;
    return dp_str_sub(s, a, b);
}

bool dp_str_find_from(dp_str s, dp_str t, int64_t from, int64_t* pos) {
    if (from > s.len) return false;
    if (t.len == 0) {
        *pos = from;
        return true;
    }
    for (int64_t i = from; i + t.len <= s.len; i++) {
        if (memcmp(s.ptr + i, t.ptr, (size_t)t.len) == 0) {
            *pos = i;
            return true;
        }
    }
    return false;
}

bool dp_str_starts_with(dp_str s, dp_str t) { return s.len >= t.len && (t.len == 0 || memcmp(s.ptr, t.ptr, (size_t)t.len) == 0); }

/* --- вывод: stdout буферизован; перед stderr он сбрасывается */

void dp_print(dp_str s) {
    if (s.len) fwrite(s.ptr, 1, (size_t)s.len, stdout);
}

void dp_println(dp_str s) {
    dp_print(s);
    fputc('\n', stdout);
}

void dp_eprintln(dp_str s) {
    fflush(stdout);
    if (s.len) fwrite(s.ptr, 1, (size_t)s.len, stderr);
    fputc('\n', stderr);
}

/* --- индексы и срезы (trap.index) */

int64_t dp_check_index(int64_t i, int64_t len, const char* loc) {
    if (i < 0 || i >= len) dp_trapf(loc, "index %" PRId64 " is out of bounds for length %" PRId64, i, len);
    return i;
}

void dp_check_slice(int64_t lo, int64_t hi, int64_t len, const char* loc) {
    if (lo < 0 || lo > hi || hi > len) {
        dp_trapf(loc, "slice %" PRId64 "..%" PRId64 " is out of bounds for length %" PRId64, lo, hi, len);
    }
}

void dp_check_insert(int64_t i, int64_t len, const char* loc) {
    if (i < 0 || i > len) dp_trapf(loc, "insert position %" PRId64 " is out of bounds for length %" PRId64, i, len);
}

void dp_check_repeat(int64_t n, const char* loc) {
    if (n < 0) dp_trapf(loc, "repeat count %" PRId64 " is negative", n);
}
