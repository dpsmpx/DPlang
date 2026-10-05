/* Runtime DPlang под Linux: вход в программу (trap.exit;
 * docs/runtime/README.md). main программы исполняется в потоке с большим
 * стеком: предел глубины вызовов (DP_MAX_DEPTH) должен срабатывать
 * ловушкой раньше, чем кончится стек процесса. */
#include <pthread.h>
#include <stdio.h>

#include "dp_rt.h"

#define DP_STACK_BYTES ((size_t)256 << 20)

static dp_entry_fn dp_entry_main;
static const char* dp_main_error;

static void* dp_thread(void* arg) {
    (void)arg;
    dp_main_error = dp_entry_main();
    return NULL;
}

int dp_run(dp_entry_fn entry) {
    dp_entry_main = entry;
    dp_depth = 0;
    pthread_attr_t attr;
    pthread_t th;
    int ok = pthread_attr_init(&attr) == 0 && pthread_attr_setstacksize(&attr, DP_STACK_BYTES) == 0 &&
             pthread_create(&th, &attr, dp_thread, NULL) == 0;
    if (ok) pthread_join(th, NULL);
    else dp_main_error = entry();
    fflush(stdout);
    if (dp_main_error) {
        fprintf(stderr, "error: main failed with %s\n", dp_main_error);
        return 1;
    }
    return 0;
}
