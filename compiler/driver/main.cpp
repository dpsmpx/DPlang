// Точка входа CLI `dplang` (Р15). Команды появляются по шагам v0.1
// (ROADMAP/IMPLEMENTATION.md); устройство — docs/compiler/cli.md.
//
// Коды выхода: 0 — успех; 1 — в программе есть ошибки (диагностика);
// 2 — неверный вызов CLI.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "support/diag.h"

namespace {

constexpr std::string_view kVersion = "dplang 0.1.0-dev";

constexpr std::string_view kUsage =
    "usage: dplang <command> [arguments]\n"
    "\n"
    "commands:\n"
    "  explain <code>   explain a diagnostic code, e.g. E0001\n"
    "  help             show this help\n"
    "  --version        print the compiler version\n";

int usage_error(std::string_view message) {
    std::fprintf(stderr, "error: %.*s\n\n%.*s", static_cast<int>(message.size()), message.data(),
                 static_cast<int>(kUsage.size()), kUsage.data());
    return 2;
}

int cmd_explain(const std::vector<std::string_view>& args) {
    if (args.size() != 1) return usage_error("explain takes one diagnostic code");
    dp::Code code;
    if (!dp::find_code(args[0], code)) {
        std::fprintf(stderr, "error: unknown diagnostic code '%.*s'\n", static_cast<int>(args[0].size()),
                     args[0].data());
        return 2;
    }
    std::string_view name = dp::code_name(code), title = dp::code_title(code), text = dp::code_text(code);
    std::printf("%.*s: %.*s\n\n%.*s\n", static_cast<int>(name.size()), name.data(), static_cast<int>(title.size()),
                title.data(), static_cast<int>(text.size()), text.data());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) return usage_error("no command given");
    std::string_view cmd = args[0];
    std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        std::fwrite(kUsage.data(), 1, kUsage.size(), stdout);
        return 0;
    }
    if (cmd == "--version") {
        std::printf("%.*s\n", static_cast<int>(kVersion.size()), kVersion.data());
        return 0;
    }
    if (cmd == "explain") return cmd_explain(rest);
    return usage_error("unknown command '" + std::string(cmd) + "'");
}
