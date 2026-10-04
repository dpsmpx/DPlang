// Фаззер «не падает» (docs/policy/TESTING.md, раздел 3).
//
//     dpfuzz lex|parse|check --seed N --iters M <каталог>...
//
// Берёт файлы .dpm из каталогов как корпус, портит их детерминированно
// (зерно задано) и проверяет инварианты прохода на каждом входе. Сбой
// инварианта — сообщение и код 1; падение ловят санитайзеры сборки.
// Вывод при успехе — одна строка без чисел, зависящих от времени.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <string_view>
#include <vector>

#include "ir/lower.h"
#include "ir/verify.h"
#include "sema/sema.h"
#include "support/diag.h"
#include "support/source.h"
#include "support/utf8.h"
#include "syntax/ast_dump.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"
#include "syntax/printer.h"

namespace {

// splitmix64: детерминированный ГПСЧ без состояния вне объекта.
struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
};

// Куски, которые чаще всего ломают лексер и парсер.
const char* const kPieces[] = {
    "\"", "'", "\\", "\n", "\r", "\r\n", "\t", " ", "//", "/", "0x", "0b", "_", ".", "..", "e", "1e", "{", "}",
    "(", ")", "[", "]", "fn ", "struct ", "enum ", "let ", "var ", "if ", "else ", "match ", "=>", "&mut ",
    "\xff", "\xc3", "\xe2\x82", "\xf0\x9f\x98\x80", "\xd0\xb6", "\\u{", "\\x", "+%", "<<=", "??", "@", "#",
    "unsafe", "none", "true", "18446744073709551616", "9223372036854775808", "1.5e", "\\u{D800}", "\0",
};

std::string mutate(Rng& rng, const std::vector<std::string>& corpus) {
    std::string s = corpus[rng.below(corpus.size())];
    size_t rounds = 1 + rng.below(4);
    for (size_t r = 0; r < rounds; r++) {
        size_t pos = rng.below(s.size() + 1);
        switch (rng.below(6)) {
            case 0:  // заменить байт
                if (!s.empty()) s[rng.below(s.size())] = static_cast<char>(rng.below(256));
                break;
            case 1:  // удалить участок
                if (!s.empty()) s.erase(pos, 1 + rng.below(8));
                break;
            case 2: {  // вставить кусок
                const char* p = kPieces[rng.below(sizeof kPieces / sizeof kPieces[0])];
                s.insert(pos, std::string(p, p[0] ? std::char_traits<char>::length(p) : 1));
                break;
            }
            case 3:  // обрезать
                s.resize(pos);
                break;
            case 4: {  // вклеить кусок другого файла
                const std::string& o = corpus[rng.below(corpus.size())];
                size_t a = rng.below(o.size() + 1);
                s.insert(pos, o.substr(a, rng.below(64)));
                break;
            }
            default: {  // повторить участок
                size_t len = rng.below(16);
                s.insert(pos, s.substr(pos, len));
                break;
            }
        }
    }
    return s;
}

bool fail(const std::string& what, const std::string& input) {
    std::fprintf(stderr, "invariant failed: %s\ninput (%zu bytes):\n", what.c_str(), input.size());
    std::fwrite(input.data(), 1, input.size(), stderr);
    std::fputc('\n', stderr);
    return false;
}

// Инварианты лексера: токены покрывают вход без пропусков, Eof в конце,
// пустых токенов нет; места сообщений — внутри файла; вывод строится.
bool check_lex(const std::string& input) {
    dp::SourceMap sources;
    dp::FileId id = sources.add("fuzz.dpm", input);
    dp::Diagnostics diags;
    std::vector<dp::Token> toks = dp::lex(sources.file(id), id, diags);
    uint32_t pos = 0;
    for (size_t i = 0; i < toks.size(); i++) {
        const dp::Token& t = toks[i];
        if (t.begin != pos) return fail("tokens are not contiguous", input);
        bool last = i + 1 == toks.size();
        if (last != (t.kind == dp::TokKind::Eof)) return fail("Eof is not the last token", input);
        if (!last && t.end <= t.begin) return fail("empty token", input);
        pos = t.end;
    }
    if (pos != input.size()) return fail("tokens do not reach the end of input", input);
    for (const dp::Diagnostic& d : diags.list()) {
        if (d.has_span && (d.span.begin > d.span.end || d.span.end > input.size())) {
            return fail("diagnostic span outside the file", input);
        }
    }
    std::string text = dp::render_text(diags.list(), sources);
    std::string json = dp::render_json(diags.list(), sources);
    if (dp::sanitize_utf8(text) != text || dp::sanitize_utf8(json) != json) {
        return fail("diagnostic output is not valid UTF-8", input);
    }
    return true;
}

// Инварианты парсера: не падает; участки объявлений — внутри файла; у
// входа без ошибок — круг: печать разбирается без ошибок в тот же дамп,
// печать печати не меняется.
bool check_parse(const std::string& input) {
    dp::SourceMap sources;
    dp::FileId id = sources.add("fuzz.dpm", input);
    dp::Diagnostics diags;
    auto m = dp::parse_module(sources.file(id), id, "fuzz", diags);
    for (const dp::ast::Item* it : m->items) {
        if (it->span.begin > it->span.end || it->span.end > input.size()) return fail("item span outside the file", input);
    }
    for (const dp::Diagnostic& d : diags.list()) {
        if (d.has_span && (d.span.begin > d.span.end || d.span.end > input.size())) {
            return fail("diagnostic span outside the file", input);
        }
    }
    std::string dump = dp::dump_module(*m);
    if (diags.has_errors()) return true;
    std::string printed = dp::print_module(*m, input);
    dp::FileId id2 = sources.add("printed.dpm", printed);
    dp::Diagnostics diags2;
    auto again = dp::parse_module(sources.file(id2), id2, "fuzz", diags2);
    if (diags2.has_errors()) return fail("printed module does not parse:\n" + printed, input);
    if (dp::dump_module(*again) != dump) return fail("printed module parses differently:\n" + printed, input);
    if (dp::print_module(*again, printed) != printed) return fail("printing is not idempotent", input);
    return true;
}

// Инварианты проверки: модуль без ошибок разбора проходит сбор
// объявлений и проверку тел без падения; места сообщений — внутри файла;
// модуль без ошибок понижается в Core IR, и верификатор его принимает.
// Модули с import пропускаются: фаззер проверяет один файл.
bool check_sema(const std::string& input) {
    if (input.find("import") != std::string::npos) return true;
    dp::SourceMap sources;
    dp::Diagnostics diags;
    auto prog = std::make_unique<dp::sema::Program>();
    auto m = std::make_unique<dp::sema::ModuleInfo>();
    m->name = "fuzz";
    m->path = "fuzz.dpm";
    m->file = sources.add("fuzz.dpm", input);
    m->ast = dp::parse_module(sources.file(m->file), m->file, "fuzz", diags);
    if (diags.has_errors()) return true;
    prog->root = m.get();
    prog->modules.push_back(std::move(m));
    dp::sema::collect(*prog, diags);
    dp::sema::check(*prog, diags);
    for (const dp::Diagnostic& d : diags.list()) {
        if (d.has_span && (d.span.begin > d.span.end || d.span.end > input.size())) {
            return fail("diagnostic span outside the file", input);
        }
    }
    dp::render_text(diags.list(), sources);
    if (diags.has_errors()) return true;
    auto ir = dp::ir::lower(*prog);
    std::vector<std::string> errors = dp::ir::verify(*ir);
    if (!errors.empty()) return fail("invalid Core IR: " + errors[0], input);
    return true;
}

bool load_corpus(const std::vector<std::string>& dirs, std::vector<std::string>& out) {
    std::vector<std::string> paths;
    for (const std::string& d : dirs) {
        DIR* dir = opendir(d.c_str());
        if (!dir) {
            std::fprintf(stderr, "error: cannot open directory '%s'\n", d.c_str());
            return false;
        }
        while (dirent* e = readdir(dir)) {
            std::string name = e->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dpm") == 0) paths.push_back(d + "/" + name);
        }
        closedir(dir);
    }
    std::sort(paths.begin(), paths.end());
    for (const std::string& p : paths) {
        std::string text;
        if (dp::read_file(p, text)) out.push_back(std::move(text));
    }
    if (out.empty()) {
        std::fprintf(stderr, "error: empty corpus\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() < 6 || (args[0] != "lex" && args[0] != "parse" && args[0] != "check") || args[1] != "--seed" ||
        args[3] != "--iters") {
        std::fprintf(stderr, "usage: dpfuzz lex|parse|check --seed N --iters M <dir>...\n");
        return 2;
    }
    bool (*check)(const std::string&) = args[0] == "lex" ? check_lex : args[0] == "parse" ? check_parse : check_sema;
    Rng rng{std::strtoull(args[2].c_str(), nullptr, 10)};
    unsigned long long iters = std::strtoull(args[4].c_str(), nullptr, 10);
    std::vector<std::string> corpus;
    if (!load_corpus(std::vector<std::string>(args.begin() + 5, args.end()), corpus)) return 2;
    for (const std::string& c : corpus) {
        if (!check(c)) return 1;
    }
    for (unsigned long long i = 0; i < iters; i++) {
        if (!check(mutate(rng, corpus))) return 1;
    }
    std::printf("%s: corpus and %llu mutated inputs pass\n", args[0].c_str(), iters);
    return 0;
}
