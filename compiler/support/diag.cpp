#include "support/diag.h"

#include <algorithm>

#include "support/json.h"
#include "support/utf8.h"

namespace dp {

namespace {

struct CodeInfo {
    std::string_view name;
    std::string_view title;
    std::string_view text;
};

constexpr CodeInfo kCodes[] = {
#define DP_CODE(code, title, text) {#code, title, text},
#include "support/codes.def"
#undef DP_CODE
};

std::string_view severity_name(Severity s) {
    switch (s) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Note: return "note";
    }
    return "error";
}

// Строка исходника и подчёркивание участка: «   3 | text» и «     | ^~~~».
void render_snippet(std::string& out, const SourceFile& f, Span span) {
    LineCol a = f.line_col(span.begin);
    LineCol b = f.line_col(span.end);
    std::string_view line = f.line_text(a.line);
    std::string num = std::to_string(a.line);
    out += std::string(num.size() < 4 ? 4 - num.size() : 0, ' ') + num + " | ";
    // Управляющие символы (кроме табуляции) в строке — U+FFFD: один символ
    // на место одного, столбцы не сдвигаются.
    for (char c : sanitize_utf8(line)) {
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') out += "\xEF\xBF\xBD";
        else out += c;
    }
    out += '\n';
    out += "     | ";
    // Отступ до столбца: табуляции сохраняются, чтобы метка встала под текст.
    uint32_t col = 1;
    for (size_t i = 0; i < line.size() && col < a.col; i++) {
        unsigned char c = static_cast<unsigned char>(line[i]);
        if ((c & 0xC0) == 0x80) continue;
        out += c == '\t' ? '\t' : ' ';
        col++;
    }
    uint32_t width = b.line == a.line && b.col > a.col ? b.col - a.col : 1;
    out += '^';
    if (width > 1) out += std::string(width - 1, '~');
    out += '\n';
}

void render_place(std::string& out, const SourceMap& sources, Span span) {
    const SourceFile& f = sources.file(span.file);
    LineCol lc = f.line_col(span.begin);
    out += f.path() + ":" + std::to_string(lc.line) + ":" + std::to_string(lc.col) + ": ";
}

void json_place(std::string& out, const SourceMap& sources, Span span) {
    const SourceFile& f = sources.file(span.file);
    LineCol a = f.line_col(span.begin);
    LineCol b = f.line_col(span.end);
    out += "\"file\":";
    json_string(out, f.path());
    out += ",\"line\":" + std::to_string(a.line) + ",\"col\":" + std::to_string(a.col);
    out += ",\"end_line\":" + std::to_string(b.line) + ",\"end_col\":" + std::to_string(b.col);
}

}  // namespace

std::string_view code_name(Code c) { return kCodes[static_cast<size_t>(c)].name; }
std::string_view code_title(Code c) { return kCodes[static_cast<size_t>(c)].title; }
std::string_view code_text(Code c) { return kCodes[static_cast<size_t>(c)].text; }

bool find_code(std::string_view name, Code& out) {
    for (size_t i = 0; i < static_cast<size_t>(Code::Count); i++) {
        if (kCodes[i].name == name) {
            out = static_cast<Code>(i);
            return true;
        }
    }
    return false;
}

Diagnostic& Diagnostics::error(Code code, Span span, std::string message) {
    Diagnostic& d = list_.emplace_back();
    d.code = code;
    d.span = span;
    d.has_span = true;
    d.message = std::move(message);
    return d;
}

Diagnostic& Diagnostics::error(Code code, std::string message) {
    Diagnostic& d = list_.emplace_back();
    d.code = code;
    d.message = std::move(message);
    return d;
}

bool Diagnostics::has_errors() const { return error_count() > 0; }

size_t Diagnostics::error_count() const {
    return static_cast<size_t>(
        std::count_if(list_.begin(), list_.end(), [](const Diagnostic& d) { return d.severity == Severity::Error; }));
}

std::vector<Diagnostic> sorted(const std::vector<Diagnostic>& diags) {
    std::vector<Diagnostic> out = diags;
    std::stable_sort(out.begin(), out.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.has_span != b.has_span) return !a.has_span;
        if (a.has_span) {
            if (a.span.file != b.span.file) return a.span.file < b.span.file;
            if (a.span.begin != b.span.begin) return a.span.begin < b.span.begin;
        }
        return a.code < b.code;
    });
    return out;
}

std::string render_text(const std::vector<Diagnostic>& diags, const SourceMap& sources) {
    std::string out;
    for (const Diagnostic& d : sorted(diags)) {
        if (d.has_span) render_place(out, sources, d.span);
        out += std::string(severity_name(d.severity)) + "[" + std::string(code_name(d.code)) + "]: " + sanitize_utf8(d.message) + '\n';
        if (d.has_span) render_snippet(out, sources.file(d.span.file), d.span);
        for (const Note& n : d.notes) {
            if (n.has_span) render_place(out, sources, n.span);
            out += "note: " + sanitize_utf8(n.message) + '\n';
            if (n.has_span) render_snippet(out, sources.file(n.span.file), n.span);
        }
    }
    return out;
}

std::string render_json(const std::vector<Diagnostic>& diags, const SourceMap& sources) {
    std::string out = "{\"diagnostics\":[";
    bool first = true;
    for (const Diagnostic& d : sorted(diags)) {
        if (!first) out += ',';
        first = false;
        out += "{\"severity\":";
        json_string(out, severity_name(d.severity));
        out += ",\"code\":";
        json_string(out, code_name(d.code));
        out += ",\"message\":";
        json_string(out, d.message);
        if (d.has_span) {
            out += ',';
            json_place(out, sources, d.span);
        }
        out += ",\"notes\":[";
        for (size_t i = 0; i < d.notes.size(); i++) {
            const Note& n = d.notes[i];
            if (i) out += ',';
            out += "{\"message\":";
            json_string(out, n.message);
            if (n.has_span) {
                out += ',';
                json_place(out, sources, n.span);
            }
            out += '}';
        }
        out += "]}";
    }
    out += "]}\n";
    return out;
}

}  // namespace dp
