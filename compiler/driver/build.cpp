// `dplang build`: сборка сгенерированного C с runtime (docs/compiler/cli.md,
// «build»; флаги — docs/compiler/backend_c.md, раздел 2).
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
#include <vector>

extern char** environ;

namespace dp {

namespace {

void say(const std::string& s) { std::fwrite(s.data(), 1, s.size(), stderr); }

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

}  // namespace

int build_native(const std::string& c_code, const BuildOptions& opt) {
    std::string core = opt.runtime_dir + "/core";
    std::string platform = opt.runtime_dir + "/linux";
    if (!exists(core + "/dp_rt.h") || !exists(core + "/dp_rt.c") || !exists(platform + "/dp_entry.c")) {
        say("error: the DPlang runtime is not found in '" + opt.runtime_dir + "'; set DPLANG_RUNTIME\n");
        return 2;
    }
    // Сгенерированный C — во временном каталоге: места в программе — по
    // `#line` в исходнике .dpm, сам файл C после сборки не нужен.
    const char* tmpdir = std::getenv("TMPDIR");
    std::string tmpl = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/dplang-build-XXXXXX";
    std::vector<char> dir(tmpl.begin(), tmpl.end());
    dir.push_back('\0');
    if (!mkdtemp(dir.data())) {
        say("error: cannot create a temporary directory in '" + tmpl.substr(0, tmpl.rfind('/')) + "'\n");
        return 2;
    }
    std::string c_path = std::string(dir.data()) + "/program.c";
    int code = 0;
    if (!write_all(c_path, c_code)) {
        say("error: cannot write '" + c_path + "'\n");
        code = 2;
    } else {
        std::vector<std::string> argv = {opt.cc, "-std=c11", "-ffp-contract=off", "-fno-strict-aliasing", "-w"};
        if (opt.release) {
            argv.push_back("-O2");
        } else {
            argv.push_back("-O0");
            argv.push_back("-g");
        }
        argv.push_back("-I");
        argv.push_back(core);
        argv.push_back(c_path);
        argv.push_back(core + "/dp_rt.c");
        argv.push_back(platform + "/dp_entry.c");
        argv.push_back("-lm");
        argv.push_back("-pthread");
        argv.push_back("-o");
        argv.push_back(opt.output);
        std::vector<char*> cargv;
        for (std::string& a : argv) cargv.push_back(a.data());
        cargv.push_back(nullptr);
        pid_t pid = 0;
        int err = posix_spawnp(&pid, opt.cc.c_str(), nullptr, nullptr, cargv.data(), environ);
        if (err != 0) {
            say("error: cannot run the C compiler '" + opt.cc + "': " + std::strerror(err) +
                "; set CC to a C11 compiler\n");
            code = 2;
        } else {
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                say("internal compiler error: the C compiler '" + opt.cc + "' rejected the generated C\n");
                code = 3;
            }
        }
    }
    unlink(c_path.c_str());
    rmdir(dir.data());
    return code;
}

}  // namespace dp
