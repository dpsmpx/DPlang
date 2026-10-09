// `dplang build` и `dplang test`: сборка сгенерированного C с runtime
// (docs/compiler/cli.md; флаги — docs/compiler/backend_c.md, раздел 2).
#include "driver/build.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern char** environ;

namespace dp {

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

// Исполняет argv; stdout и stderr — в файлы (пусто — как у dplang).
// Результат: код выхода, 128 + сигнал или -errno, если запустить нельзя.
int spawn_wait(std::vector<std::string> argv, const std::string& out_file = "", const std::string& err_file = "") {
    std::vector<char*> cargv;
    for (std::string& a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!out_file.empty()) posix_spawn_file_actions_addopen(&fa, 1, out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (!err_file.empty()) posix_spawn_file_actions_addopen(&fa, 2, err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid = 0;
    int err = posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) return -err;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
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
        std::string tmpl = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/dplang-build-XXXXXX";
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

// Компилятор C умеет ASan и UBSan: пробная сборка пустой программы.
bool has_sanitizers(const BuildOptions& opt, TempDir& tmp) {
    std::string src = tmp.file("probe.c");
    std::string exe = tmp.file("probe");
    if (!write_all(src, "int main(void) { return 0; }\n")) return false;
    return spawn_wait({opt.cc, "-fsanitize=address,undefined", src, "-o", exe}, "/dev/null", "/dev/null") == 0;
}

// Компилирует c_code в exe. Код выхода команды: 0, 2 или 3.
int compile(const std::string& c_code, const std::string& exe, const BuildOptions& opt, TempDir& tmp) {
    std::string core = opt.runtime_dir + "/core";
    std::string platform = opt.runtime_dir + "/linux";
    if (!exists(core + "/dp_rt.h") || !exists(core + "/dp_rt.c") || !exists(platform + "/dp_entry.c")) {
        say("error: the DPlang runtime is not found in '" + opt.runtime_dir + "'; set DPLANG_RUNTIME\n");
        return 2;
    }
    if (tmp.path.empty()) return 2;
    std::string c_path = tmp.file("program.c");
    if (!write_all(c_path, c_code)) {
        say("error: cannot write '" + c_path + "'\n");
        return 2;
    }
    std::vector<std::string> argv = {opt.cc, "-std=c11", "-ffp-contract=off", "-fno-strict-aliasing", "-w"};
    if (opt.profile == Profile::Release) {
        argv.push_back("-O2");
    } else {
        argv.push_back("-O0");
        argv.push_back("-g");
    }
    if (opt.profile == Profile::Test) {
        if (has_sanitizers(opt, tmp)) {
            argv.push_back("-fsanitize=address,undefined");
            argv.push_back("-fno-sanitize-recover=all");
        } else {
            say("note: the C compiler '" + opt.cc + "' has no ASan and UBSan; tests run without them\n");
        }
    }
    for (const std::string& a : {std::string("-I"), core, c_path, core + "/dp_rt.c", platform + "/dp_entry.c",
                                  std::string("-lm"), std::string("-pthread"), std::string("-o"), exe}) {
        argv.push_back(a);
    }
    int r = spawn_wait(argv);
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

}  // namespace

int build_native(const std::string& c_code, const BuildOptions& opt) {
    TempDir tmp;
    return compile(c_code, opt.output, opt, tmp);
}

int run_tests(const std::string& c_code, const std::vector<std::string>& names, const BuildOptions& opt) {
    TempDir tmp;
    std::string exe = tmp.file("tests");
    if (!names.empty()) {
        BuildOptions o = opt;
        o.profile = Profile::Test;
        int code = compile(c_code, exe, o, tmp);
        if (code != 0) return code;
    }
    std::string out_file = tmp.file("out"), err_file = tmp.file("err");
    std::string failures;
    size_t passed = 0, failed = 0;
    for (size_t i = 0; i < names.size(); i++) {
        std::fflush(stdout);
        int r = spawn_wait({exe, std::to_string(i)}, out_file, err_file);
        if (r == 0) {
            passed++;
            say("test " + names[i] + " ... ok\n", stdout);
            continue;
        }
        failed++;
        say("test " + names[i] + " ... FAILED\n", stdout);
        std::string what = r > 128 ? "signal " + std::to_string(r - 128) : "exit " + std::to_string(r);
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
