/* Runtime dpl под Linux: вход в программу (trap.exit;
 * docs/runtime/README.md). main программы исполняется в потоке с большим
 * стеком: предел глубины вызовов (DP_MAX_DEPTH) должен срабатывать
 * ловушкой раньше, чем кончится стек процесса. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dp_rt.h"

#define DP_STACK_BYTES ((size_t)256 << 20)

static dp_entry_fn dp_entry_main;
static const char* dp_main_error;

static void* dp_thread(void* arg) {
    (void)arg;
    dp_main_error = dp_entry_main();
    return NULL;
}

static int dp_run_as(dp_entry_fn entry, const char* what) {
    dp_entry_main = entry;
    dp_depth = 0;
    pthread_attr_t attr;
    pthread_t th;
    int ok = pthread_attr_init(&attr) == 0 && pthread_attr_setstacksize(&attr, DP_STACK_BYTES) == 0 &&
             pthread_create(&th, &attr, dp_thread, NULL) == 0;
    if (ok) pthread_join(th, NULL);
    else dp_main_error = entry();
    fflush(stdout);
    dp_alloc_report();
    if (dp_main_error) {
        fprintf(stderr, "error: %s failed with %s\n", what, dp_main_error);
        return 1;
    }
    return 0;
}

int dp_run(dp_entry_fn entry) { return dp_run_as(entry, "main"); }

/* --- потоки пула `parallel for` (Р69) */

int dp_sys_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

typedef struct {
    void (*fn)(void*);
    void* arg;
} dp_sys_start;

static void* dp_sys_trampoline(void* p) {
    dp_sys_start s = *(dp_sys_start*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int dp_sys_thread(void (*fn)(void*), void* arg) {
    dp_sys_start* s = (dp_sys_start*)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t attr;
    pthread_t th;
    int ok = pthread_attr_init(&attr) == 0 && pthread_attr_setstacksize(&attr, DP_STACK_BYTES) == 0 &&
             pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0 &&
             pthread_create(&th, &attr, dp_sys_trampoline, s) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) free(s);
    return ok ? 0 : -1;
}

void* dp_sys_mutex(void) {
    pthread_mutex_t* m = (pthread_mutex_t*)malloc(sizeof *m);
    if (m && pthread_mutex_init(m, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

void dp_sys_lock(void* m) { pthread_mutex_lock((pthread_mutex_t*)m); }

void dp_sys_unlock(void* m) { pthread_mutex_unlock((pthread_mutex_t*)m); }

void dp_sys_mutex_free(void* m) {
    pthread_mutex_destroy((pthread_mutex_t*)m);
    free(m);
}

void* dp_sys_cond(void) {
    pthread_cond_t* c = (pthread_cond_t*)malloc(sizeof *c);
    if (c && pthread_cond_init(c, NULL) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

void dp_sys_wait(void* c, void* m) { pthread_cond_wait((pthread_cond_t*)c, (pthread_mutex_t*)m); }

void dp_sys_broadcast(void* c) { pthread_cond_broadcast((pthread_cond_t*)c); }

void dp_sys_cond_free(void* c) {
    pthread_cond_destroy((pthread_cond_t*)c);
    free(c);
}

int dp_run_test(dp_entry_fn entry) { return dp_run_as(entry, "test"); }

FILE* dp_os_open_read(const char* path) { return fopen(path, "rb"); }
