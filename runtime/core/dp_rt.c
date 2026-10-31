/* Runtime dpl: ядро без зависимости от платформы (docs/runtime/README.md).
 * Тексты ловушек совпадают с эталонным интерпретатором
 * (compiler/ir/interp.cpp): различающие тесты сравнивают вывод байт в байт. */
#include "dp_rt.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Thread_local int64_t dp_depth = 0;

/* --- `parallel for` (Р69): состояние итерации потока
 *
 * Итерации раздаются частями по возрастанию k. Часть исполняет один
 * поток по порядку, её вывод — в своём буфере: записи «поток (1 — stdout,
 * 2 — stderr), длина, байты». Ловушка в итерации запоминается (наименьшая
 * k) и прерывает часть через longjmp; части с началом дальше её не
 * исполняются. */
typedef struct dp_par_out {
    char* buf;
    size_t len, cap;
} dp_par_out;

typedef struct dp_par_job {
    dp_par_body body;
    void* env;
    uint64_t n, chunk;
    _Atomic uint64_t next;     /* начало следующей части */
    _Atomic uint64_t trap_at;  /* наименьшая итерация с ловушкой; UINT64_MAX — нет */
    char trap_loc[512];
    char trap_msg[512];
    dp_par_out* outs; /* по части */
} dp_par_job;

static void* dp_par_m;
static _Thread_local dp_par_job* dp_par_cur; /* задание, итерацию которого исполняет поток */
static _Thread_local uint64_t dp_par_iter;
static _Thread_local dp_par_out* dp_par_buf;
static _Thread_local jmp_buf* dp_par_jb;

/* Задача блока `tasks` (Р70): свой поток; её вывод — в её буфер
 * до конца блока. */
typedef struct dp_task {
    void (*body)(void*);
    void* env;
    struct dp_task* next;
    struct dp_group* group;
    dp_par_out out;
} dp_task;

static _Thread_local dp_task* dp_task_cur; /* задача, которую исполняет поток */
static _Thread_local dp_mtx* dp_mtx_held;  /* захваченные потоком мьютексы: вершина стека */
static void dp_out_write(const dp_par_out* o);
static void* dp_die_m;
static void* dp_lazy_mutex(void** slot);
static void dp_dl_enter(void);
static void dp_dl_exit(void);
static void dp_dl_block(int* waiting, const char* loc);
static void dp_dl_wake(int* waiting);
static void dp_dl_unblock(int n);

/* --- ловушки */

static _Noreturn void dp_die(const char* loc, const char* msg) {
    dp_par_job* j = dp_par_cur;
    if (j) { /* в итерации `parallel for`: запомнить и прервать часть */
        dp_sys_lock(dp_par_m);
        if (dp_par_iter < atomic_load(&j->trap_at)) {
            atomic_store(&j->trap_at, dp_par_iter);
            snprintf(j->trap_loc, sizeof j->trap_loc, "%s", loc ? loc : "");
            snprintf(j->trap_msg, sizeof j->trap_msg, "%s", msg);
        }
        dp_sys_unlock(dp_par_m);
        longjmp(*dp_par_jb, 1);
    }
    /* Ловушка в задаче: остальные потоки живы — умирает один, первым — вывод
     * задачи. Второй умирающий ждёт здесь, пока первый не завершит процесс. */
    if (dp_task_cur) {
        dp_sys_lock(dp_lazy_mutex(&dp_die_m));
        dp_task* t = dp_task_cur;
        dp_task_cur = NULL;
        fflush(stdout);
        dp_out_write(&t->out);
    }
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

/* Счётчик выделений (Р68): вызовы malloc и realloc и их байты — для
 * тестов и замеров кадровой памяти. Атомарный: библиотеку (`--lib`)
 * могут звать из нескольких потоков C. */
static _Atomic int64_t dp_alloc_count;
static _Atomic int64_t dp_alloc_bytes;

static void dp_count_alloc(int64_t bytes) {
    atomic_fetch_add_explicit(&dp_alloc_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&dp_alloc_bytes, bytes, memory_order_relaxed);
}

void dp_alloc_stats(int64_t* count, int64_t* bytes) {
    *count = atomic_load_explicit(&dp_alloc_count, memory_order_relaxed);
    *bytes = atomic_load_explicit(&dp_alloc_bytes, memory_order_relaxed);
}

void dp_alloc_report(void) {
    const char* on = getenv("DPL_ALLOC_STATS");
    if (!on || !*on) return;
    int64_t count, bytes;
    dp_alloc_stats(&count, &bytes);
    fprintf(stderr, "dpl: %" PRId64 " allocations, %" PRId64 " bytes\n", count, bytes);
}

void* dp_alloc(int64_t count, size_t elem, const char* loc) {
    if (count <= 0) return NULL;
    if (elem != 0 && count > DP_MAX_BYTES / (int64_t)elem) dp_trap(loc, "out of memory");
    void* p = malloc((size_t)count * elem);
    if (!p) dp_trap(loc, "out of memory");
    dp_count_alloc(count * (int64_t)elem);
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
    dp_count_alloc(count * (int64_t)elem);
    return q;
}

void dp_free(void* p) { free(p); }

/* --- служебная часть Pool (Р67) */

static void dp_heap_push(dp_pool* p, uint32_t x) {
    int64_t i = p->nheap++;
    while (i > 0 && p->heap[(i - 1) / 2] > x) {
        p->heap[i] = p->heap[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    p->heap[i] = x;
}

static uint32_t dp_heap_pop(dp_pool* p) {
    uint32_t top = p->heap[0], x = p->heap[--p->nheap];
    int64_t i = 0;
    for (;;) {
        int64_t c = 2 * i + 1;
        if (c >= p->nheap) break;
        if (c + 1 < p->nheap && p->heap[c + 1] < p->heap[c]) c++;
        if (p->heap[c] >= x) break;
        p->heap[i] = p->heap[c];
        i = c;
    }
    if (p->nheap > 0) p->heap[i] = x;
    return top;
}

void dp_pool_grow(dp_pool* p, int64_t n, const char* loc) {
    if (n <= p->cap) return;
    if (n > (int64_t)UINT32_MAX) dp_trap(loc, "out of memory"); /* номер слота — u32 */
    p->gens = (uint32_t*)dp_realloc(p->gens, n, sizeof(uint32_t), loc);
    p->live = (uint8_t*)dp_realloc(p->live, n, 1, loc);
    p->heap = (uint32_t*)dp_realloc(p->heap, n, sizeof(uint32_t), loc);
    p->cap = n;
}

int64_t dp_pool_take(dp_pool* p, const char* loc) {
    int64_t i;
    if (p->nheap > 0) {
        i = dp_heap_pop(p);
    } else {
        if (p->nslots == p->cap) {
            int64_t n = p->cap ? 2 * p->cap : 4;
            dp_pool_grow(p, n > (int64_t)UINT32_MAX ? p->cap + 1 : n, loc);
        }
        i = p->nslots++;
        p->gens[i] = 1;
    }
    p->live[i] = 1;
    p->len++;
    return i;
}

void dp_pool_release(dp_pool* p, int64_t i) {
    p->live[i] = 0;
    p->len--;
    if (++p->gens[i] < DP_POOL_GEN_LIMIT) dp_heap_push(p, (uint32_t)i);
}

void dp_pool_clear(dp_pool* p) {
    p->nheap = 0;
    for (int64_t i = 0; i < p->nslots; i++) {
        if (p->live[i]) {
            p->live[i] = 0;
            p->gens[i]++;
        }
        if (p->gens[i] < DP_POOL_GEN_LIMIT) p->heap[p->nheap++] = (uint32_t)i; /* по возрастанию — уже куча */
    }
    p->len = 0;
}

dp_pool dp_pool_copy(const dp_pool* p) {
    dp_pool r = *p;
    r.cap = p->nslots;
    r.gens = (uint32_t*)dp_alloc(r.cap, sizeof(uint32_t), NULL);
    r.live = (uint8_t*)dp_alloc(r.cap, 1, NULL);
    r.heap = (uint32_t*)dp_alloc(r.cap, sizeof(uint32_t), NULL);
    if (r.cap > 0) {
        memcpy(r.gens, p->gens, (size_t)r.cap * sizeof(uint32_t));
        memcpy(r.live, p->live, (size_t)r.cap);
    }
    if (p->nheap > 0) memcpy(r.heap, p->heap, (size_t)p->nheap * sizeof(uint32_t));
    return r;
}

void dp_pool_drop(dp_pool* p) {
    dp_free(p->gens);
    dp_free(p->live);
    dp_free(p->heap);
    *p = (dp_pool){0};
}

/* --- запись плавающего (Р48)
 *
 * Тот же алгоритм, что compiler/support/format.h: кратчайшая запись,
 * читаемая обратно в то же число (у f32 — в то же f32); обычная форма при
 * десятичном порядке от −4 до 15, без хвоста `.0`, иначе экспоненциальная.
 * Для каждой длины p проверяются правильно округлённая p-значная запись и
 * её соседи на единицу последнего разряда: у степеней двойки промежуток
 * округления несимметричен, и читаться обратно может только сосед. */

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

/* Десятичное d·10^k читается обратно в v (у f32 — strtof: без двойного
 * округления через double). */
static bool dp_reads_back(uint64_t d, int k, double v, bool f32) {
    char buf[48];
    snprintf(buf, sizeof buf, "%" PRIu64 "e%d", d, k);
    if (f32) return strtof(buf, NULL) == dp_to_f32(v);
    return strtod(buf, NULL) == v;
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
    bool neg = v < 0;
    double a = fabs(v);
    uint64_t d = 0;
    int k = 0;
    for (int p = 1; p <= 17; p++) {
        char buf[48];
        snprintf(buf, sizeof buf, "%.*e", p - 1, a);
        /* buf: d[.ddd]e±XX — p цифр и десятичный порядок первой. */
        d = 0;
        const char* c = buf;
        for (; *c != 'e'; c++) {
            if (*c != '.') d = d * 10 + (uint64_t)(*c - '0');
        }
        k = atoi(c + 1) - (p - 1);
        if (dp_reads_back(d, k, a, f32)) break;
        uint64_t low = 1;
        for (int i = 1; i < p; i++) low *= 10; /* у 1.00…0 сосед снизу — 9.99…9 порядком ниже */
        uint64_t below = d == low ? d * 10 - 1 : d - 1;
        int below_k = d == low ? k - 1 : k;
        if (dp_reads_back(below, below_k, a, f32)) {
            d = below;
            k = below_k;
            break;
        }
        if (dp_reads_back(d + 1, k, a, f32)) {
            d = d + 1;
            break;
        }
    }
    char digits[24];
    snprintf(digits, sizeof digits, "%" PRIu64, d);
    size_t nd = strlen(digits);
    while (nd > 1 && digits[nd - 1] == '0') {
        digits[--nd] = 0;
        k++;
    }
    int exp = k + (int)nd - 1; /* десятичный порядок первой цифры */
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

/* Вывод итерации `parallel for` — в буфер её части. Память — malloc, не
 * dp_alloc: счётчик выделений (Р68) считает только память программы. */
static void dp_par_put(int stream, const void* p, size_t n, bool nl) {
    dp_par_out* o = dp_par_cur ? dp_par_buf : &dp_task_cur->out;
    size_t need = o->len + 1 + sizeof(uint64_t) + n + (nl ? 1 : 0);
    if (need > o->cap) {
        size_t cap = o->cap ? o->cap : 256;
        while (cap < need) cap *= 2;
        char* b = (char*)realloc(o->buf, cap);
        if (!b) {
            fprintf(stderr, "trap: out of memory\n");
            _Exit(101);
        }
        o->buf = b;
        o->cap = cap;
    }
    uint64_t len = (uint64_t)n + (nl ? 1 : 0);
    o->buf[o->len++] = (char)stream;
    memcpy(o->buf + o->len, &len, sizeof len);
    o->len += sizeof len;
    if (n) memcpy(o->buf + o->len, p, n);
    o->len += n;
    if (nl) o->buf[o->len++] = '\n';
}

void dp_print(dp_str s) {
    if (dp_par_cur || dp_task_cur) {
        dp_par_put(1, s.ptr, (size_t)s.len, false);
        return;
    }
    if (s.len) fwrite(s.ptr, 1, (size_t)s.len, stdout);
}

void dp_println(dp_str s) {
    if (dp_par_cur || dp_task_cur) {
        dp_par_put(1, s.ptr, (size_t)s.len, true);
        return;
    }
    dp_print(s);
    fputc('\n', stdout);
}

void dp_eprintln(dp_str s) {
    if (dp_par_cur || dp_task_cur) {
        dp_par_put(2, s.ptr, (size_t)s.len, true);
        return;
    }
    fflush(stdout);
    if (s.len) fwrite(s.ptr, 1, (size_t)s.len, stderr);
    fputc('\n', stderr);
}

/* --- аргументы программы и файлы (Р51) */

static int dp_argc;
static char** dp_argv;

void dp_set_args(int argc, char** argv) {
    dp_argc = argc;
    dp_argv = argv;
}

int64_t dp_args_len(void) { return dp_argc > 1 ? dp_argc - 1 : 0; }

dp_str dp_arg(int64_t i, const char* loc) {
    dp_check_index(i, dp_args_len(), loc);
    const char* a = dp_argv[i + 1];
    return dp_str_own((const uint8_t*)a, (int64_t)strlen(a), loc);
}

bool dp_utf8_valid(const uint8_t* p, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        int len;
        uint32_t cp, min;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            len = 2, min = 0x80, cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3, min = 0x800, cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4, min = 0x10000, cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (int k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

char* dp_str_to_c(dp_str s, const char* loc) {
    if (s.len > 0 && memchr(s.ptr, 0, (size_t)s.len)) dp_trap(loc, "a str passed to C contains a zero byte");
    char* c = (char*)dp_alloc(s.len + 1, 1, loc);
    if (s.len) memcpy(c, s.ptr, (size_t)s.len);
    c[s.len] = 0;
    return c;
}

dp_str dp_str_from_c(const char* p, const char* fn, const char* loc) {
    if (!p) dp_trapf(loc, "C function '%s' returned NULL instead of a string", fn);
    size_t n = strlen(p);
    if (!dp_utf8_valid((const uint8_t*)p, (int64_t)n)) dp_trapf(loc, "C function '%s' returned a string that is not UTF-8", fn);
    dp_str s = {0};
    if (n == 0) return s;
    s.ptr = (uint8_t*)dp_alloc((int64_t)n, 1, loc);
    memcpy(s.ptr, p, n);
    s.len = (int64_t)n;
    return s;
}

static void** dp_carena_blocks;
static int64_t dp_carena_n, dp_carena_cap;

int64_t dp_carena_mark(void) { return dp_carena_n; }

void dp_carena_release(int64_t mark) {
    while (dp_carena_n > mark) dp_free(dp_carena_blocks[--dp_carena_n]);
}

static void dp_carena_keep(void* p, const char* loc) {
    if (dp_carena_n == dp_carena_cap) {
        int64_t cap = dp_carena_cap ? dp_carena_cap * 2 : 16;
        void** b = (void**)dp_alloc(cap, sizeof(void*), loc);
        if (dp_carena_n) memcpy(b, dp_carena_blocks, (size_t)dp_carena_n * sizeof(void*));
        dp_free(dp_carena_blocks);
        dp_carena_blocks = b;
        dp_carena_cap = cap;
    }
    dp_carena_blocks[dp_carena_n++] = p;
}

void* dp_carena_alloc(int64_t count, size_t elem, const char* loc) {
    if (count <= 0) return NULL;
    void* p = dp_alloc(count, elem, loc);
    dp_carena_keep(p, loc);
    return p;
}

const char* dp_carena_str(dp_str s, const char* loc) {
    char* c = dp_str_to_c(s, loc);
    dp_carena_keep(c, loc);
    return c;
}

dp_str dp_str_from_c_arg(const char* p, const char* param, const char* fn, const char* loc) {
    if (!p) dp_trapf(loc, "C passed NULL instead of a string as '%s' of '%s'", param, fn);
    size_t n = strlen(p);
    if (!dp_utf8_valid((const uint8_t*)p, (int64_t)n)) {
        dp_trapf(loc, "C passed a string that is not UTF-8 as '%s' of '%s'", param, fn);
    }
    dp_str s = {0};
    if (n == 0) return s;
    s.ptr = (uint8_t*)dp_alloc((int64_t)n, 1, loc);
    memcpy(s.ptr, p, n);
    s.len = (int64_t)n;
    return s;
}

int32_t dp_read_file(dp_str path, dp_str* out, const char* loc) {
    /* Путь — строка C: байт 0 внутри пути не называет никакого файла. */
    if (path.len > 0 && memchr(path.ptr, 0, (size_t)path.len)) return 1;
    char* name = (char*)dp_alloc(path.len + 1, 1, loc);
    if (path.len) memcpy(name, path.ptr, (size_t)path.len);
    name[path.len] = 0;
    FILE* f = dp_os_open_read(name);
    int err = errno;
    dp_free(name);
    if (!f) return err == ENOENT || err == ENOTDIR ? 1 : err == EACCES || err == EPERM ? 2 : 4;
    uint8_t* buf = NULL;
    int64_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            if (cap >= DP_MAX_BYTES) {
                fclose(f);
                dp_free(buf);
                dp_trap(loc, "out of memory");
            }
            cap = cap ? (cap > DP_MAX_BYTES / 2 ? DP_MAX_BYTES : cap * 2) : 65536;
            buf = (uint8_t*)dp_realloc(buf, cap, 1, loc);
        }
        size_t n = fread(buf + len, 1, (size_t)(cap - len), f);
        len += (int64_t)n;
        if (n == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad || !dp_utf8_valid(buf, len)) {
        dp_free(buf);
        return bad ? 4 : 3;
    }
    dp_str_drop(out);
    if (len == 0) {
        dp_free(buf);
        return 0;
    }
    out->ptr = (uint8_t*)dp_realloc(buf, len, 1, loc);
    out->len = len;
    return 0;
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

void dp_check_reserve(int64_t n, const char* loc) {
    if (n < 0) dp_trapf(loc, "reserve count %" PRId64 " is negative", n);
}

/* --- пул `parallel for` (Р69)
 *
 * Рабочие потоки запускаются при первом цикле и живут до конца программы.
 * Задание публикуется под мьютексом (dp_par_pending и номер поколения);
 * вызывающий поток работает наравне с рабочими, затем ждёт, пока все, кто
 * взял задание, выйдут из него. */
static void* dp_par_work_cv;
static void* dp_par_done_cv;
static int dp_par_workers = -1; /* -1 — пул ещё не запущен */
static dp_par_job* dp_par_pending;
static uint64_t dp_par_gen;
static int dp_par_active;
static int dp_par_waiting; /* поток ждёт рабочих: 1 (снимает со счёта последний рабочий) */

/* Части задания, пока они есть и не дальше ловушки. */
static void dp_par_run(dp_par_job* j) {
    int64_t depth = dp_depth;
    for (;;) {
        uint64_t start = atomic_fetch_add(&j->next, j->chunk);
        if (start >= j->n || start > atomic_load(&j->trap_at)) break;
        uint64_t end = j->n - start < j->chunk ? j->n : start + j->chunk;
        jmp_buf jb;
        dp_mtx* held = dp_mtx_held;
        dp_par_cur = j;
        dp_par_buf = &j->outs[start / j->chunk];
        dp_par_jb = &jb;
        if (setjmp(jb) == 0) {
            for (uint64_t k = start; k < end; k++) {
                if (k > atomic_load_explicit(&j->trap_at, memory_order_relaxed)) break;
                dp_par_iter = k;
                j->body(j->env, k);
            }
        } else {
            dp_depth = depth; /* кадры прерванной итерации брошены */
            /* Её мьютексы — тоже: иначе итерации, ждущие их, не кончатся. */
            while (dp_mtx_held != held) dp_mtx_unlock(dp_mtx_held);
        }
        dp_par_cur = NULL;
    }
}

static void dp_par_worker(void* arg) {
    (void)arg;
    uint64_t seen = 0;
    dp_sys_lock(dp_par_m);
    for (;;) {
        while (!dp_par_pending || dp_par_gen == seen) dp_sys_wait(dp_par_work_cv, dp_par_m);
        seen = dp_par_gen;
        dp_par_job* j = dp_par_pending;
        dp_par_active++;
        dp_dl_enter(); /* под dp_par_m: ждущий рабочих учтёт этот поток живым */
        dp_sys_unlock(dp_par_m);
        dp_par_run(j);
        dp_sys_lock(dp_par_m);
        if (--dp_par_active == 0) {
            dp_dl_wake(&dp_par_waiting);
            dp_sys_broadcast(dp_par_done_cv);
        }
        dp_dl_exit(); /* после побудки: ждущий рабочих — не в счёте ждущих */
    }
}

/* Число потоков: DPL_THREADS или число процессоров; не больше 64. */
static void dp_par_start(void) {
    int n = 0;
    const char* env = getenv("DPL_THREADS");
    if (env && *env) n = atoi(env);
    if (n <= 0) n = dp_sys_cpus();
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    dp_par_workers = 0;
    if (n == 1) return;
    dp_par_m = dp_sys_mutex();
    dp_par_work_cv = dp_sys_cond();
    dp_par_done_cv = dp_sys_cond();
    if (!dp_par_m || !dp_par_work_cv || !dp_par_done_cv) return;
    for (int i = 1; i < n; i++) {
        if (dp_sys_thread(dp_par_worker, NULL) != 0) break;
        dp_par_workers++;
    }
}

/* Записи буфера — в вывод потока: у задачи — в её буфер, иначе в stdout и
 * stderr. */
static void dp_out_emit(const dp_par_out* o) {
    for (size_t at = 0; at < o->len;) {
        int stream = o->buf[at++];
        uint64_t len;
        memcpy(&len, o->buf + at, sizeof len);
        at += sizeof len;
        if (dp_task_cur) {
            dp_par_put(stream, o->buf + at, (size_t)len, false);
        } else if (stream == 2) {
            fflush(stdout);
            fwrite(o->buf + at, 1, (size_t)len, stderr);
        } else {
            fwrite(o->buf + at, 1, (size_t)len, stdout);
        }
        at += (size_t)len;
    }
}

/* Записи буфера — прямо в stdout и stderr (ловушка в задаче). */
static void dp_out_write(const dp_par_out* o) {
    dp_task* t = dp_task_cur;
    dp_task_cur = NULL;
    dp_out_emit(o);
    dp_task_cur = t;
}

/* Вывод частей по порядку — до части за ловушкой. */
static void dp_par_flush(dp_par_job* j, uint64_t parts, uint64_t trap_at) {
    for (uint64_t c = 0; c < parts; c++) {
        dp_par_out* o = &j->outs[c];
        if (c * j->chunk <= trap_at) dp_out_emit(o);
        free(o->buf);
    }
}

void dp_parallel_for(uint64_t n, dp_par_body body, void* env) {
    if (n == 0) return;
    if (dp_par_workers < 0) dp_par_start();
    /* Вложенный цикл, цикл в задаче (Р70: пул — у main), пул из
     * одного потока, одна итерация — по очереди. */
    if (dp_par_cur || dp_task_cur || dp_par_workers == 0 || n == 1) {
        for (uint64_t k = 0; k < n; k++) body(env, k);
        return;
    }
    dp_par_job j;
    j.body = body;
    j.env = env;
    j.n = n;
    uint64_t threads = (uint64_t)dp_par_workers + 1;
    j.chunk = n / (threads * 8);
    if (j.chunk == 0) j.chunk = 1;
    uint64_t parts = n / j.chunk + (n % j.chunk != 0);
    atomic_init(&j.next, 0);
    atomic_init(&j.trap_at, UINT64_MAX);
    j.trap_loc[0] = 0;
    j.trap_msg[0] = 0;
    j.outs = (dp_par_out*)calloc((size_t)parts, sizeof(dp_par_out));
    if (!j.outs) dp_trap(NULL, "out of memory");
    fflush(stdout);
    dp_sys_lock(dp_par_m);
    dp_par_pending = &j;
    dp_par_gen++;
    dp_sys_broadcast(dp_par_work_cv);
    dp_sys_unlock(dp_par_m);
    dp_par_run(&j);
    dp_sys_lock(dp_par_m);
    dp_par_pending = NULL;
    while (dp_par_active > 0) {
        /* Место ожидания — у ждущих рабочих: своего у цикла нет. */
        if (!dp_par_waiting) dp_dl_block(&dp_par_waiting, NULL);
        dp_sys_wait(dp_par_done_cv, dp_par_m);
    }
    dp_sys_unlock(dp_par_m);
    uint64_t trap_at = atomic_load(&j.trap_at);
    dp_par_flush(&j, parts, trap_at);
    free(j.outs);
    if (trap_at != UINT64_MAX) dp_die(j.trap_loc, j.trap_msg);
}

/* --- Mutex[T] (Р70) */

static _Thread_local char dp_thread_id; /* адрес — имя потока */

dp_mtx dp_mtx_new(void) {
    dp_mtx m;
    memset(&m, 0, sizeof m);
    m.sys = dp_sys_mutex();
    m.cv = dp_sys_cond();
    if (!m.sys || !m.cv) dp_trap(NULL, "out of memory");
    return m;
}

static bool dp_mtx_try(dp_mtx* m) {
    void* none = NULL;
    return __atomic_compare_exchange_n(&m->owner, &none, (void*)&dp_thread_id, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_RELAXED);
}

/* Захват. Медленный путь: waiters растёт до проверки владельца, отпускающий
 * читает waiters после записи NULL (обе — последовательно согласованные),
 * поэтому побудка не теряется. Спит поток только под m->sys, отпускающий
 * будит под ним же — счёт ждущих точен. Короткое ожидание — без сна. */
void dp_mtx_lock(dp_mtx* m, const char* loc) {
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == &dp_thread_id) {
        dp_trap(loc, "mutex is already locked by this thread");
    }
    if (!dp_mtx_try(m)) {
        bool got = false;
        for (int spin = 0; spin < 64 && !got; spin++) {
            if (!__atomic_load_n(&m->owner, __ATOMIC_RELAXED)) got = dp_mtx_try(m);
        }
        if (!got) {
            dp_sys_lock(m->sys);
            __atomic_fetch_add(&m->waiters, 1, __ATOMIC_SEQ_CST);
            while (!dp_mtx_try(m)) {
                dp_dl_block(&m->waiting, loc);
                do dp_sys_wait(m->cv, m->sys);
                while (m->wakes == 0);
                m->wakes--; /* со счёта снял отпускающий */
            }
            __atomic_fetch_sub(&m->waiters, 1, __ATOMIC_SEQ_CST);
            dp_sys_unlock(m->sys);
        }
    }
    m->held_up = dp_mtx_held;
    dp_mtx_held = m;
}

/* Отпускание. Есть спящие — будит одного и снимает его со счёта; не
 * получивший захват засыпает снова — снова в счёте. */
void dp_mtx_unlock(dp_mtx* m) {
    dp_mtx** p = &dp_mtx_held; /* блоки `lock` вложены: m — обычно вершина */
    while (*p && *p != m) p = &(*p)->held_up;
    if (*p) *p = m->held_up;
    __atomic_store_n(&m->owner, NULL, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m->waiters, __ATOMIC_SEQ_CST) == 0) return;
    bool wake = false;
    dp_sys_lock(m->sys);
    if (m->waiting > 0) {
        m->waiting--;
        m->wakes++;
        dp_dl_unblock(1);
        wake = true;
    }
    dp_sys_unlock(m->sys);
    /* Сигнал — после отпускания: разбуженный не упирается в m->sys. */
    if (wake) dp_sys_signal(m->cv);
}

void dp_mtx_drop(dp_mtx* m) {
    if (m->sys) dp_sys_mutex_free(m->sys);
    if (m->cv) dp_sys_cond_free(m->cv);
    m->sys = NULL;
    m->cv = NULL;
}

/* --- Channel[T] (Р70) */

struct dp_chan {
    void* m;
    void* not_empty;
    void* not_full;
    int64_t cap, head, len;
    bool closed;
    int recv_waiting, send_waiting; /* ждут сейчас (их снимает со счёта будящий) */
    uint64_t recv_gen, send_gen;    /* номер побудки: ложная побудка его не меняет */
};

/* Взаимная блокировка: живые потоки dpl и ждущие (канал, мьютекс, конец
 * блока задач, рабочих пула) — под dp_dl_m.
 * Будящий снимает ждущих со счёта сам, поэтому «все ждут» не бывает
 * ложным, пока разбуженный ещё не проснулся. */
static void* dp_dl_m;
static int dp_dl_live = 1; /* поток main */
static int dp_dl_blocked;
static const char* dp_dl_loc; /* место последнего ждущего */

/* Мьютекс, созданный при первом обращении (из любого потока). */
static void* dp_lazy_mutex(void** slot) {
    void* m = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (m) return m;
    void* fresh = dp_sys_mutex();
    if (!fresh) {
        fprintf(stderr, "trap: out of memory\n");
        _Exit(101);
    }
    void* expected = NULL;
    if (!__atomic_compare_exchange_n(slot, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        dp_sys_mutex_free(fresh);
        return expected;
    }
    return fresh;
}

static void* dp_dl_mutex(void) { return dp_lazy_mutex(&dp_dl_m); }

/* Ловушка без longjmp итерации `parallel for`: взаимная блокировка — у всей
 * программы, захваченные мьютексы не отпускаются. */
static _Noreturn void dp_die_now(const char* loc, const char* msg) {
    fflush(stdout);
    if (loc && *loc) fprintf(stderr, "%s: trap: %s\n", loc, msg);
    else fprintf(stderr, "trap: %s\n", msg);
    fflush(stderr);
    _Exit(101);
}

/* Поток засыпает в ожидании (мьютекс ожидаемого захвачен). Место NULL
 * оставляет прежнее — место другого ждущего. */
static void dp_dl_block(int* waiting, const char* loc) {
    void* m = dp_dl_mutex();
    dp_sys_lock(m);
    dp_dl_blocked++;
    (*waiting)++;
    if (loc) dp_dl_loc = loc;
    if (dp_dl_blocked >= dp_dl_live) dp_die_now(dp_dl_loc, "deadlock: every thread is waiting");
    dp_sys_unlock(m);
}

/* Поток dpl начинает работу: задача или рабочий пула в задании. */
static void dp_dl_enter(void) {
    void* m = dp_dl_mutex();
    dp_sys_lock(m);
    dp_dl_live++;
    dp_sys_unlock(m);
}

/* Поток кончил работу: если остальные ждут, будить их некому. */
static void dp_dl_exit(void) {
    void* m = dp_dl_mutex();
    dp_sys_lock(m);
    dp_dl_live--;
    if (dp_dl_live > 0 && dp_dl_blocked >= dp_dl_live) dp_die_now(dp_dl_loc, "deadlock: every thread is waiting");
    dp_sys_unlock(m);
}

/* n ждущих разбужены — они больше не в счёте. */
static void dp_dl_unblock(int n) {
    void* m = dp_dl_mutex();
    dp_sys_lock(m);
    dp_dl_blocked -= n;
    dp_sys_unlock(m);
}

/* Будит ждущих (мьютекс ожидаемого захвачен): они больше не в счёте. */
static void dp_dl_wake(int* waiting) {
    if (*waiting == 0) return;
    dp_dl_unblock(*waiting);
    *waiting = 0;
}

dp_chan* dp_chan_new(int64_t cap, const char* loc) {
    if (cap < 1) dp_trapf(loc, "channel capacity %" PRId64 " is less than 1", cap);
    dp_chan* c = (dp_chan*)calloc(1, sizeof(dp_chan));
    if (!c) dp_trap(loc, "out of memory");
    c->m = dp_sys_mutex();
    c->not_empty = dp_sys_cond();
    c->not_full = dp_sys_cond();
    if (!c->m || !c->not_empty || !c->not_full) dp_trap(loc, "out of memory");
    c->cap = cap;
    return c;
}

int64_t dp_chan_send_begin(dp_chan* c, const char* loc) {
    dp_sys_lock(c->m);
    while (c->len == c->cap && !c->closed) {
        uint64_t gen = c->send_gen;
        dp_dl_block(&c->send_waiting, loc);
        do dp_sys_wait(c->not_full, c->m);
        while (c->send_gen == gen);
    }
    if (c->closed) {
        dp_sys_unlock(c->m);
        dp_trap(loc, "send on a closed channel");
    }
    return (c->head + c->len) % c->cap;
}

void dp_chan_send_end(dp_chan* c) {
    c->len++;
    c->recv_gen++;
    dp_dl_wake(&c->recv_waiting);
    dp_sys_broadcast(c->not_empty);
    dp_sys_unlock(c->m);
}

bool dp_chan_recv_begin(dp_chan* c, bool wait, int64_t* slot, const char* loc) {
    dp_sys_lock(c->m);
    while (wait && c->len == 0 && !c->closed) {
        uint64_t gen = c->recv_gen;
        dp_dl_block(&c->recv_waiting, loc);
        do dp_sys_wait(c->not_empty, c->m);
        while (c->recv_gen == gen);
    }
    if (c->len == 0) {
        dp_sys_unlock(c->m);
        return false;
    }
    *slot = c->head;
    return true;
}

void dp_chan_recv_end(dp_chan* c) {
    c->head = (c->head + 1) % c->cap;
    c->len--;
    c->send_gen++;
    dp_dl_wake(&c->send_waiting);
    dp_sys_broadcast(c->not_full);
    dp_sys_unlock(c->m);
}

void dp_chan_close(dp_chan* c, const char* loc) {
    dp_sys_lock(c->m);
    if (c->closed) {
        dp_sys_unlock(c->m);
        dp_trap(loc, "close of a closed channel");
    }
    c->closed = true;
    c->recv_gen++;
    c->send_gen++;
    dp_dl_wake(&c->recv_waiting);
    dp_dl_wake(&c->send_waiting);
    dp_sys_broadcast(c->not_empty);
    dp_sys_broadcast(c->not_full);
    dp_sys_unlock(c->m);
}

int64_t dp_chan_cap(const dp_chan* c) { return c->cap; }
int64_t dp_chan_head(const dp_chan* c) { return c->head; }
int64_t dp_chan_len(const dp_chan* c) { return c->len; }

void dp_chan_free(dp_chan* c) {
    if (!c) return;
    dp_sys_mutex_free(c->m);
    dp_sys_cond_free(c->not_empty);
    dp_sys_cond_free(c->not_full);
    free(c);
}


/* --- задачи блока `tasks` (Р70)
 *
 * Задача — свой поток ОС; группа — задачи блока по порядку запуска, стек
 * групп — у потока. Конец блока ждёт задачи (в счёте взаимной блокировки)
 * и выводит их буферы по порядку запуска — в вывод потока. */
typedef struct dp_group {
    struct dp_group* up;
    dp_task* first;
    dp_task* last;
    void* m;
    void* cv;
    int pending;
    int waiting; /* блок ждёт: 1 (снимает со счёта последняя задача) */
} dp_group;

static _Thread_local dp_group* dp_group_top;

void dp_tasks_begin(void) {
    dp_group* g = (dp_group*)calloc(1, sizeof(dp_group));
    if (!g) dp_trap(NULL, "out of memory");
    g->m = dp_sys_mutex();
    g->cv = dp_sys_cond();
    if (!g->m || !g->cv) dp_trap(NULL, "out of memory");
    g->up = dp_group_top;
    dp_group_top = g;
}

static void dp_task_main(void* arg) {
    dp_task* t = (dp_task*)arg;
    dp_group* g = t->group;
    dp_task_cur = t;
    t->body(t->env);
    dp_task_cur = NULL;
    dp_sys_lock(g->m);
    if (--g->pending == 0) {
        dp_dl_wake(&g->waiting);
        dp_sys_broadcast(g->cv);
    }
    dp_sys_unlock(g->m);
    dp_dl_exit();
}

void dp_spawn(void (*body)(void*), const void* env, size_t size, const char* loc) {
    dp_group* g = dp_group_top;
    if (!g) dp_unreachable("spawn outside a block of tasks");
    dp_task* t = (dp_task*)calloc(1, sizeof(dp_task));
    void* e = malloc(size ? size : 1);
    if (!t || !e) dp_trap(loc, "out of memory");
    memcpy(e, env, size);
    t->body = body;
    t->env = e;
    t->group = g;
    if (g->last) g->last->next = t;
    else g->first = t;
    g->last = t;
    dp_sys_lock(g->m);
    g->pending++;
    dp_sys_unlock(g->m);
    dp_dl_enter(); /* до запуска: блок не сочтёт себя последним живым */
    if (dp_sys_thread(dp_task_main, t) != 0) dp_trap(loc, "cannot start a thread for a task");
}

void dp_tasks_end(const char* loc) {
    dp_group* g = dp_group_top;
    if (!g) dp_unreachable("end of a block of tasks without its beginning");
    dp_sys_lock(g->m);
    while (g->pending > 0) {
        if (!g->waiting) dp_dl_block(&g->waiting, loc);
        dp_sys_wait(g->cv, g->m);
    }
    dp_sys_unlock(g->m);
    dp_group_top = g->up;
    for (dp_task* t = g->first; t;) {
        dp_task* next = t->next;
        dp_out_emit(&t->out);
        free(t->out.buf);
        free(t->env);
        free(t);
        t = next;
    }
    dp_sys_mutex_free(g->m);
    dp_sys_cond_free(g->cv);
    free(g);
}
