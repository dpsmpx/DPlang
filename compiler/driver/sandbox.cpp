// Песочница worker-а (sandbox.h): seccomp-BPF со списком разрешённого.
#include "driver/sandbox.h"

#if defined(__linux__)
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#endif

namespace dp {

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))

namespace {

#if defined(__x86_64__)
constexpr uint32_t kArch = AUDIT_ARCH_X86_64;
#else
constexpr uint32_t kArch = AUDIT_ARCH_AARCH64;
#endif

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "args[2] low word offset assumes little endian");

// Разрешено без условий: ввод-вывод на уже открытых дескрипторах (их
// у worker-а три), память, ожидание потоков, выход.
constexpr long kAllowed[] = {
    __NR_read,     __NR_write,         __NR_brk,          __NR_munmap, __NR_mremap,
    __NR_madvise,  __NR_futex,         __NR_exit,         __NR_exit_group,
    __NR_rt_sigreturn, __NR_rt_sigprocmask,
};

sock_filter stmt(uint16_t code, uint32_t k) { return BPF_STMT(code, k); }
sock_filter jump(uint16_t code, uint32_t k, uint8_t jt, uint8_t jf) { return BPF_JUMP(code, k, jt, jf); }

std::vector<sock_filter> program() {
    constexpr uint32_t kill = SECCOMP_RET_KILL_PROCESS;
    std::vector<sock_filter> p;
    // Чужая архитектура (например, 32-битные вызовы) — смерть.
    p.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    p.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0));
    p.push_back(stmt(BPF_RET | BPF_K, kill));
    p.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
#if defined(__x86_64__)
    // Вызовы x32 на x86-64 — тот же номер с битом __X32_SYSCALL_BIT.
    p.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
    p.push_back(stmt(BPF_RET | BPF_K, kill));
#endif
    for (long nr : kAllowed) {
        p.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 1));
        p.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    }
    // mmap и mprotect — только без PROT_EXEC: исполняемой памяти не
    // появляется. prot — третий аргумент, младшее слово.
    p.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 1, 0));
    p.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_mprotect, 0, 3));
    p.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args) + 2 * sizeof(uint64_t)));
    p.push_back(jump(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 1, 0));
    p.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    p.push_back(stmt(BPF_RET | BPF_K, kill));
    return p;
}

}  // namespace

bool lock_down(std::string& error) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        error = std::string("PR_SET_NO_NEW_PRIVS: ") + std::strerror(errno);
        return false;
    }
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        error = std::string("PR_SET_DUMPABLE: ") + std::strerror(errno);
        return false;
    }
    std::vector<sock_filter> p = program();
    sock_fprog prog{static_cast<unsigned short>(p.size()), p.data()};
    long r = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog);
    if (r != 0) {
        error = r > 0 ? "seccomp: thread " + std::to_string(r) + " cannot be synchronized"
                      : std::string("seccomp: ") + std::strerror(errno);
        return false;
    }
    return true;
}

#else

bool lock_down(std::string& error) {
    error = "the sandbox is implemented only for Linux on x86-64 and arm64";
    return false;
}

#endif

}  // namespace dp
