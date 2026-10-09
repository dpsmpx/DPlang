// Фаззер «не падает» (docs/policy/TESTING.md, раздел 3).
//
//     dpfuzz lex|parse|check|contract --seed N --iters M <каталог>...
//
// Берёт файлы .dpm из каталогов как корпус (у contract — тела запросов
// .json онлайн-сервиса), портит их детерминированно
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

#include "api/compile.h"
#include "api/contract.h"
#include "ir/lower.h"
#include "ir/verify.h"
#include "sema/sema.h"
#include "support/diag.h"
#include "support/json_read.h"
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
    // JSON запросов онлайн-сервиса (contract)
    "\":\"", ",", "\\u0000", "\\ud83d\\ude00", "\\udc00", "\\\"", "null", "-0", "1e999", "{\"a\":", "[[[[",
    "\"source\":\"\"", "\"mode\":\"check\"",
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
    {
        // Комментарии — те же, кроме места: печать их не теряет и не повторяет.
        auto a = dp::comment_texts(*m, input), b = dp::comment_texts(*again, printed);
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (a != b) return fail("printed module loses or repeats comments:\n" + printed, input);
    }
    return true;
}

// Инварианты проверки: модуль без ошибок разбора проходит сбор
// объявлений и проверку тел без падения; места сообщений — внутри файла;
// модуль без ошибок понижается в Core IR, и верификатор его принимает.
// Проверка — через общий вход компиляции (compiler/api), модуль — из
// памяти: import другого модуля не находится, и проверка не идёт дальше.
bool check_sema(const std::string& input) {
    dp::SourceMap sources;
    dp::Diagnostics diags;
    dp::api::MemorySources files;
    files.add("fuzz.dpm", input);
    dp::api::CompileOptions opt;
    opt.reader = &files;
    auto prog = dp::api::check("fuzz.dpm", opt, sources, diags);
    if (!prog || !prog->load_ok) return true;
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

// Пределы как у тестового профиля tests/worker.py: маленькие, чтобы
// мутации доставали до каждой границы.
dp::api::OnlineLimits contract_limits() {
    dp::api::OnlineLimits l;
    l.request_bytes = 4096;
    l.source_bytes = 2048;
    l.lines = 60;
    l.identifier_bytes = 32;
    l.nesting = 64;
    l.diagnostics = 5;
    l.output_bytes = 8192;
    return l;
}

// Инварианты контракта v1: ответ — JSON-объект с api, compiler и ровно
// одним из result и error; результат не больше предела ответа; тот же
// запрос — те же байты.
bool check_contract(const std::string& input) {
    dp::api::OnlineLimits lim = contract_limits();
    std::string reply = dp::api::compile_v1(input, lim);
    if (dp::api::compile_v1(input, lim) != reply) return fail("reply is not deterministic", input);
    if (reply.empty() || reply.back() != '\n') return fail("reply does not end with a newline", input);
    dp::json::Value v;
    std::string err;
    if (!dp::json::parse(reply, 8, v, err)) return fail("reply is not valid JSON: " + err + "\n" + reply, input);
    if (v.kind != dp::json::Kind::Object || !v.get("api") || !v.get("compiler")) {
        return fail("reply has no api or compiler:\n" + reply, input);
    }
    bool result = v.get("result") != nullptr, error = v.get("error") != nullptr;
    if (result == error || v.keys.size() != 3) return fail("reply needs exactly one of result and error:\n" + reply, input);
    if (result && reply.size() > lim.output_bytes) return fail("result is larger than output_bytes", input);
    return true;
}

bool load_corpus(const std::vector<std::string>& dirs, const std::string& ext, std::vector<std::string>& out) {
    std::vector<std::string> paths;
    for (const std::string& d : dirs) {
        DIR* dir = opendir(d.c_str());
        if (!dir) {
            std::fprintf(stderr, "error: cannot open directory '%s'\n", d.c_str());
            return false;
        }
        while (dirent* e = readdir(dir)) {
            std::string name = e->d_name;
            // limit_memory — запрос, которому нужна память сверх RLIMIT_AS
            // worker-а: у вычисления констант нет бюджета (TODO.md), а у
            // фаззера под ASan нет предела памяти.
            if (name.rfind("limit_memory", 0) == 0) continue;
            if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
                paths.push_back(d + "/" + name);
            }
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
    bool (*check)(const std::string&) = nullptr;
    if (!args.empty()) {
        if (args[0] == "lex") check = check_lex;
        else if (args[0] == "parse") check = check_parse;
        else if (args[0] == "check") check = check_sema;
        else if (args[0] == "contract") check = check_contract;
    }
    if (args.size() < 6 || !check || args[1] != "--seed" || args[3] != "--iters") {
        std::fprintf(stderr, "usage: dpfuzz lex|parse|check|contract --seed N --iters M <dir>...\n");
        return 2;
    }
    Rng rng{std::strtoull(args[2].c_str(), nullptr, 10)};
    unsigned long long iters = std::strtoull(args[4].c_str(), nullptr, 10);
    std::vector<std::string> corpus;
    std::string ext = check == check_contract ? ".json" : ".dpm";
    if (!load_corpus(std::vector<std::string>(args.begin() + 5, args.end()), ext, corpus)) return 2;
    for (const std::string& c : corpus) {
        if (!check(c)) return 1;
    }
    for (unsigned long long i = 0; i < iters; i++) {
        if (!check(mutate(rng, corpus))) return 1;
    }
    std::printf("%s: corpus and %llu mutated inputs pass\n", args[0].c_str(), iters);
    return 0;
}
