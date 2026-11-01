/* Runtime dpl под Windows x86-64 (MinGW-w64): вход в программу (trap.exit;
 * docs/runtime/README.md). Как под Linux, main программы исполняется в
 * потоке с большим стеком, чтобы предел глубины вызовов (DP_MAX_DEPTH)
 * срабатывал ловушкой раньше, чем кончится стек. Вывод — байты как есть,
 * без замены \n на \r\n; аргументы и пути файлов — в UTF-8 (Р51). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include "dp_rt.h"

#define DP_STACK_BYTES ((size_t)256 << 20)

static dp_entry_fn dp_entry_main;
static const char* dp_main_error;

static DWORD WINAPI dp_thread(LPVOID arg) {
    (void)arg;
    dp_main_error = dp_entry_main();
    return 0;
}

static void dp_binary_io(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
}

/* Строка UTF-16 в UTF-8; память живёт до конца программы. */
static char* dp_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char* s = (char*)dp_alloc(n > 0 ? n : 1, 1, NULL);
    if (n <= 0 || WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) s[0] = 0;
    return s;
}

/* argv от MinGW — в кодовой странице ANSI; аргументы берутся заново из
 * командной строки UTF-16. */
static void dp_utf8_args(void) {
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return;
    char** argv = (char**)dp_alloc(argc + 1, sizeof(char*), NULL);
    for (int i = 0; i < argc; i++) argv[i] = dp_utf8(wargv[i]);
    argv[argc] = NULL;
    LocalFree(wargv);
    dp_set_args(argc, argv);
}

static int dp_run_as(dp_entry_fn entry, const char* what) {
    dp_binary_io();
    dp_entry_main = entry;
    dp_depth = 0;
    HANDLE th = CreateThread(NULL, DP_STACK_BYTES, dp_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (th) {
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
    } else {
        dp_main_error = entry();
    }
    fflush(stdout);
    dp_alloc_report();
    if (dp_main_error) {
        fprintf(stderr, "error: %s failed with %s\n", what, dp_main_error);
        return 1;
    }
    return 0;
}

int dp_run(dp_entry_fn entry) {
    dp_utf8_args();
    return dp_run_as(entry, "main");
}

int dp_run_test(dp_entry_fn entry) { return dp_run_as(entry, "test"); }

size_t dp_sys_alloc_size(void* p) { return _msize(p); }

/* --- потоки пула `parallel for` (Р69) */

int dp_sys_cpus(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

typedef struct {
    void (*fn)(void*);
    void* arg;
} dp_sys_start;

static DWORD WINAPI dp_sys_trampoline(LPVOID p) {
    dp_sys_start s = *(dp_sys_start*)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int dp_sys_thread(void (*fn)(void*), void* arg) {
    dp_sys_start* s = (dp_sys_start*)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    HANDLE th = CreateThread(NULL, DP_STACK_BYTES, dp_sys_trampoline, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!th) {
        free(s);
        return -1;
    }
    CloseHandle(th);
    return 0;
}

void* dp_sys_mutex(void) {
    SRWLOCK* m = (SRWLOCK*)malloc(sizeof *m);
    if (m) InitializeSRWLock(m);
    return m;
}

void dp_sys_lock(void* m) { AcquireSRWLockExclusive((SRWLOCK*)m); }

void dp_sys_unlock(void* m) { ReleaseSRWLockExclusive((SRWLOCK*)m); }

void dp_sys_mutex_free(void* m) { free(m); }

void* dp_sys_cond(void) {
    CONDITION_VARIABLE* c = (CONDITION_VARIABLE*)malloc(sizeof *c);
    if (c) InitializeConditionVariable(c);
    return c;
}

void dp_sys_wait(void* c, void* m) { SleepConditionVariableSRW((CONDITION_VARIABLE*)c, (SRWLOCK*)m, INFINITE, 0); }

void dp_sys_broadcast(void* c) { WakeAllConditionVariable((CONDITION_VARIABLE*)c); }

void dp_sys_signal(void* c) { WakeConditionVariable((CONDITION_VARIABLE*)c); }

void dp_sys_cond_free(void* c) { free(c); }

/* Путь UTF-8 в UTF-16 для _wfopen; NULL — путь не UTF-8 (errno ENOENT). */
static wchar_t* dp_os_wpath(const char* path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n <= 0) {
        errno = ENOENT;
        return NULL;
    }
    wchar_t* w = (wchar_t*)dp_alloc(n, sizeof(wchar_t), NULL);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    return w;
}

FILE* dp_os_open_write(const char* path) {
    wchar_t* w = dp_os_wpath(path);
    if (!w) return NULL;
    FILE* f = _wfopen(w, L"wb");
    int err = errno;
    dp_free(w);
    errno = err;
    return f;
}

FILE* dp_os_open_read(const char* path) {
    wchar_t* w = dp_os_wpath(path);
    if (!w) return NULL;
    FILE* f = _wfopen(w, L"rb");
    int err = errno;
    /* Каталог Windows не открывает (EACCES); как под Linux, это не отказ в
     * доступе, а файл, который нельзя прочесть (os.IoError.Other). */
    DWORD attr = f ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(w);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) err = EISDIR;
    dp_free(w);
    errno = err;
    return f;
}
