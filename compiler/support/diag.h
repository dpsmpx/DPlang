// Диагностика: ошибки пользователя с кодами из реестра (support/codes.def).
//
// Ошибка пользователя — всегда диагностика, а не падение
// (docs/policy/ENGINEERING.md, раздел 6). Текстовый и машиночитаемый
// (JSON) виды строятся из одних и тех же данных.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "support/source.h"

namespace dp {

enum class Code : uint16_t {
#define DP_CODE(code, title, text) code,
#include "support/codes.def"
#undef DP_CODE
    Count
};

std::string_view code_name(Code c);   // "E0001"
std::string_view code_title(Code c);  // краткое название
std::string_view code_text(Code c);   // объяснение для explain
// Код по имени ("E0001"); false — такого кода нет.
bool find_code(std::string_view name, Code& out);

enum class Severity : uint8_t { Error, Warning, Note };

struct Note {
    Span span;
    bool has_span = false;
    std::string message;
};

struct Diagnostic {
    Severity severity = Severity::Error;
    Code code = Code::E0001;
    std::string message;
    Span span;
    bool has_span = false;
    std::vector<Note> notes;
};

class Diagnostics {
public:
    Diagnostic& error(Code code, Span span, std::string message);
    // Ошибка без места в исходнике (например, файл не найден).
    Diagnostic& error(Code code, std::string message);

    const std::vector<Diagnostic>& list() const { return list_; }
    bool has_errors() const;
    size_t error_count() const;

private:
    std::vector<Diagnostic> list_;
};

// Порядок вывода не зависит от порядка обнаружения: файл, позиция, код.
std::vector<Diagnostic> sorted(const std::vector<Diagnostic>& diags);

std::string render_text(const std::vector<Diagnostic>& diags, const SourceMap& sources);
std::string render_json(const std::vector<Diagnostic>& diags, const SourceMap& sources);

}  // namespace dp
