// Режим `dplang worker` (worker.h).
#include "driver/worker.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

#include "api/contract.h"
#include "driver/big_stack.h"
#include "driver/sandbox.h"

#if defined(__linux__)
#include <malloc.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define DP_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DP_SANITIZED 1
#endif
#endif

namespace dp {

#if defined(__linux__)

namespace {

void put_err(std::string_view s) {
    while (!s.empty()) {
        ssize_t n = ::write(2, s.data(), s.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        s.remove_prefix(static_cast<size_t>(n));
    }
}

bool write_all(int fd, std::string_view s) {
    while (!s.empty()) {
        ssize_t n = ::write(fd, s.data(), s.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        s.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

int usage(std::string_view message) {
    put_err("error: worker: ");
    put_err(message);
    put_err("\n");
    return 2;
}

int internal(std::string_view message) {
    put_err("internal compiler error: worker: ");
    put_err(message);
    put_err("\n");
    return 3;
}

// Аргументы worker-а: пределы профиля (contract.h) и ресурсы процесса.
struct Options {
    api::OnlineLimits limits;
    uint64_t memory_bytes = 0;  // RLIMIT_AS
    uint64_t cpu_seconds = 0;   // RLIMIT_CPU
    uint64_t stack_bytes = 0;   // стек потока компилятора
};

struct Flag {
    std::string_view name;
    uint32_t* u32;
    uint64_t* u64;
};

bool parse_number(std::string_view s, uint64_t& out) {
    if (s.empty() || s.size() > 19) return false;
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + static_cast<uint64_t>(c - '0');
    }
    return out > 0;
}

// Все пределы обязательны: значений по умолчанию нет, их задаёт профиль.
int parse_options(int argc, char** argv, Options& o) {
    api::OnlineLimits& l = o.limits;
    Flag flags[] = {
        {"request-bytes", &l.request_bytes, nullptr},   {"source-bytes", &l.source_bytes, nullptr},
        {"lines", &l.lines, nullptr},                   {"identifier-bytes", &l.identifier_bytes, nullptr},
        {"nesting", &l.nesting, nullptr},               {"diagnostics", &l.diagnostics, nullptr},
        {"output-bytes", &l.output_bytes, nullptr},     {"memory-bytes", nullptr, &o.memory_bytes},
        {"cpu-seconds", nullptr, &o.cpu_seconds},       {"stack-bytes", nullptr, &o.stack_bytes},
    };
    bool seen[sizeof flags / sizeof flags[0]] = {};
    for (int i = 2; i < argc; i++) {
        std::string_view a = argv[i];
        size_t eq = a.find('=');
        if (a.substr(0, 2) != "--" || eq == std::string_view::npos) {
            return usage("unexpected argument '" + std::string(a) + "'");
        }
        std::string_view name = a.substr(2, eq - 2);
        size_t k = 0;
        while (k < sizeof flags / sizeof flags[0] && flags[k].name != name) k++;
        if (k == sizeof flags / sizeof flags[0]) return usage("unknown option '--" + std::string(name) + "'");
        if (seen[k]) return usage("option '--" + std::string(name) + "' is given twice");
        uint64_t v;
        if (!parse_number(a.substr(eq + 1), v) || (flags[k].u32 && v > UINT32_MAX)) {
            return usage("option '--" + std::string(name) + "' needs a positive number");
        }
        if (flags[k].u32) *flags[k].u32 = static_cast<uint32_t>(v);
        else *flags[k].u64 = v;
        seen[k] = true;
    }
    for (size_t k = 0; k < sizeof flags / sizeof flags[0]; k++) {
        if (!seen[k]) return usage("option '--" + std::string(flags[k].name) + "' is required");
    }
    return 0;
}

// Ответ на нехватку памяти готовится заранее: в обработчике выделять нельзя.
// Обработчик new — состояние процесса по природе, поэтому и ответ здесь.
std::string& oom_reply() {
    static std::string reply;
    return reply;
}

void on_out_of_memory() {
    write_all(1, oom_reply());
    _exit(4);
}

bool set_limit(int resource, uint64_t soft, uint64_t hard, std::string& error) {
    rlimit r{static_cast<rlim_t>(soft), static_cast<rlim_t>(hard)};
    if (setrlimit(resource, &r) == 0) return true;
    error = std::string("setrlimit: ") + std::strerror(errno);
    return false;
}

// Пределы процесса до чтения входа: память, CPU, без новых дескрипторов и
// дампов. Если stdout или stderr — файл, а не канал, запись в него — не
// больше предела ответа с запасом на сообщения.
bool limit_process(const Options& o, std::string& error) {
    uint64_t file_bytes = uint64_t(o.limits.output_bytes) + (uint64_t(1) << 16);
    return set_limit(RLIMIT_CORE, 0, 0, error) && set_limit(RLIMIT_FSIZE, file_bytes, file_bytes, error) &&
           set_limit(RLIMIT_NOFILE, 3, 3, error) && set_limit(RLIMIT_CPU, o.cpu_seconds, o.cpu_seconds + 1, error) &&
           set_limit(RLIMIT_AS, o.memory_bytes, o.memory_bytes, error);
}

struct Job {
    const Options* opt;
    int code;
};

// Поток компилятора: запереть, прочитать вход, ответить.
void serve(void* p) {
    auto* job = static_cast<Job*>(p);
    std::string error;
    if (!lock_down(error)) {
        job->code = internal("cannot enter the sandbox: " + error);
        return;
    }
    const api::OnlineLimits& lim = job->opt->limits;
    // Читается не больше предела + 1 байт: этого хватает, чтобы ответить
    // limit_exceeded, и большой вход не попадает в память.
    std::string request;
    char buf[1 << 16];
    for (;;) {
        size_t want = sizeof buf;
        size_t left = static_cast<size_t>(lim.request_bytes) + 1 - request.size();
        if (left == 0) break;
        if (want > left) want = left;
        ssize_t n = ::read(0, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            job->code = internal(std::string("cannot read the request: ") + std::strerror(errno));
            return;
        }
        if (n == 0) break;
        request.append(buf, static_cast<size_t>(n));
    }
    std::string reply = api::compile_v1(request, lim);
    job->code = write_all(1, reply) ? 0 : internal("cannot write the reply");
}

}  // namespace

int run_worker(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[2]) == "--describe") {
        return write_all(1, api::describe_v1()) ? 0 : 3;
    }
    Options opt;
    if (int rc = parse_options(argc, argv, opt)) return rc;
#if defined(DP_SANITIZED)
    // Санитайзеры делают системные вызовы вне списка песочницы и
    // резервируют память больше любого RLIMIT_AS: worker — только в сборке
    // без них (docs/service/README.md, «Сборка»).
    return usage("not available in a build with sanitizers; use the release build");
#endif
    // Лишние дескрипторы закрываются: worker-у нужны только 0, 1 и 2.
    syscall(SYS_close_range, 3u, ~0u, 0u);
    std::signal(SIGPIPE, SIG_IGN);
#if defined(M_ARENA_MAX)
    // Одна арена malloc: поток компилятора не резервирует свою (64 МБ
    // адресов на арену), и RLIMIT_AS считает только нужное.
    mallopt(M_ARENA_MAX, 1);
#endif
    oom_reply() = api::error_v1("limit_exceeded", "compilation needs more memory than the limit of " +
                                                      std::to_string(opt.memory_bytes) + " bytes",
                                "memory_bytes", opt.memory_bytes);
    std::set_new_handler(on_out_of_memory);
    std::string error;
    if (!limit_process(opt, error)) return internal(error);
    Job job{&opt, 3};
    if (!run_on_stack(static_cast<size_t>(opt.stack_bytes), serve, &job)) {
        return internal("cannot start the compiler thread");
    }
    return job.code;
}

#else

int run_worker(int, char**) {
    std::fputs("error: worker: the online-service worker runs only on Linux\n", stderr);
    return 2;
}

#endif

}  // namespace dp
