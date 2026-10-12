// `dpl build` и `dpl test`: сборка сгенерированного C с runtime
// (docs/compiler/cli.md; флаги — docs/compiler/backend_c.md, раздел 2).
#include "driver/build.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern char** environ;

namespace dp {

const std::vector<Target>& targets() {
    static const std::vector<Target> list = {
        {"linux-x86_64", "x86_64-linux-gnu", "linux", true, true, "", {}, {"-lm", "-pthread"}, ""},
        // clang + lld поверх MinGW-w64; тесты — под Wine (docs/policy/PLATFORMS.md).
        {"windows-x86_64", "x86_64-w64-mingw32", "windows", false, false, ".exe",
         {"--target=x86_64-w64-mingw32", "-fuse-ld=lld", "-D__USE_MINGW_ANSI_STDIO=1"}, {"-lshell32"}, "wine"},
        // clang из Android NDK, API 24; порт — POSIX, как у Linux (bionic). Статическая
        // программа запускается и в эмуляторе пользовательского режима (qemu-aarch64).
        {"android-arm64", "aarch64-linux-android24", "linux", false, false, "",
         {"--target=aarch64-linux-android24", "-static"}, {"-lm"}, "qemu-aarch64"},
    };
    return list;
}

const Target* find_target(std::string_view name) {
    for (const Target& t : targets()) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

std::string target_names() {
    std::string out;
    for (const Target& t : targets()) out += (out.empty() ? "" : ", ") + std::string(t.name);
    return out;
}

std::string cc_for(const Target& t) {
    const char* env = std::getenv(t.host ? "CC" : "DPL_CROSS_CC");
    if (env && *env) return env;
    if (t.host) return "cc";
    if (t.name == "android-arm64") {
        const char* ndk = std::getenv("ANDROID_NDK_HOME");
        std::string root = ndk && *ndk ? ndk : "/opt/android-ndk";
        return root + "/toolchains/llvm/prebuilt/linux-x86_64/bin/clang";
    }
    return "clang";
}

std::string ar_for(const Target& t, const std::string& cc) {
    const char* env = std::getenv("DPL_AR");
    if (env && *env) return env;
    if (t.host) return "ar";
    // llvm-ar пишет архивы всех целей; у NDK он рядом с clang.
    size_t slash = cc.find_last_of('/');
    if (slash != std::string::npos) {
        std::string near = cc.substr(0, slash + 1) + "llvm-ar";
        struct stat st {};
        if (stat(near.c_str(), &st) == 0) return near;
    }
    return "llvm-ar";
}

std::string runner_for(const Target& t) {
    const char* env = std::getenv("DPL_RUNNER");
    if (env && *env && !t.host) return env;
    return std::string(t.runner);
}

namespace {

void say(const std::string& s, FILE* f = stderr) { std::fwrite(s.data(), 1, s.size(), f); }

bool exists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

bool write_all(const std::string& path, const std::string& text) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < text.size()) {
        ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n <= 0) {
            close(fd);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return close(fd) == 0;
}

std::string read_all(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// Исполняет argv; stdout и stderr — в файлы (пусто — как у dpl).
// Результат: код выхода, 128 + сигнал или -errno, если запустить нельзя.
// Код «не уложился в предел времени» у spawn_wait: вне 0–255 и не -errno.
constexpr int kTimedOut = 1000;

// timeout_s > 0: процесс — в своей группе; по истечении группа получает
// SIGKILL (с ней и дочерние процессы runner-а), результат — kTimedOut.
int spawn_wait(std::vector<std::string> argv, const std::string& out_file = "", const std::string& err_file = "",
               uint32_t timeout_s = 0) {
    std::vector<char*> cargv;
    for (std::string& a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!out_file.empty()) posix_spawn_file_actions_addopen(&fa, 1, out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (!err_file.empty()) posix_spawn_file_actions_addopen(&fa, 2, err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    if (timeout_s > 0) {
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
    }
    pid_t pid = 0;
    int err = posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (err != 0) return -err;
    int status = 0;
    if (timeout_s == 0) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    } else {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
        for (;;) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno != EINTR)) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                kill(-pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
                return kTimedOut;
            }
            struct timespec pause = {0, 10 * 1000 * 1000};  // 10 мс
            nanosleep(&pause, nullptr);
        }
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 255;
}

// Временный каталог сборки: сгенерированный C и программа тестов. Места
// в программе — по `#line` в исходнике .dpm, сам C после сборки не нужен.
class TempDir {
public:
    TempDir() {
        const char* tmpdir = std::getenv("TMPDIR");
        std::string tmpl = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/dpl-build-XXXXXX";
        std::vector<char> d(tmpl.begin(), tmpl.end());
        d.push_back('\0');
        if (mkdtemp(d.data())) path = d.data();
        else say("error: cannot create a temporary directory for the build\n");
    }
    ~TempDir() {
        for (const std::string& f : files) unlink(f.c_str());
        if (!path.empty()) rmdir(path.c_str());
    }
    std::string file(const std::string& name) {
        files.push_back(path + "/" + name);
        return files.back();
    }
    std::string path;
    std::vector<std::string> files;
};

const Target& target_of(const BuildOptions& opt) { return opt.target ? *opt.target : targets().front(); }

// Компилятор C умеет ASan и UBSan: пробная сборка пустой программы.
bool has_sanitizers(const BuildOptions& opt, TempDir& tmp) {
    std::string src = tmp.file("probe.c");
    std::string exe = tmp.file("probe");
    if (!write_all(src, "int main(void) { return 0; }\n")) return false;
    return spawn_wait({opt.cc, "-fsanitize=address,undefined", src, "-o", exe}, "/dev/null", "/dev/null") == 0;
}

// Инструменты чужой цели работают: пробная сборка пустой программы с её
// флагами. Иначе отказ компилятора C был бы принят за ошибку бэкенда.
bool toolchain_works(const BuildOptions& opt, TempDir& tmp) {
    const Target& t = target_of(opt);
    std::string src = tmp.file("toolchain.c");
    std::string exe = tmp.file("toolchain" + std::string(t.exe_suffix));
    if (!write_all(src, "#include <stdio.h>\nint main(void) { return 0; }\n")) return false;
    std::vector<std::string> argv = {opt.cc};
    for (std::string_view f : t.cflags) argv.emplace_back(f);
    argv.push_back(src);
    for (std::string_view l : t.libs) argv.emplace_back(l);
    argv.push_back("-o");
    argv.push_back(exe);
    return spawn_wait(argv, "/dev/null", "/dev/null") == 0;
}

// Исходники сборки: сгенерированный C и runtime. Пусто — ошибка
// окружения уже сообщена (код 2).
struct Sources {
    std::string core, c_path, rt, entry;
};

bool prepare(const std::string& c_code, const BuildOptions& opt, TempDir& tmp, Sources& s) {
    const Target& t = target_of(opt);
    s.core = opt.runtime_dir + "/core";
    std::string platform = opt.runtime_dir + "/" + std::string(t.platform);
    s.rt = s.core + "/dp_rt.c";
    s.entry = platform + "/dp_entry.c";
    if (!exists(s.core + "/dp_rt.h") || !exists(s.rt) || !exists(s.entry)) {
        say("error: the dpl runtime is not found in '" + opt.runtime_dir + "'; set DPL_RUNTIME\n");
        return false;
    }
    if (tmp.path.empty()) return false;
    s.c_path = tmp.file("program.c");
    if (!write_all(s.c_path, c_code)) {
        say("error: cannot write '" + s.c_path + "'\n");
        return false;
    }
    if (!t.host && !toolchain_works(opt, tmp)) {
        say("error: the C toolchain for target '" + std::string(t.name) + "' does not work ('" + opt.cc +
            "' with " + std::string(t.triple) + "); see docs/policy/PLATFORMS.md or set " +
            (t.name == "android-arm64" ? "ANDROID_NDK_HOME or " : "") + "DPL_CROSS_CC\n");
        return false;
    }
    return true;
}

// Флаги компилятора C для всех исходников сборки.
std::vector<std::string> c_flags(const BuildOptions& opt, TempDir& tmp) {
    const Target& t = target_of(opt);
    std::vector<std::string> argv = {opt.cc, "-std=c11", "-ffp-contract=off", "-fno-strict-aliasing", "-w"};
    for (std::string_view f : t.cflags) argv.emplace_back(f);
    if (opt.profile == Profile::Release) {
        argv.push_back("-O2");
    } else {
        argv.push_back("-O0");
        argv.push_back("-g");
    }
    if (opt.profile == Profile::Test && t.sanitizers) {
        if (has_sanitizers(opt, tmp)) {
            argv.push_back("-fsanitize=address,undefined");
            argv.push_back("-fno-sanitize-recover=all");
        } else {
            say("note: the C compiler '" + opt.cc + "' has no ASan and UBSan; tests run without them\n");
        }
    }
    return argv;
}

// Код выхода по итогу запуска компилятора C (spawn_wait): 0, 2 (не
// запустился) или 3 (отверг код).
int run_cc_result(int r, const BuildOptions& opt) {
    if (r < 0) {
        say("error: cannot run the C compiler '" + opt.cc + "': " + std::strerror(-r) + "; set CC to a C11 compiler\n");
        return 2;
    }
    if (r != 0) {
        say("internal compiler error: the C compiler '" + opt.cc + "' rejected the generated C\n");
        return 3;
    }
    return 0;
}

int run_cc(const std::vector<std::string>& argv, const BuildOptions& opt) { return run_cc_result(spawn_wait(argv), opt); }

// Библиотека `@link` компонуется для цели: пробная сборка пустой
// программы с ней. Иначе её отсутствие было бы принято за ошибку бэкенда.
bool library_links(const std::string& lib, const BuildOptions& opt, TempDir& tmp) {
    const Target& t = target_of(opt);
    std::string src = tmp.file("link.c");
    std::string exe = tmp.file("link" + std::string(t.exe_suffix));
    if (!write_all(src, "int main(void) { return 0; }\n")) return false;
    std::vector<std::string> argv = {opt.cc};
    for (std::string_view f : t.cflags) argv.emplace_back(f);
    argv.push_back(src);
    argv.push_back("-l" + lib);
    for (std::string_view l : t.libs) argv.emplace_back(l);
    argv.push_back("-o");
    argv.push_back(exe);
    return spawn_wait(argv, "/dev/null", "/dev/null") == 0;
}

// Компилирует c_code в exe. Код выхода команды: 0, 2 или 3.
int compile(const std::string& c_code, const std::string& exe, const BuildOptions& opt, TempDir& tmp) {
    const Target& t = target_of(opt);
    Sources s;
    if (!prepare(c_code, opt, tmp, s)) return 2;
    std::vector<std::string> argv = c_flags(opt, tmp);
    for (const std::string& a : {std::string("-I"), s.core, s.c_path, s.rt, s.entry}) argv.push_back(a);
    for (const std::string& l : opt.links) argv.push_back("-l" + l);
    for (std::string_view l : t.libs) argv.emplace_back(l);
    argv.push_back("-o");
    argv.push_back(exe);
    int r = spawn_wait(argv);
    if (r > 0) {
        // Нет библиотеки `@link` для цели — ошибка окружения, не бэкенда.
        for (const std::string& l : opt.links) {
            if (!library_links(l, opt, tmp)) {
                say("error: cannot link library '" + l + "' (@link) for target '" + std::string(t.name) +
                    "': install it for the target (for the host: the development package with lib" + l + ".so)\n");
                return 2;
            }
        }
    }
    return run_cc_result(r, opt);
}

// Копирует собранный файл в path (права mode с учётом umask).
// Ошибка — errno; на месте path тогда может остаться неполный файл.
int copy_program(const std::string& from, const std::string& path, mode_t mode = 0755) {
    std::string bytes = read_all(from);
    if (bytes.empty()) return EIO;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return errno;
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int e = n < 0 ? errno : EIO;
            close(fd);
            return e;
        }
        done += static_cast<size_t>(n);
    }
    if (close(fd) != 0) return errno;
    return 0;
}

}  // namespace

// Программа собирается во временный каталог и копируется в -o: так
// ошибка записи — ошибка окружения (код 2), а не отказ компилятора C, и
// неудачная сборка не портит прежний файл.
int build_native(const std::string& c_code, const BuildOptions& opt) {
    TempDir tmp;
    std::string exe = tmp.file("program" + std::string(target_of(opt).exe_suffix));
    int code = compile(c_code, exe, opt, tmp);
    if (code != 0) return code;
    if (int e = copy_program(exe, opt.output)) {
        say("error: cannot write '" + opt.output + "': " + std::strerror(e) + "\n");
        return 2;
    }
    return 0;
}

// Библиотека (Р61): каждый исходник — объектом, объекты — в архив;
// заголовок — рядом. Всё — во временном каталоге, затем копией в -o.
int build_lib(const std::string& c_code, const std::string& header, const std::string& header_path,
              const BuildOptions& opt) {
    const Target& t = target_of(opt);
    TempDir tmp;
    Sources s;
    if (!prepare(c_code, opt, tmp, s)) return 2;
    std::vector<std::string> objs;
    const std::string srcs[] = {s.c_path, s.rt, s.entry};
    for (size_t i = 0; i < 3; i++) {
        std::vector<std::string> argv = c_flags(opt, tmp);
        if (std::string_view(t.platform) != "windows") argv.push_back("-fPIC");  // архив годится и в .so хоста
        objs.push_back(tmp.file("part" + std::to_string(i) + ".o"));
        for (const std::string& a : {std::string("-I"), s.core, std::string("-c"), srcs[i], std::string("-o"), objs.back()}) {
            argv.push_back(a);
        }
        if (int code = run_cc(argv, opt)) return code;
    }
    std::string ar = ar_for(t, opt.cc);
    std::string lib = tmp.file("lib.a");
    std::vector<std::string> argv = {ar, "rcs", lib};
    argv.insert(argv.end(), objs.begin(), objs.end());
    int r = spawn_wait(argv);
    if (r != 0) {
        say("error: the archiver '" + ar + "' failed" + (r < 0 ? std::string(": ") + std::strerror(-r) : std::string()) +
            "; set DPL_AR\n");
        return 2;
    }
    std::string h = tmp.file("lib.h");
    if (!write_all(h, header)) {
        say("error: cannot write '" + h + "'\n");
        return 2;
    }
    for (const auto& [from, to] : {std::pair{lib, opt.output}, std::pair{h, header_path}}) {
        if (int e = copy_program(from, to, 0644)) {
            say("error: cannot write '" + to + "': " + std::strerror(e) + "\n");
            return 2;
        }
    }
    return 0;
}

int run_tests(const std::string& c_code, const std::vector<std::string>& names, const BuildOptions& opt) {
    TempDir tmp;
    std::string exe = tmp.file("tests" + std::string(target_of(opt).exe_suffix));
    if (!names.empty()) {
        BuildOptions o = opt;
        o.profile = Profile::Test;
        int code = compile(c_code, exe, o, tmp);
        if (code != 0) return code;
    }
    // Wine без журнала отладки: иначе его строки смешались бы с выводом теста.
    if (opt.runner == "wine") setenv("WINEDEBUG", "-all", 0);
    std::string out_file = tmp.file("out"), err_file = tmp.file("err");
    std::string failures;
    size_t passed = 0, failed = 0;
    for (size_t i = 0; i < names.size(); i++) {
        std::fflush(stdout);
        std::vector<std::string> argv = {exe, std::to_string(i)};
        if (!opt.runner.empty()) argv.insert(argv.begin(), opt.runner);
        int r = spawn_wait(argv, out_file, err_file, opt.test_timeout);
        if (r < 0) {
            say("error: cannot run the tests with '" + argv[0] + "': " + std::strerror(-r) +
                (opt.runner.empty() ? std::string() : "; set DPL_RUNNER") + "\n");
            return 2;
        }
        if (r == 0) {
            passed++;
            say("test " + names[i] + " ... ok\n", stdout);
            continue;
        }
        failed++;
        say("test " + names[i] + " ... FAILED\n", stdout);
        std::string what = r == kTimedOut ? "timed out after " + std::to_string(opt.test_timeout) + " s"
                           : r > 128  ? "signal " + std::to_string(r - 128)
                                      : "exit " + std::to_string(r);
        failures += "\n--- " + names[i] + " (" + what + ")\n";
        std::string text = read_all(out_file) + read_all(err_file);
        if (!text.empty() && text.back() != '\n') text += '\n';
        failures += text;
    }
    say(failures, stdout);
    say(std::string(names.empty() ? "" : "\n") + "test result: " + std::string(failed ? "FAILED" : "ok") + ". " +
            std::to_string(passed) + " passed; " +
            std::to_string(failed) + " failed\n",
        stdout);
    std::fflush(stdout);
    return failed ? 1 : 0;
}

}  // namespace dp
