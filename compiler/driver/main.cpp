// Точка входа CLI `dplang` (Р15). Команды появляются по шагам v0.1
// (ROADMAP/IMPLEMENTATION.md); устройство — docs/compiler/cli.md.
//
// Коды выхода: 0 — успех; 1 — в программе есть ошибки (диагностика);
// 2 — неверный вызов CLI; 3 — внутренняя ошибка компилятора.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "api/compile.h"
#include "backend_c/emit_c.h"
#include "driver/big_stack.h"
#include "ir/dump.h"
#include "ir/interp.h"
#include "ir/lower.h"
#include "ir/verify.h"
#include "sema/sema.h"
#include "support/diag.h"
#include "support/json.h"
#include "support/version.h"
#include "syntax/ast_dump.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"
#include "syntax/printer.h"

namespace {


constexpr std::string_view kUsage =
    "usage: dplang <command> [arguments]\n"
    "\n"
    "commands:\n"
    "  check [--format=json] <file.dpm>\n"
    "                   check a program and the modules it imports\n"
    "  explain <code>   explain a diagnostic code, e.g. E0101\n"
    "  help             show this help\n"
    "  --version        print the compiler version\n"
    "\n"
    "debugging commands (output format is not stable):\n"
    "  debug lex <file.dpm>     print the tokens of a file\n"
    "  debug parse <file.dpm>   print the syntax tree of a file\n"
    "  debug print <file.dpm>   print a file in canonical form\n"
    "  debug summary <file.dpm> print the interface summaries of a program\n"
    "  debug types <file.dpm>   print the syntax tree of the main module with types\n"
    "  debug ir <file.dpm>      print the Core IR of a program\n"
    "  debug run <file.dpm>     run a program in the reference interpreter\n"
    "  debug c <file.dpm>       print the C code of a program (C backend)\n";

void put(std::string_view s, FILE* f = stdout) { std::fwrite(s.data(), 1, s.size(), f); }

int usage_error(std::string_view message) {
    put("error: ", stderr);
    put(message, stderr);
    put("\nrun 'dplang help' to list the commands\n", stderr);
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

// Имя модуля — имя файла без каталога и расширения.
std::string module_name(std::string_view path) {
    size_t slash = path.find_last_of('/');
    std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    size_t dot = base.rfind('.');
    return std::string(dot == std::string_view::npos ? base : base.substr(0, dot));
}

// debug parse: дамп AST. Если ошибок нет — ещё и проверка круга: печать,
// повторный разбор, тот же дамп, и печать печати не меняется.
int cmd_debug_parse(const std::vector<std::string_view>& args, bool print) {
    if (args.size() != 1) return usage_error("debug parse and debug print take one file");
    dp::SourceMap sources;
    dp::Diagnostics diags;
    dp::FileId id;
    if (!load(args[0], sources, diags, id)) return finish(diags, sources);
    std::string name = module_name(args[0]);
    auto m = dp::parse_module(sources.file(id), id, name, diags);  // предел вложенности — по умолчанию
    std::string dump = dp::dump_module(*m);
    if (!diags.has_errors()) {
        std::string printed = dp::print_module(*m, sources.file(id).text());
        dp::Diagnostics again_diags;
        dp::FileId id2 = sources.add("<printed>", printed);
        auto again = dp::parse_module(sources.file(id2), id2, name, again_diags);
        if (again_diags.has_errors()) {
            return internal_error("printed module does not parse:\n" + dp::render_text(again_diags.list(), sources) +
                                  printed);
        }
        if (dp::dump_module(*again) != dump) return internal_error("printed module parses differently:\n" + printed);
        if (dp::print_module(*again, printed) != printed) return internal_error("printing is not idempotent");
        if (print) {
            put(printed);
            return 0;
        }
    }
    if (!print) put(dump);
    return finish(diags, sources);
}

// Каталог стандартной библиотеки: $DPLANG_STDLIB или stdlib/ репозитория
// рядом с каталогом сборки (build/compiler/dplang → stdlib).
std::string stdlib_dir(const char* argv0) {
    if (const char* env = std::getenv("DPLANG_STDLIB")) return env;
    std::string exe = argv0 ? argv0 : "";
    size_t slash = exe.find_last_of('/');
    std::string dir = slash == std::string::npos ? "." : exe.substr(0, slash);
    return dir + "/../../stdlib";
}

// Окружение запуска, общее для команд.
struct Cli {
    std::string stdlib;
};

// Загрузка и проверка программы. nullptr — главный модуль не прочитан
// или в программе ошибки разбора (дальше не проверяем: сообщения были бы
// следствием).
// Общий вход компиляции (compiler/api) — тот же, что у worker онлайн-сервиса.
std::unique_ptr<dp::sema::Program> check_program(const Cli& cli, std::string_view path, dp::SourceMap& sources,
                                                 dp::Diagnostics& diags) {
    dp::api::CompileOptions opt;
    opt.stdlib_dir = cli.stdlib;
    return dp::api::check(std::string(path), opt, sources, diags);
}

// check: диагностика текстом в stderr или (--format=json) документом JSON в stdout.
int cmd_check(const Cli& cli, const std::vector<std::string_view>& args) {
    bool json = false;
    std::vector<std::string_view> files;
    for (std::string_view a : args) {
        if (a == "--format=json") json = true;
        else if (a == "--format=text") json = false;
        else if (a.substr(0, 2) == "--") return usage_error("unknown option '" + std::string(a) + "'");
        else files.push_back(a);
    }
    if (files.size() != 1) return usage_error("check takes one file");
    dp::SourceMap sources;
    dp::Diagnostics diags;
    check_program(cli, files[0], sources, diags);
    if (json) {
        put(dp::render_json(diags.list(), sources));
        return diags.has_errors() ? 1 : 0;
    }
    return finish(diags, sources);
}

int cmd_debug_summary(const Cli& cli, const std::vector<std::string_view>& args) {
    if (args.size() != 1) return usage_error("debug summary takes one file");
    dp::SourceMap sources;
    dp::Diagnostics diags;
    auto prog = check_program(cli, args[0], sources, diags);
    if (prog && !diags.has_errors()) {
        for (auto& m : prog->modules) put(dp::sema::summary_text(*m));
    }
    return finish(diags, sources);
}

// debug types: дамп AST главного модуля, у выражений — тип и неявное преобразование.
int cmd_debug_types(const Cli& cli, const std::vector<std::string_view>& args) {
    if (args.size() != 1) return usage_error("debug types takes one file");
    dp::SourceMap sources;
    dp::Diagnostics diags;
    auto prog = check_program(cli, args[0], sources, diags);
    if (prog && prog->load_ok) {
        const dp::sema::ModuleInfo* root = prog->root;
        put(dp::dump_module(*root->ast, [root](const dp::ast::Expr* e) {
            if (!e->type) return std::string();  // вызываемое имя: не значение
            std::string s = " : " + dp::sema::type_name(e->type, root);
            if (e->conv) s += " -> " + dp::sema::type_name(e->conv, root);
            return s;
        }));
    }
    return finish(diags, sources);
}

// Понижение проверенной программы в Core IR и верификация. nullptr —
// IR нарушает инварианты (внутренняя ошибка, сообщение уже напечатано).
std::unique_ptr<dp::ir::Program> lower_program(dp::sema::Program& prog, const dp::SourceMap& sources) {
    auto ir = dp::ir::lower(prog);
    std::vector<std::string> errors = dp::ir::verify(*ir);
    if (errors.empty()) return ir;
    std::string msg = "invalid Core IR:\n";
    for (const std::string& e : errors) msg += "  " + e + "\n";
    msg += dp::ir::dump_program(*ir, sources);
    internal_error(msg);
    return nullptr;
}

// debug ir: Core IR всех функций программы (после верификатора).
int cmd_debug_ir(const Cli& cli, const std::vector<std::string_view>& args) {
    if (args.size() != 1) return usage_error("debug ir takes one file");
    dp::SourceMap sources;
    dp::Diagnostics diags;
    auto prog = check_program(cli, args[0], sources, diags);
    if (!prog || diags.has_errors()) return finish(diags, sources);
    auto ir = lower_program(*prog, sources);
    if (!ir) return 3;
    put(dp::ir::dump_program(*ir, sources));
    return finish(diags, sources);
}

// debug run: исполнение программы эталонным интерпретатором (Р43).
// Код выхода — код программы (docs/ir/README.md, «Интерпретатор»).
int cmd_debug_run(const Cli& cli, const std::vector<std::string_view>& args) {
    if (args.size() != 1) return usage_error("debug run takes one file");
    dp::SourceMap sources;
    dp::Diagnostics diags;
    auto prog = check_program(cli, args[0], sources, diags);
    if (prog && !diags.has_errors() && !dp::sema::check_main(*prog, diags)) return finish(diags, sources);
    if (!prog || diags.has_errors()) return finish(diags, sources);
    auto ir = lower_program(*prog, sources);
    if (!ir) return 3;
    std::fflush(stdout);
    dp::ir::RunResult r = dp::ir::run(*ir, sources);
    std::fflush(stdout);
    if (r.internal) return internal_error(r.message);
    return r.exit_code;
}

// debug c: C-код программы от C-бэкенда (docs/compiler/backend_c.md).
int cmd_debug_c(const Cli& cli, const std::vector<std::string_view>& args) {
    if (args.size() != 1) return usage_error("debug c takes one file");
    dp::SourceMap sources;
    dp::Diagnostics diags;
    auto prog = check_program(cli, args[0], sources, diags);
    if (prog && !diags.has_errors() && !dp::sema::check_main(*prog, diags)) return finish(diags, sources);
    if (!prog || diags.has_errors()) return finish(diags, sources);
    auto ir = lower_program(*prog, sources);
    if (!ir) return 3;
    dp::backend_c::CResult c = dp::backend_c::emit_c(*ir, sources);
    if (!c.errors.empty()) return internal_error("C backend: " + c.errors.front());
    put(c.code);
    return 0;
}

int cmd_debug(const Cli& cli, const std::vector<std::string_view>& args) {
    if (args.empty()) return usage_error("debug needs a subcommand");
    std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (args[0] == "lex") return cmd_debug_lex(rest);
    if (args[0] == "parse") return cmd_debug_parse(rest, false);
    if (args[0] == "print") return cmd_debug_parse(rest, true);
    if (args[0] == "summary") return cmd_debug_summary(cli, rest);
    if (args[0] == "types") return cmd_debug_types(cli, rest);
    if (args[0] == "ir") return cmd_debug_ir(cli, rest);
    if (args[0] == "run") return cmd_debug_run(cli, rest);
    if (args[0] == "c") return cmd_debug_c(cli, rest);
    return usage_error("unknown debug subcommand '" + std::string(args[0]) + "'");
}

}  // namespace

static int run_cli(int argc, char** argv) {
    Cli cli;
    cli.stdlib = stdlib_dir(argv[0]);
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) return usage_error("no command given");
    std::string_view cmd = args[0];
    std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        put(kUsage);
        return 0;
    }
    if (cmd == "--version") {
        put("dplang ");
        put(dp::kCompilerVersion);
        put("\n");
        return 0;
    }
    if (cmd == "explain") return cmd_explain(rest);
    if (cmd == "check") return cmd_check(cli, rest);
    if (cmd == "debug") return cmd_debug(cli, rest);
    return usage_error("unknown command '" + std::string(cmd) + "'");
}

// Команда исполняется в потоке с большим стеком: проходы компилятора
// рекурсивны, а глубину дерева ограничивает только предел вложенности
// (docs/compiler/README.md, «Предел вложенности»).
int main(int argc, char** argv) {
    struct Args {
        int argc;
        char** argv;
        int code;
    } a{argc, argv, 0};
    dp::run_with_stack(dp::kCompilerStackBytes, [](void* p) {
        auto* x = static_cast<Args*>(p);
        x->code = run_cli(x->argc, x->argv);
    }, &a);
    return a.code;
}
