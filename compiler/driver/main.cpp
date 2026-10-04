// Точка входа CLI `dplang` (Р15). Команды появляются по шагам v0.1
// (ROADMAP/IMPLEMENTATION.md); устройство — docs/compiler/cli.md.
//
// Коды выхода: 0 — успех; 1 — в программе есть ошибки (диагностика);
// 2 — неверный вызов CLI; 3 — внутренняя ошибка компилятора.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "support/diag.h"
#include "support/json.h"
#include "syntax/lexer.h"

namespace {

constexpr std::string_view kVersion = "dplang 0.1.0-dev";

constexpr std::string_view kUsage =
    "usage: dplang <command> [arguments]\n"
    "\n"
    "commands:\n"
    "  explain <code>   explain a diagnostic code, e.g. E0101\n"
    "  help             show this help\n"
    "  --version        print the compiler version\n"
    "\n"
    "debugging commands (output format is not stable):\n"
    "  debug lex <file.dpm>   print the tokens of a file\n";

void put(std::string_view s, FILE* f = stdout) { std::fwrite(s.data(), 1, s.size(), f); }

int usage_error(std::string_view message) {
    put("error: ", stderr);
    put(message, stderr);
    put("\n\n", stderr);
    put(kUsage, stderr);
    return 2;
}

int internal_error(std::string_view message) {
    put("internal compiler error: ", stderr);
    put(message, stderr);
    put("\n", stderr);
    return 3;
}

// Загружает файл в sources; при ошибке — диагностика E0001.
bool load(std::string_view path, dp::SourceMap& sources, dp::Diagnostics& diags, dp::FileId& id) {
    std::string text;
    if (!dp::read_file(std::string(path), text)) {
        diags.error(dp::Code::E0001, "cannot read source file '" + std::string(path) + "'");
        return false;
    }
    id = sources.add(std::string(path), std::move(text));
    return true;
}

int finish(const dp::Diagnostics& diags, const dp::SourceMap& sources) {
    put(dp::render_text(diags.list(), sources), stderr);
    return diags.has_errors() ? 1 : 0;
}

int cmd_explain(const std::vector<std::string_view>& args) {
    if (args.size() != 1) return usage_error("explain takes one diagnostic code");
    dp::Code code;
    if (!dp::find_code(args[0], code)) {
        put("error: unknown diagnostic code '", stderr);
        put(args[0], stderr);
        put("'\n", stderr);
        return 2;
    }
    put(dp::code_name(code));
    put(": ");
    put(dp::code_title(code));
    put("\n\n");
    put(dp::code_text(code));
    put("\n");
    return 0;
}

// debug lex: токены без пробелов, по строке на токен: «строка:столбец Вид "текст"».
int cmd_debug_lex(const std::vector<std::string_view>& args) {
    if (args.size() != 1) return usage_error("debug lex takes one file");
    dp::SourceMap sources;
    dp::Diagnostics diags;
    dp::FileId id;
    if (!load(args[0], sources, diags, id)) return finish(diags, sources);
    const dp::SourceFile& f = sources.file(id);
    std::vector<dp::Token> toks = dp::lex(f, id, diags);
    // Токены покрывают файл без пропусков и наложений.
    uint32_t pos = 0;
    for (const dp::Token& t : toks) {
        if (t.begin != pos || t.end < t.begin) return internal_error("lexer tokens do not cover the file");
        pos = t.end;
    }
    if (pos != f.text().size() || toks.empty() || toks.back().kind != dp::TokKind::Eof) {
        return internal_error("lexer tokens do not cover the file");
    }
    std::string out;
    for (const dp::Token& t : toks) {
        if (t.kind == dp::TokKind::Whitespace) continue;
        dp::LineCol lc = f.line_col(t.begin);
        out += std::to_string(lc.line) + ":" + std::to_string(lc.col) + " " + std::string(dp::tok_name(t.kind)) + " ";
        dp::json_string(out, std::string_view(f.text()).substr(t.begin, t.end - t.begin));
        out += '\n';
    }
    put(out);
    return finish(diags, sources);
}

int cmd_debug(const std::vector<std::string_view>& args) {
    if (args.empty()) return usage_error("debug needs a subcommand");
    std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (args[0] == "lex") return cmd_debug_lex(rest);
    return usage_error("unknown debug subcommand '" + std::string(args[0]) + "'");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) return usage_error("no command given");
    std::string_view cmd = args[0];
    std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        put(kUsage);
        return 0;
    }
    if (cmd == "--version") {
        put(kVersion);
        put("\n");
        return 0;
    }
    if (cmd == "explain") return cmd_explain(rest);
    if (cmd == "debug") return cmd_debug(rest);
    return usage_error("unknown command '" + std::string(cmd) + "'");
}
