// Контракт онлайн-сервиса v1 (contract.h).
#include "api/contract.h"

#include <algorithm>

#include "api/compile.h"
#include "support/json.h"
#include "support/json_read.h"
#include "support/version.h"
#include "syntax/lexer.h"

namespace dp::api {

namespace {

// Запрос — плоский объект строк; глубже — не запрос.
constexpr uint32_t kRequestDepth = 4;
constexpr std::string_view kFields[] = {"language", "mode", "file_name", "source"};

void head(std::string& out) {
    out += "{\"api\":";
    json_string(out, kApiVersion);
    out += ",\"compiler\":{\"version\":";
    json_string(out, kCompilerVersion);
    out += ",\"language\":";
    json_string(out, kLanguageVersion);
    out += '}';
}

// Значение из запроса в сообщении — не длиннее 64 байт, по границе символа.
std::string excerpt(std::string_view s) {
    constexpr size_t kMax = 64;
    if (s.size() <= kMax) return std::string(s);
    size_t n = kMax;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return std::string(s.substr(0, n)) + "...";
}

std::string limit_error(std::string_view what, std::string_view limit, uint64_t max) {
    return error_v1("limit_exceeded", std::string(what) + " exceeds the limit of " + std::to_string(max), limit,
                    max);
}

// Имя файла — имя модуля и `.dpm`: буква или `_`, затем буквы, цифры, `_`.
bool valid_file_name(std::string_view name, uint32_t max_stem) {
    constexpr std::string_view ext = ".dpm";
    if (name.size() <= ext.size() || name.substr(name.size() - ext.size()) != ext) return false;
    std::string_view stem = name.substr(0, name.size() - ext.size());
    if (stem.size() > max_stem) return false;
    for (size_t i = 0; i < stem.size(); i++) {
        char c = stem[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return false;
    }
    return true;
}

// Строки: переводы строки и последняя строка без него; пустой текст — 0.
uint64_t count_lines(std::string_view s) {
    uint64_t n = static_cast<uint64_t>(std::count(s.begin(), s.end(), '\n'));
    if (!s.empty() && s.back() != '\n') n++;
    return n;
}

// Самое длинное имя (токен Ident) по лексеру языка.
uint64_t longest_identifier(const std::string& file_name, const std::string& source) {
    SourceFile f(file_name, source);
    Diagnostics ignore;
    uint64_t best = 0;
    for (const Token& t : lex(f, 0, ignore)) {
        if (t.kind == TokKind::Ident) best = std::max<uint64_t>(best, t.end - t.begin);
    }
    return best;
}

// Результат проверки: сообщения по порядку, пока их число и размер ответа
// в пределах; остальные отбрасываются, total — сколько было всего.
std::string result(const Diagnostics& diags, const SourceMap& sources, const OnlineLimits& lim) {
    std::vector<Diagnostic> list = sorted(diags.list());
    std::string out;
    head(out);
    out += ",\"result\":{\"success\":";
    out += diags.has_errors() ? "false" : "true";
    out += ",\"diagnostics\":[";
    std::string tail_max = "],\"truncated\":true,\"total\":" + std::to_string(list.size()) + "}}\n";
    size_t budget = lim.output_bytes > out.size() + tail_max.size() ? lim.output_bytes - out.size() - tail_max.size()
                                                                    : 0;
    size_t used = 0, shown = 0;
    std::string item;
    for (const Diagnostic& d : list) {
        if (shown >= lim.diagnostics) break;
        item.clear();
        if (shown) item += ',';
        json_diagnostic(item, d, sources);
        if (used + item.size() > budget) break;
        out += item;
        used += item.size();
        shown++;
    }
    out += "],\"truncated\":";
    out += shown < list.size() ? "true" : "false";
    out += ",\"total\":" + std::to_string(list.size()) + "}}\n";
    return out;
}

}  // namespace

std::string error_v1(std::string_view code, std::string_view message, std::string_view limit, uint64_t max) {
    std::string out;
    head(out);
    out += ",\"error\":{\"code\":";
    json_string(out, code);
    out += ",\"message\":";
    json_string(out, message);
    if (!limit.empty()) {
        out += ",\"limit\":{\"name\":";
        json_string(out, limit);
        out += ",\"max\":" + std::to_string(max) + "}";
    }
    out += "}}\n";
    return out;
}

std::string describe_v1() {
    std::string out;
    head(out);
    out += "}\n";
    return out;
}

std::string compile_v1(std::string_view request, const OnlineLimits& lim) {
    if (request.size() > lim.request_bytes) return limit_error("request body", "request_bytes", lim.request_bytes);
    json::Value req;
    std::string err;
    if (!json::parse(request, kRequestDepth, req, err)) return error_v1("invalid_request", err);
    if (req.kind != json::Kind::Object) return error_v1("invalid_request", "request must be a JSON object");
    for (const std::string& k : req.keys) {
        if (std::find(std::begin(kFields), std::end(kFields), k) == std::end(kFields)) {
            return error_v1("invalid_request", "unknown field '" + excerpt(k) + "'");
        }
    }
    for (std::string_view k : kFields) {
        const json::Value* v = req.get(k);
        if (!v) return error_v1("invalid_request", "missing field '" + std::string(k) + "'");
        if (v->kind != json::Kind::String) {
            return error_v1("invalid_request", "field '" + std::string(k) + "' must be a string");
        }
    }
    const std::string& language = req.get("language")->text;
    const std::string& mode = req.get("mode")->text;
    const std::string& file_name = req.get("file_name")->text;
    const std::string& source = req.get("source")->text;
    if (language != kLanguageVersion) {
        return error_v1("unsupported_language", "language version '" + excerpt(language) +
                                                    "' is not supported; this compiler accepts '" +
                                                    std::string(kLanguageVersion) + "'");
    }
    if (mode != "check") {
        return error_v1("invalid_request", "mode '" + excerpt(mode) + "' is not supported; use 'check'");
    }
    if (!valid_file_name(file_name, lim.identifier_bytes)) {
        return error_v1("invalid_request",
                        "file_name must be a module name with the '.dpm' extension, for example 'main.dpm'");
    }
    if (source.size() > lim.source_bytes) return limit_error("source size in bytes", "source_bytes", lim.source_bytes);
    if (count_lines(source) > lim.lines) return limit_error("number of source lines", "lines", lim.lines);
    if (longest_identifier(file_name, source) > lim.identifier_bytes) {
        return limit_error("identifier length in bytes", "identifier_bytes", lim.identifier_bytes);
    }
    MemorySources files;
    files.add(file_name, source);
    CompileOptions opt;
    opt.reader = &files;
    opt.max_nesting = lim.nesting;
    SourceMap sources;
    Diagnostics diags;
    check(file_name, opt, sources, diags);
    return result(diags, sources, lim);
}

}  // namespace dp::api
