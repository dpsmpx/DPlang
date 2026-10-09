/* spec: trap.def trap.int trap.float trap.convert trap.index trap.builtin trap.memory trap.exit
 *
 * Тесты runtime по функциям (docs/runtime/README.md, раздел 8): граничные
 * значения проверенной арифметики, преобразований, строк, индексов и
 * памяти. Собирается с runtime и санитайзерами из tests/runtime.py.
 *
 *     rt_test unit        — проверки без ловушек; «ok N» и код 0
 *     rt_test trap ИМЯ    — одна ловушка: текст и код сверяет tests/runtime.py
 *     rt_test run ИМЯ     — вход dp_run: код выхода, ошибка main, глубина
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dp_rt.h"

static int checks, failures;

#define CHECK(c) \
    do { \
        checks++; \
        if (!(c)) { \
            failures++; \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        } \
    } while (0)

#define LOC "t.dpm:3:7"

static dp_str S(const char* c) { return DP_STR_LIT(c, (int64_t)strlen(c)); }

/* s равна c; s освобождается. */
static bool take_is(dp_str s, const char* c) {
    bool ok = dp_str_eq(s, S(c));
    if (!ok) fprintf(stderr, "  got \"%.*s\", want \"%s\"\n", (int)s.len, s.ptr ? (const char*)s.ptr : "", c);
    dp_str_drop(&s);
    return ok;
}

static bool float_is(double v, bool f32, const char* c) { return take_is(dp_str_from_f(v, f32), c); }

static void unit_int(void) {
    CHECK(dp_smin(8) == -128 && dp_smax(8) == 127 && dp_umax(8) == 255);
    CHECK(dp_smin(64) == INT64_MIN && dp_smax(64) == INT64_MAX && dp_umax(64) == UINT64_MAX);
    CHECK(dp_smin(32) == INT32_MIN && dp_umax(16) == 65535);
    CHECK(dp_sbits(0xFF, 8) == -1 && dp_sbits(0x80, 8) == -128 && dp_sbits(0x7F, 8) == 127);
    CHECK(dp_sbits(0x1FF, 8) == -1 && dp_sbits(UINT64_MAX, 64) == -1);
    CHECK(dp_sbits(UINT64_C(1) << 63, 64) == INT64_MIN && dp_sbits((uint64_t)INT64_MAX, 64) == INT64_MAX);
    CHECK(dp_ubits(0x1FF, 8) == 0xFF && dp_ubits(UINT64_MAX, 64) == UINT64_MAX);

    CHECK(dp_sadd(INT64_MAX - 1, 1, 64, "i64", LOC) == INT64_MAX);
    CHECK(dp_sadd(INT64_MIN, INT64_MAX, 64, "i64", LOC) == -1);
    CHECK(dp_sadd(126, 1, 8, "i8", LOC) == 127 && dp_sadd(-127, -1, 8, "i8", LOC) == -128);
    CHECK(dp_ssub(INT64_MIN + 1, 1, 64, "i64", LOC) == INT64_MIN);
    CHECK(dp_ssub(-1, INT64_MAX, 64, "i64", LOC) == INT64_MIN && dp_ssub(0, -127, 8, "i8", LOC) == 127);
    CHECK(dp_smul(-1, INT64_MAX, 64, "i64", LOC) == -INT64_MAX && dp_smul(INT64_MIN, 1, 64, "i64", LOC) == INT64_MIN);
    CHECK(dp_smul(0, INT64_MIN, 64, "i64", LOC) == 0 && dp_smul(INT64_MIN, 0, 64, "i64", LOC) == 0);
    CHECK(dp_smul(-1, -1, 64, "i64", LOC) == 1 && dp_smul(-64, 2, 8, "i8", LOC) == -128);
    CHECK(dp_smul(46340, 46340, 32, "i32", LOC) == 2147395600);
    CHECK(dp_sdiv(-7, 2, 64, "i64", LOC) == -3 && dp_sdiv(7, -2, 64, "i64", LOC) == -3);
    CHECK(dp_sdiv(-127, -1, 8, "i8", LOC) == 127 && dp_sdiv(INT64_MIN, 1, 64, "i64", LOC) == INT64_MIN);
    CHECK(dp_srem(-7, 2, 64, "i64", LOC) == -1 && dp_srem(7, -2, 64, "i64", LOC) == 1);
    CHECK(dp_srem(INT64_MIN, -1, 64, "i64", LOC) == 0 && dp_srem(-128, -1, 8, "i8", LOC) == 0);
    CHECK(dp_sneg(-127, 8, "i8", LOC) == 127 && dp_sneg(INT64_MAX, 64, "i64", LOC) == -INT64_MAX);
    CHECK(dp_sabs(-127, 8, "i8", LOC) == 127 && dp_sabs(INT64_MIN + 1, 64, "i64", LOC) == INT64_MAX);
    CHECK(dp_sabs(5, 8, "i8", LOC) == 5 && dp_sabs(0, 64, "i64", LOC) == 0);

    CHECK(dp_uadd(254, 1, 8, "u8", LOC) == 255 && dp_uadd(UINT64_MAX - 1, 1, 64, "u64", LOC) == UINT64_MAX);
    CHECK(dp_usub(0, 0, 8, "u8", LOC) == 0 && dp_usub(UINT64_MAX, UINT64_MAX, 64, "u64", LOC) == 0);
    CHECK(dp_umul(UINT32_MAX, 1, 32, "u32", LOC) == UINT32_MAX);
    CHECK(dp_umul(UINT64_C(1) << 32, UINT64_C(1) << 31, 64, "u64", LOC) == UINT64_C(1) << 63);
    CHECK(dp_umul(0, UINT64_MAX, 64, "u64", LOC) == 0 && dp_umul(15, 17, 8, "u8", LOC) == 255);
    CHECK(dp_udiv(7, 2, LOC) == 3 && dp_urem(7, 2, LOC) == 1 && dp_urem(UINT64_MAX, 10, LOC) == 5);

    CHECK(dp_shift_count_s(7, 8, "i8", LOC) == 7 && dp_shift_count_s(0, 64, "i64", LOC) == 0);
    CHECK(dp_shift_count_u(63, 64, "u64", LOC) == 63);
    CHECK(dp_sshl(1, 7, 8) == -128 && dp_sshl(-1, 63, 64) == INT64_MIN && dp_sshl(3, 62, 64) == INT64_MIN + (INT64_C(1) << 62));
    CHECK(dp_ushl(200, 1, 8) == 144 && dp_ushl(UINT64_MAX, 63, 64) == UINT64_C(1) << 63);
    CHECK(dp_sshr(-128, 1) == -64 && dp_sshr(-1, 63) == -1 && dp_sshr(INT64_MIN, 63) == -1);
    CHECK(dp_sshr(5, 1) == 2 && dp_sshr(INT64_MAX, 62) == 1 && dp_sshr(-5, 1) == -3);
}

static void unit_convert(void) {
    CHECK(dp_cast_s_to_s(-128, 8, "i8", LOC) == -128 && dp_cast_s_to_s(127, 8, "i8", LOC) == 127);
    CHECK(dp_cast_s_to_s(INT64_MIN, 64, "i64", LOC) == INT64_MIN);
    CHECK(dp_cast_s_to_u(255, 8, "u8", LOC) == 255 && dp_cast_s_to_u(INT64_MAX, 64, "u64", LOC) == (uint64_t)INT64_MAX);
    CHECK(dp_cast_u_to_s(127, 8, "i8", LOC) == 127 && dp_cast_u_to_s((uint64_t)INT64_MAX, 64, "i64", LOC) == INT64_MAX);
    CHECK(dp_cast_u_to_u(UINT32_MAX, 32, "u32", LOC) == UINT32_MAX);

    CHECK(dp_cast_f_to_s(-3.99, false, 32, "i32", LOC) == -3 && dp_cast_f_to_s(3.99, false, 32, "i32", LOC) == 3);
    CHECK(dp_cast_f_to_s(-0.5, false, 8, "i8", LOC) == 0 && dp_cast_f_to_s(-0.0, false, 8, "i8", LOC) == 0);
    CHECK(dp_cast_f_to_s(-2147483648.9, false, 32, "i32", LOC) == INT32_MIN);
    CHECK(dp_cast_f_to_s(2147483647.9, false, 32, "i32", LOC) == INT32_MAX);
    CHECK(dp_cast_f_to_s(-9223372036854775808.0, false, 64, "i64", LOC) == INT64_MIN);
    CHECK(dp_cast_f_to_s(9223372036854774784.0, false, 64, "i64", LOC) == INT64_C(9223372036854774784));
    CHECK(dp_cast_f_to_u(-0.99, false, 8, "u8", LOC) == 0 && dp_cast_f_to_u(255.99, false, 8, "u8", LOC) == 255);
    CHECK(dp_cast_f_to_u(18446744073709549568.0, false, 64, "u64", LOC) == UINT64_C(18446744073709549568));
    CHECK(dp_cast_f_to_s((double)16777216.0f, true, 32, "i32", LOC) == 16777216);

    CHECK(dp_cast_f64_to_f32((double)FLT_MAX, LOC) == FLT_MAX && dp_cast_f64_to_f32(-(double)FLT_MAX, LOC) == -FLT_MAX);
    CHECK(dp_cast_f64_to_f32(0.1, LOC) == 0.1f && dp_cast_f64_to_f32(1e-46, LOC) == 0.0f);
    CHECK(isinf(dp_cast_f64_to_f32(INFINITY, LOC)) && isnan(dp_cast_f64_to_f32(NAN, LOC)));
}

static void unit_str(void) {
    dp_str e = dp_str_own(NULL, 0, LOC);
    CHECK(e.ptr == NULL && e.len == 0);
    CHECK(take_is(dp_str_concat(S("ab"), S("cd"), LOC), "abcd"));
    CHECK(take_is(dp_str_concat(S("ab"), S(""), LOC), "ab") && take_is(dp_str_concat(S(""), S("c"), LOC), "c"));
    dp_str both = dp_str_concat(S(""), S(""), LOC);
    CHECK(both.ptr == NULL && both.len == 0);

    CHECK(dp_str_eq(S(""), S("")) && dp_str_eq(S("a"), S("a")) && !dp_str_eq(S("a"), S("ab")));
    CHECK(dp_str_lt(S(""), S("a")) && dp_str_lt(S("a"), S("b")) && dp_str_lt(S("ab"), S("b")));
    CHECK(dp_str_lt(S("ab"), S("abc")) && !dp_str_lt(S("abc"), S("ab")) && !dp_str_lt(S("a"), S("a")));
    CHECK(dp_str_lt(S("a"), S("\xff")) && !dp_str_lt(S("\xc3\xa9"), S("z")));  /* по байтам без знака */

    CHECK(dp_str_at(S("abc"), 2, LOC) == 'c' && dp_str_at(S("\xff"), 0, LOC) == 0xFF);
    CHECK(take_is(dp_str_slice(S("h\xc3\xa9llo"), 0, 1, LOC), "h"));
    CHECK(take_is(dp_str_slice(S("h\xc3\xa9llo"), 1, 3, LOC), "\xc3\xa9"));
    CHECK(take_is(dp_str_slice(S("h\xc3\xa9llo"), 6, 6, LOC), "") && take_is(dp_str_slice(S(""), 0, 0, LOC), ""));

    CHECK(take_is(dp_str_from_s(INT64_MIN), "-9223372036854775808") && take_is(dp_str_from_s(0), "0"));
    CHECK(take_is(dp_str_from_u(UINT64_MAX), "18446744073709551615"));
    CHECK(take_is(dp_str_from_bool(true), "true") && take_is(dp_str_from_bool(false), "false"));

    /* Р48: кратчайшая запись, обычная форма при порядке от −4 до 15. */
    CHECK(float_is(0.1, false, "0.1") && float_is(100.0, false, "100") && float_is(-2.5, false, "-2.5"));
    CHECK(float_is(1e15, false, "1000000000000000") && float_is(1e16, false, "1e+16"));
    CHECK(float_is(0.0001, false, "0.0001") && float_is(0.00001, false, "1e-05") && float_is(1.5e-7, false, "1.5e-07"));
    CHECK(float_is(0.0, false, "0") && float_is(-0.0, false, "-0"));
    CHECK(float_is(INFINITY, false, "inf") && float_is(-INFINITY, false, "-inf") && float_is(NAN, false, "nan"));
    CHECK(float_is(5e-324, false, "5e-324") && float_is(DBL_MAX, false, "1.7976931348623157e+308"));
    CHECK(float_is(9999999999999998.0, false, "9999999999999998"));
    CHECK(float_is(0.1f, true, "0.1") && float_is(FLT_MAX, true, "3.4028235e+38") && float_is(16777216.0f, true, "16777216"));
    CHECK(float_is(1.4e-45f, true, "1e-45") && float_is(FLT_MIN, true, "1.1754944e-38"));
    /* Степени двойки: ближайшая запись из p цифр не читается обратно, соседняя — да. */
    CHECK(float_is(ldexp(1.0, -1017), false, "7.120236347223045e-307"));
    CHECK(float_is((double)ldexpf(1.0f, -96), true, "1.2621775e-29") && float_is((double)ldexpf(1.0f, 87), true, "1.5474251e+26"));

    int64_t n = 99;
    CHECK(dp_parse_int(S("0"), &n) && n == 0 && dp_parse_int(S("-0"), &n) && n == 0);
    CHECK(dp_parse_int(S("+5"), &n) && n == 5 && dp_parse_int(S("007"), &n) && n == 7);
    CHECK(dp_parse_int(S("-9223372036854775808"), &n) && n == INT64_MIN);
    CHECK(dp_parse_int(S("9223372036854775807"), &n) && n == INT64_MAX);
    CHECK(!dp_parse_int(S("9223372036854775808"), &n) && !dp_parse_int(S("-9223372036854775809"), &n));
    CHECK(!dp_parse_int(S(""), &n) && !dp_parse_int(S("-"), &n) && !dp_parse_int(S("+"), &n));
    CHECK(!dp_parse_int(S(" 1"), &n) && !dp_parse_int(S("1 "), &n) && !dp_parse_int(S("12a"), &n));
    CHECK(!dp_parse_int(S("99999999999999999999"), &n) && !dp_parse_int(S("--1"), &n));

    CHECK(take_is(dp_str_trim(S("  a b \t\n\r")), "a b") && take_is(dp_str_trim(S("")), ""));
    CHECK(take_is(dp_str_trim(S(" \t ")), "") && take_is(dp_str_trim(S("\va\f")), "\va\f"));

    int64_t pos = -1;
    CHECK(dp_str_find_from(S("hello"), S("l"), 0, &pos) && pos == 2);
    CHECK(dp_str_find_from(S("hello"), S("l"), 3, &pos) && pos == 3 && !dp_str_find_from(S("hello"), S("l"), 4, &pos));
    CHECK(dp_str_find_from(S("abc"), S(""), 3, &pos) && pos == 3 && !dp_str_find_from(S("abc"), S(""), 4, &pos));
    CHECK(!dp_str_find_from(S("abc"), S("abcd"), 0, &pos) && dp_str_find_from(S("aaa"), S("aa"), 1, &pos) && pos == 1);
    CHECK(dp_str_starts_with(S("abc"), S("ab")) && dp_str_starts_with(S("abc"), S("")));
    CHECK(!dp_str_starts_with(S("ab"), S("abc")) && dp_str_starts_with(S(""), S("")));

    CHECK(dp_is_digit('0') && dp_is_digit('9') && !dp_is_digit('/') && !dp_is_digit(':'));
    CHECK(dp_is_alpha('A') && dp_is_alpha('Z') && dp_is_alpha('a') && dp_is_alpha('z'));
    CHECK(!dp_is_alpha('@') && !dp_is_alpha('[') && !dp_is_alpha('`') && !dp_is_alpha('{') && !dp_is_alpha(0xC3));
    CHECK(dp_is_space(' ') && dp_is_space('\t') && dp_is_space('\n') && dp_is_space('\r'));
    CHECK(!dp_is_space('\v') && !dp_is_space('\f') && !dp_is_space(0xA0));
    CHECK(dp_is_alnum('5') && dp_is_alnum('q') && !dp_is_alnum('_'));
}

static void unit_index_memory(void) {
    CHECK(dp_check_index(0, 1, LOC) == 0 && dp_check_index(4, 5, LOC) == 4);
    dp_check_slice(0, 0, 0, LOC);
    dp_check_slice(3, 3, 3, LOC);
    dp_check_slice(0, 3, 3, LOC);
    dp_check_insert(3, 3, LOC);
    dp_check_insert(0, 0, LOC);
    dp_check_repeat(0, LOC);
    CHECK(dp_alloc(0, 8, LOC) == NULL && dp_alloc(-1, 8, LOC) == NULL);
    uint64_t* p = (uint64_t*)dp_alloc(16, sizeof *p, LOC);
    CHECK(p != NULL);
    p[15] = 7;
    p = (uint64_t*)dp_realloc(p, 32, sizeof *p, LOC);
    CHECK(p != NULL && p[15] == 7);
    p[31] = 1;
    CHECK(dp_realloc(p, 0, sizeof *p, LOC) == NULL);  /* освобождает */
    dp_free(NULL);
    uint8_t* big = (uint8_t*)dp_alloc(1 << 20, 1, LOC);
    CHECK(big != NULL);
    dp_free(big);
}

/* --- ловушки: по одной на запуск */

static void trap_case(const char* name) {
    volatile int64_t zero = 0;  /* не даёт компилятору свернуть деление */
    if (!strcmp(name, "sadd")) dp_sadd(INT64_MAX, 1, 64, "i64", LOC);
    if (!strcmp(name, "sadd8")) dp_sadd(127, 1, 8, "i8", LOC);
    if (!strcmp(name, "ssub")) dp_ssub(INT64_MIN, 1, 64, "int", LOC);
    if (!strcmp(name, "smul")) dp_smul(INT64_MIN, -1, 64, "i64", LOC);
    if (!strcmp(name, "smul32")) dp_smul(65536, 32768, 32, "i32", LOC);
    if (!strcmp(name, "sdiv0")) dp_sdiv(1, zero, 32, "i32", LOC);
    if (!strcmp(name, "sdivmin")) dp_sdiv(-128, -1, 8, "i8", LOC);
    if (!strcmp(name, "srem0")) dp_srem(1, zero, 64, "i64", LOC);
    if (!strcmp(name, "sneg")) dp_sneg(-128, 8, "i8", LOC);
    if (!strcmp(name, "sabs")) dp_sabs(INT64_MIN, 64, "i64", LOC);
    if (!strcmp(name, "uadd")) dp_uadd(255, 1, 8, "u8", LOC);
    if (!strcmp(name, "usub")) dp_usub(0, 1, 8, "u8", LOC);
    if (!strcmp(name, "umul")) dp_umul(UINT64_C(1) << 32, UINT64_C(1) << 32, 64, "u64", LOC);
    if (!strcmp(name, "udiv0")) dp_udiv(1, (uint64_t)zero, LOC);
    if (!strcmp(name, "urem0")) dp_urem(1, (uint64_t)zero, LOC);
    if (!strcmp(name, "shift_neg")) dp_shift_count_s(-1, 32, "i32", LOC);
    if (!strcmp(name, "shift_wide")) dp_shift_count_u(64, 64, "u64", LOC);
    if (!strcmp(name, "cast_ss")) dp_cast_s_to_s(128, 8, "i8", LOC);
    if (!strcmp(name, "cast_su")) dp_cast_s_to_u(-1, 32, "u32", LOC);
    if (!strcmp(name, "cast_us")) dp_cast_u_to_s(UINT64_MAX, 64, "i64", LOC);
    if (!strcmp(name, "cast_uu")) dp_cast_u_to_u(256, 8, "u8", LOC);
    if (!strcmp(name, "cast_nan")) dp_cast_f_to_s(NAN, false, 32, "i32", LOC);
    if (!strcmp(name, "cast_inf")) dp_cast_f_to_u(INFINITY, false, 64, "u64", LOC);
    if (!strcmp(name, "cast_big")) dp_cast_f_to_s(2147483648.0, false, 32, "i32", LOC);
    if (!strcmp(name, "cast_neg")) dp_cast_f_to_u(-1.0, false, 8, "u8", LOC);
    if (!strcmp(name, "cast_i64_top")) dp_cast_f_to_s(9223372036854775808.0, false, 64, "i64", LOC);
    if (!strcmp(name, "cast_f32_text")) dp_cast_f_to_s((double)3.4e38f, true, 64, "i64", LOC);
    if (!strcmp(name, "f64_to_f32")) dp_cast_f64_to_f32(1e39, LOC);
    if (!strcmp(name, "index")) dp_check_index(5, 5, LOC);
    if (!strcmp(name, "index_neg")) dp_check_index(-1, 3, LOC);
    if (!strcmp(name, "slice")) dp_check_slice(2, 1, 5, LOC);
    if (!strcmp(name, "slice_end")) dp_check_slice(0, 6, 5, LOC);
    if (!strcmp(name, "insert")) dp_check_insert(4, 3, LOC);
    if (!strcmp(name, "repeat")) dp_check_repeat(-2, LOC);
    if (!strcmp(name, "str_at")) dp_str_at(S("abc"), 3, LOC);
    if (!strcmp(name, "str_utf8")) dp_str_slice(S("h\xc3\xa9llo"), 0, 2, LOC);
    if (!strcmp(name, "str_bounds")) dp_str_slice(S("abc"), 1, 4, LOC);
    if (!strcmp(name, "alloc")) dp_alloc(DP_MAX_BYTES + 1, 1, LOC);
    if (!strcmp(name, "alloc_elem")) dp_alloc(DP_MAX_BYTES / 8 + 1, 8, LOC);
    if (!strcmp(name, "realloc")) dp_realloc(NULL, DP_MAX_BYTES + 1, 1, LOC);
    if (!strcmp(name, "concat")) {
        dp_str huge = {NULL, DP_MAX_BYTES};  /* только длина: проверка до копирования */
        dp_str_concat(huge, S("x"), LOC);
    }
    if (!strcmp(name, "depth")) {
        dp_depth = DP_MAX_DEPTH;
        DP_CALL_CHECK(LOC);
    }
    if (!strcmp(name, "long")) {
        static char text[1001];
        memset(text, 'x', 1000);
        dp_trapf(LOC, "%s", text);  /* сообщение обрезается, не переполняет */
    }
    if (!strcmp(name, "noloc")) dp_trap(NULL, "boom");
    if (!strcmp(name, "emptyloc")) dp_trap("", "boom");
    if (!strcmp(name, "unreachable")) dp_unreachable("m.f");
    if (!strcmp(name, "flush")) {
        dp_print(S("before"));
        dp_trap(LOC, "assertion failed");
    }
    if (!strcmp(name, "order")) {  /* stderr слит со stdout: порядок строк — порядок программы */
        dp_println(S("1"));
        dp_eprintln(S("2"));
        dp_println(S("3"));
        exit(0);
    }
    fprintf(stderr, "unknown trap case '%s'\n", name);
    exit(2);
}

/* --- вход dp_run */

static const char* entry_ok(void) {
    dp_println(S("hi"));
    return NULL;
}

static const char* entry_error(void) {
    dp_println(S("before"));
    return "ParseError.BadKey";
}

static const char* entry_trap(void) {
    dp_print(S("partial"));
    dp_trap(LOC, "division by zero");
}

/* Рекурсия, как у сгенерированной функции: проверка глубины до вызова. */
static int64_t deep(int64_t n) {
    dp_depth++;
    int64_t r = n;
    if (n > 0) {
        DP_CALL_CHECK(LOC);
        r = deep(n - 1) + 1;
    }
    dp_depth--;
    return r;
}

static const char* entry_deep(void) {
    int64_t r = deep(DP_MAX_DEPTH - 2);  /* ровно в пределе */
    dp_println(r == DP_MAX_DEPTH - 2 ? S("deep ok") : S("deep wrong"));
    deep(DP_MAX_DEPTH);  /* на один кадр глубже предела — ловушка */
    return NULL;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "unit")) {
        unit_int();
        unit_convert();
        unit_str();
        unit_index_memory();
        if (failures) {
            fprintf(stderr, "%d of %d checks failed\n", failures, checks);
            return 1;
        }
        printf("ok %d\n", checks);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "trap")) trap_case(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "run")) {
        if (!strcmp(argv[2], "ok")) return dp_run(entry_ok);
        if (!strcmp(argv[2], "error")) return dp_run(entry_error);
        if (!strcmp(argv[2], "trap")) return dp_run(entry_trap);
        if (!strcmp(argv[2], "deep")) return dp_run(entry_deep);
    }
    fprintf(stderr, "usage: rt_test unit | trap NAME | run ok|error|trap|deep\n");
    return 2;
}
