// Пробник песочницы worker-а (sandbox.h) для тестов злоупотреблений.
//
//     dpsandbox_probe <действие>
//
// Второй поток запирает процесс (как worker), главный поток после него
// делает действие: так проверяется и TSYNC — фильтр на всех потоках.
// Разрешённое действие `ok` (память, запись) печатает «ok» и выходит с 0;
// запрещённое должно убить процесс сигналом SIGSYS. Если запрещённое
// прошло — печать «escaped» и код 1.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "driver/big_stack.h"
#include "driver/sandbox.h"

namespace {

struct Lock {
    bool ok = false;
    std::string error;
};

void lock(void* p) {
    auto* l = static_cast<Lock*>(p);
    l->ok = dp::lock_down(l->error);
}

void say(std::string_view s) {
    ssize_t n = write(1, s.data(), s.size());
    (void)n;
}

// Выделение и запись по многим страницам — память после запирания работает.
bool memory_works() {
    const size_t n = size_t(64) << 20;
    char* big = static_cast<char*>(std::malloc(n));
    if (!big) return false;
    for (size_t i = 0; i < n; i += 4096) big[i] = static_cast<char>(i);
    big = static_cast<char*>(std::realloc(big, n * 2));
    if (!big) return false;
    std::free(big);
    std::string s;
    for (int i = 0; i < 100000; i++) s += "abc";
    return s.size() == 300000;
}

// Действие; true — запрещённое не убило процесс.
bool act(std::string_view what) {
    if (what == "open") return open("/etc/hostname", O_RDONLY) >= 0 || errno != 0;
    if (what == "creat") return open("dp_probe_file", O_WRONLY | O_CREAT, 0600) >= 0 || errno != 0;
    if (what == "socket") return socket(AF_INET, SOCK_STREAM, 0) >= 0 || errno != 0;
    if (what == "unix_socket") return socket(AF_UNIX, SOCK_STREAM, 0) >= 0 || errno != 0;
    if (what == "execve") {
        char* argv[] = {const_cast<char*>("/bin/true"), nullptr};
        char* envp[] = {nullptr};
        execve("/bin/true", argv, envp);
        return true;
    }
    if (what == "fork") return fork() >= 0 || errno != 0;
    if (what == "thread") {
        Lock l;
        return dp::run_on_stack(1 << 20, [](void*) {}, &l) || true;
    }
    if (what == "mmap_exec") {
        return mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED ||
               errno != 0;
    }
    if (what == "mprotect_exec") {
        void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return mprotect(p, 4096, PROT_READ | PROT_EXEC) == 0 || errno != 0;
    }
    if (what == "ptrace") return ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) >= 0 || errno != 0;
    if (what == "kill") return kill(getppid(), 0) == 0 || errno != 0;
    if (what == "getpid") return syscall(SYS_getpid) >= 0;
#if defined(__x86_64__)
    if (what == "x32") return syscall(0x40000000 | SYS_getpid) >= 0 || errno != 0;
#endif
    say("unknown action\n");
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        say("usage: dpsandbox_probe <action>\n");
        return 2;
    }
    Lock l;
    if (!dp::run_on_stack(1 << 20, lock, &l) || !l.ok) {
        say("lock failed: " + l.error + "\n");
        return 3;
    }
    std::string_view what = argv[1];
    if (what == "ok") {
        if (!memory_works()) return 1;
        say("ok\n");
        return 0;
    }
    act(what);
    say("escaped\n");
    return 1;
}
