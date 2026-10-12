/* Runtime dpl: ядро (docs/runtime/README.md).
 *
 * Контракт с C-бэкендом (compiler/backend_c/): сгенерированный C
 * подключает этот заголовок и линкуется с dp_rt.c. Всё здесь — C11 с
 * определённым поведением: переполнение целых проверяется до операции,
 * обёрточная арифметика — беззнаковая, переход беззнакового в знаковое —
 * арифметикой, а не преобразованием вне диапазона.
 *
 * Ловушка (Р36): «файл:строка:столбец: trap: сообщение» в stderr, код
 * выхода 101; напечатанное до неё сохраняется, defer не выполняются.
 */
#ifndef DP_RT_H
#define DP_RT_H

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Плавающие (trap.float, docs/policy/DETERMINISM.md): каждая операция
 * округляется до своего типа. Промежуточная точность сверх типа (x87)
 * дала бы другие биты — такая цель не поддерживается. */
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
#error "dpl runtime requires FLT_EVAL_METHOD == 0"
#endif

/* Вспомогательные функции сгенерированного кода: на тип и на список их
 * много, и не все нужны программе — без предупреждений о неиспользуемых.
 * DP_FN — функция программы: dpl не запрещает функцию без вызовов. */
#if defined(__GNUC__) || defined(__clang__)
#define DP_INLINE static inline __attribute__((unused))
#define DP_FN static __attribute__((unused))
#else
#define DP_INLINE static inline
#define DP_FN static
#endif

/* --- ловушки и память */

_Noreturn void dp_trap(const char* loc, const char* msg);
_Noreturn void dp_trapf(const char* loc, const char* fmt, ...);
/* Внутренняя ошибка: исполнение пришло туда, куда по проверке программы
 * прийти не может. Не ловушка программы — код выхода 3. */
_Noreturn void dp_unreachable(const char* where);

/* Предел одного буфера List или str (trap.memory). */
#define DP_MAX_BYTES (INT64_C(1) << 31)

/* Буфер count элементов по elem байт; больше предела или нет памяти —
 * ловушка «out of memory». count = 0 — NULL. */
void* dp_alloc(int64_t count, size_t elem, const char* loc);
void* dp_realloc(void* p, int64_t count, size_t elem, const char* loc);
void dp_free(void* p);

/* Глубина вложенности вызовов (trap.memory): число активных кадров;
 * функция увеличивает её на входе и уменьшает на выходе, вызов
 * проверяет предел до входа в вызываемую. */
#define DP_MAX_DEPTH 32768
extern int64_t dp_depth;
#define DP_CALL_CHECK(loc) \
    do { \
        if (dp_depth >= DP_MAX_DEPTH) dp_trap((loc), "stack overflow: calls are nested too deeply"); \
    } while (0)

/* --- целые: проверенная арифметика (trap.int)
 *
 * Знаковые типы считаются в int64_t, беззнаковые — в uint64_t; bits —
 * ширина типа dpl, tname — его имя в сообщении ('i32', 'int').
 * Результат лежит в диапазоне типа, и сгенерированный код приводит его
 * к своему типу без потерь. */

static inline int64_t dp_smin(int bits) { return bits >= 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1)); }
static inline int64_t dp_smax(int bits) { return bits >= 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1; }
static inline uint64_t dp_umax(int bits) { return bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1; }

/* Биты в знаковое значение ширины bits: дополнительный код, без
 * преобразования вне диапазона. */
static inline int64_t dp_sbits(uint64_t v, int bits) {
    if (bits < 64) {
        v &= dp_umax(bits);
        if (v >> (bits - 1)) return (int64_t)(v - (UINT64_C(1) << (bits - 1))) + dp_smin(bits);
        return (int64_t)v;
    }
    if (v <= (uint64_t)INT64_MAX) return (int64_t)v;
    return (int64_t)(v - (uint64_t)INT64_MAX - 1) + INT64_MIN;
}
static inline uint64_t dp_ubits(uint64_t v, int bits) { return v & dp_umax(bits); }

_Noreturn void dp_overflow(const char* op, const char* tname, const char* loc);

static inline int64_t dp_sadd(int64_t a, int64_t b, int bits, const char* t, const char* loc) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) dp_overflow("+", t, loc);
    int64_t r = a + b;
    if (r < dp_smin(bits) || r > dp_smax(bits)) dp_overflow("+", t, loc);
    return r;
}
static inline int64_t dp_ssub(int64_t a, int64_t b, int bits, const char* t, const char* loc) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) dp_overflow("-", t, loc);
    int64_t r = a - b;
    if (r < dp_smin(bits) || r > dp_smax(bits)) dp_overflow("-", t, loc);
    return r;
}
static inline int64_t dp_smul(int64_t a, int64_t b, int bits, const char* t, const char* loc) {
    bool o;
    if (a > 0) o = b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
    else o = b > 0 ? a < INT64_MIN / b : (a != 0 && b < INT64_MAX / a);
    if (o) dp_overflow("*", t, loc);
    int64_t r = a * b;
    if (r < dp_smin(bits) || r > dp_smax(bits)) dp_overflow("*", t, loc);
    return r;
}
static inline int64_t dp_sdiv(int64_t a, int64_t b, int bits, const char* t, const char* loc) {
    if (b == 0) dp_trap(loc, "division by zero");
    if (a == dp_smin(bits) && b == -1) dp_overflow("/", t, loc);
    return a / b;
}
static inline int64_t dp_srem(int64_t a, int64_t b, int bits, const char* t, const char* loc) {
    (void)t;
    if (b == 0) dp_trap(loc, "division by zero");
    if (a == dp_smin(bits) && b == -1) return 0;
    return a % b;
}
static inline int64_t dp_sneg(int64_t a, int bits, const char* t, const char* loc) {
    if (a == dp_smin(bits)) dp_overflow("-", t, loc);
    return -a;
}
static inline int64_t dp_sabs(int64_t a, int bits, const char* t, const char* loc) {
    if (a == dp_smin(bits)) dp_overflow("abs", t, loc);
    return a < 0 ? -a : a;
}

static inline uint64_t dp_uadd(uint64_t a, uint64_t b, int bits, const char* t, const char* loc) {
    uint64_t r = a + b;
    if (r < a || r > dp_umax(bits)) dp_overflow("+", t, loc);
    return r;
}
static inline uint64_t dp_usub(uint64_t a, uint64_t b, int bits, const char* t, const char* loc) {
    (void)bits;
    if (b > a) dp_overflow("-", t, loc);
    return a - b;
}
static inline uint64_t dp_umul(uint64_t a, uint64_t b, int bits, const char* t, const char* loc) {
    if (a != 0 && b > UINT64_MAX / a) dp_overflow("*", t, loc);
    uint64_t r = a * b;
    if (r > dp_umax(bits)) dp_overflow("*", t, loc);
    return r;
}
static inline uint64_t dp_udiv(uint64_t a, uint64_t b, const char* loc) {
    if (b == 0) dp_trap(loc, "division by zero");
    return a / b;
}
static inline uint64_t dp_urem(uint64_t a, uint64_t b, const char* loc) {
    if (b == 0) dp_trap(loc, "division by zero");
    return a % b;
}

/* Проверяемая арифметика (Р53): true и *r — точный результат в типе
 * ширины bits; false — вне типа или делитель 0. Ловушки нет. */
static inline bool dp_try_sadd(int64_t a, int64_t b, int bits, int64_t* r) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    int64_t x = a + b;
    if (x < dp_smin(bits) || x > dp_smax(bits)) return false;
    *r = x;
    return true;
}
static inline bool dp_try_ssub(int64_t a, int64_t b, int bits, int64_t* r) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;
    int64_t x = a - b;
    if (x < dp_smin(bits) || x > dp_smax(bits)) return false;
    *r = x;
    return true;
}
static inline bool dp_try_smul(int64_t a, int64_t b, int bits, int64_t* r) {
    bool o;
    if (a > 0) o = b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
    else o = b > 0 ? a < INT64_MIN / b : (a != 0 && b < INT64_MAX / a);
    if (o) return false;
    int64_t x = a * b;
    if (x < dp_smin(bits) || x > dp_smax(bits)) return false;
    *r = x;
    return true;
}
static inline bool dp_try_sdiv(int64_t a, int64_t b, int bits, int64_t* r) {
    if (b == 0 || (a == dp_smin(bits) && b == -1)) return false;
    *r = a / b;
    return true;
}
static inline bool dp_try_sneg(int64_t a, int bits, int64_t* r) {
    if (a == dp_smin(bits)) return false;
    *r = -a;
    return true;
}
static inline bool dp_try_uadd(uint64_t a, uint64_t b, int bits, uint64_t* r) {
    uint64_t x = a + b;
    if (x < a || x > dp_umax(bits)) return false;
    *r = x;
    return true;
}
static inline bool dp_try_usub(uint64_t a, uint64_t b, int bits, uint64_t* r) {
    (void)bits;
    if (b > a) return false;
    *r = a - b;
    return true;
}
static inline bool dp_try_umul(uint64_t a, uint64_t b, int bits, uint64_t* r) {
    if (a != 0 && b > UINT64_MAX / a) return false;
    uint64_t x = a * b;
    if (x > dp_umax(bits)) return false;
    *r = x;
    return true;
}
static inline bool dp_try_udiv(uint64_t a, uint64_t b, int bits, uint64_t* r) {
    (void)bits;
    if (b == 0) return false;
    *r = a / b;
    return true;
}

/* Сдвиги: счётчик — значение правого операнда (знаковое или нет). */
_Noreturn void dp_bad_shift_s(int64_t n, const char* tname, const char* loc);
_Noreturn void dp_bad_shift_u(uint64_t n, const char* tname, const char* loc);
static inline uint64_t dp_shift_count_s(int64_t n, int bits, const char* t, const char* loc) {
    if (n < 0 || n >= bits) dp_bad_shift_s(n, t, loc);
    return (uint64_t)n;
}
static inline uint64_t dp_shift_count_u(uint64_t n, int bits, const char* t, const char* loc) {
    if (n >= (uint64_t)bits) dp_bad_shift_u(n, t, loc);
    return n;
}
static inline int64_t dp_sshl(int64_t a, uint64_t n, int bits) { return dp_sbits((uint64_t)a << n, bits); }
static inline int64_t dp_sshr(int64_t a, uint64_t n) { return a >= 0 ? a >> n : ~(~a >> n); }
static inline uint64_t dp_ushl(uint64_t a, uint64_t n, int bits) { return dp_ubits(a << n, bits); }

/* Преобразование T(x) между целыми и из плавающего (trap.convert). */
int64_t dp_cast_s_to_s(int64_t v, int bits, const char* to, const char* loc);
uint64_t dp_cast_s_to_u(int64_t v, int bits, const char* to, const char* loc);
int64_t dp_cast_u_to_s(uint64_t v, int bits, const char* to, const char* loc);
uint64_t dp_cast_u_to_u(uint64_t v, int bits, const char* to, const char* loc);
int64_t dp_cast_f_to_s(double x, bool from_f32, int bits, const char* to, const char* loc);
uint64_t dp_cast_f_to_u(double x, bool from_f32, int bits, const char* to, const char* loc);
float dp_cast_f64_to_f32(double x, const char* loc);

/* --- строки (str): UTF-8, владеют памятью; пустая — {NULL, 0} */

typedef struct {
    uint8_t* ptr;
    int64_t len;
} dp_str;

#define DP_STR_LIT(data, n) ((dp_str){(uint8_t*)(data), (n)})

dp_str dp_str_own(const uint8_t* p, int64_t n, const char* loc); /* копия байтов */
static inline dp_str dp_str_copy(const dp_str* s) { return dp_str_own(s->ptr, s->len, NULL); }
static inline void dp_str_drop(dp_str* s) {
    dp_free(s->ptr);
    s->ptr = NULL;
    s->len = 0;
}
dp_str dp_str_concat(dp_str a, dp_str b, const char* loc);
bool dp_str_eq(dp_str a, dp_str b);
bool dp_str_lt(dp_str a, dp_str b); /* по байтам без знака */

/* Хэш ключа Map (Р54): без случайной соли. Обход Map — по возрастанию
 * ключа, от хэша он не зависит; хэш нужен только для поиска. */
static inline uint64_t dp_hash_u64(uint64_t x) {
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}
static inline uint64_t dp_hash_str(dp_str s) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (int64_t i = 0; i < s.len; i++) {
        h ^= s.ptr[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}
uint8_t dp_str_at(dp_str s, int64_t i, const char* loc);
dp_str dp_str_slice(dp_str s, int64_t lo, int64_t hi, const char* loc);
dp_str dp_str_sub(dp_str s, int64_t lo, int64_t hi); /* без проверок: границы уже верны */

dp_str dp_str_from_s(int64_t v);
dp_str dp_str_from_u(uint64_t v);
dp_str dp_str_from_bool(bool v);
dp_str dp_str_from_f(double v, bool f32); /* Р48 */

bool dp_parse_int(dp_str s, int64_t* out);
dp_str dp_str_trim(dp_str s);
bool dp_str_find_from(dp_str s, dp_str t, int64_t from, int64_t* pos);
bool dp_str_starts_with(dp_str s, dp_str t);

static inline bool dp_is_space(uint64_t b) { return b == ' ' || b == '\t' || b == '\n' || b == '\r'; }
static inline bool dp_is_digit(uint64_t b) { return b >= '0' && b <= '9'; }
static inline bool dp_is_alpha(uint64_t b) { return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z'); }
static inline bool dp_is_alnum(uint64_t b) { return dp_is_alpha(b) || dp_is_digit(b); }

void dp_print(dp_str s);
void dp_println(dp_str s);
void dp_eprintln(dp_str s);

/* --- аргументы программы и файлы (Р51, stdlib/os.dpm) */

/* Запоминает argv программы; argv[0] — её имя, в os.args не входит. */
void dp_set_args(int argc, char** argv);
int64_t dp_args_len(void);
dp_str dp_arg(int64_t i, const char* loc);
/* Файл целиком в *out (прежнее значение освобождается): 0 — прочитан;
 * иначе 1 + вариант os.IoError — 1 NotFound, 2 PermissionDenied,
 * 3 InvalidUtf8, 4 Other — и *out не меняется. Файл больше предела
 * буфера — ловушка «out of memory». */
int32_t dp_read_file(dp_str path, dp_str* out, const char* loc);
/* Канонический NaN класса D (Р56): положительный тихий, без полезной
 * нагрузки; остальные значения — как есть. */
static inline double dp_canon_f64(double x) {
    union { uint64_t u; double d; } c = {UINT64_C(0x7ff8000000000000)};
    return x != x ? c.d : x;
}
static inline float dp_canon_f32(float x) {
    union { uint32_t u; float f; } c = {UINT32_C(0x7fc00000)};
    return x != x ? c.f : x;
}

/* --- строки на границе C (Р59) */

/* Копия s с нулём в конце для функции C; освобождается dp_free после
 * вызова. Байт 0 внутри s — ловушка: C увидел бы только начало строки. */
char* dp_str_to_c(dp_str s, const char* loc);

/* Арена C на время вызова (Р62): строки и массивы заимствующих структур.
 * Отметка — число блоков; освобождение — всех блоков после отметки
 * (вызовы вложены, поэтому порядок — стековый). */
int64_t dp_carena_mark(void);
void dp_carena_release(int64_t mark);
void* dp_carena_alloc(int64_t count, size_t elem, const char* loc);
const char* dp_carena_str(dp_str s, const char* loc);
/* Строка C из функции fn — копия в str. NULL и не UTF-8 — ловушка. */
dp_str dp_str_from_c(const char* p, const char* fn, const char* loc);
/* Строка, которую C передал функции `@export` (Р61): копия; NULL и не
 * UTF-8 — ловушка с именами параметра и функции. */
dp_str dp_str_from_c_arg(const char* p, const char* param, const char* fn, const char* loc);

/* Открывает файл для чтения байтов по пути в UTF-8 (порт платформы:
 * runtime/linux, runtime/windows). NULL — нет; errno — причина. */
FILE* dp_os_open_read(const char* path);
/* Байты — корректный UTF-8: без обрезанных, избыточных записей,
 * суррогатов и символов больше U+10FFFF. */
bool dp_utf8_valid(const uint8_t* p, int64_t n);

/* --- индексы и срезы (trap.index) */

int64_t dp_check_index(int64_t i, int64_t len, const char* loc);
void dp_check_slice(int64_t lo, int64_t hi, int64_t len, const char* loc);
void dp_check_insert(int64_t i, int64_t len, const char* loc);
void dp_check_repeat(int64_t n, const char* loc);

/* --- вход в программу (trap.exit)
 *
 * Сгенерированный main вызывает dp_run: entry исполняется в потоке с
 * большим стеком; результат — код выхода. error_name — запись «E.V»
 * ошибки main или NULL. */
typedef const char* (*dp_entry_fn)(void);
int dp_run(dp_entry_fn entry);
/* То же для теста (Р50): ошибка — «error: test failed with E.V». */
int dp_run_test(dp_entry_fn entry);

#endif
