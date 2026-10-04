// Лексер (docs/spec/core.md, раздел 1).
//
// Токены покрывают исходник целиком и без пропусков: пробелы, переводы
// строки и комментарии — тоже токены (пустоты). Поэтому дерево разбора
// хранит исходник без потерь (Р7, docs/design/ARCHITECTURE.md). Значения
// литералов лексер проверяет, а вычисляет syntax/literal.h.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "support/diag.h"
#include "support/source.h"

namespace dp {

enum class TokKind : uint8_t {
#define DP_TOKEN(name, desc) name,
#define DP_KEYWORD(name, text) name,
#define DP_RESERVED(name, text) name,
#define DP_PUNCT(name, text) name,
#include "syntax/tokens.def"
    Count
};

struct Token {
    TokKind kind = TokKind::Eof;
    uint32_t begin = 0;
    uint32_t end = 0;
};

// Имя вида для дампа (`Ident`, `KwFn`, `PlusEq`).
std::string_view tok_name(TokKind k);
// Как вид называют в сообщениях: написание (`'fn'`, `'+='`) или описание.
std::string tok_describe(TokKind k);
bool is_trivia(TokKind k);  // пробелы, комментарии (перевод строки — не пустота)
bool is_keyword(TokKind k);

// Разбивает файл на токены; последний — Eof нулевой длины. Ошибки — в diags.
std::vector<Token> lex(const SourceFile& file, FileId id, Diagnostics& diags);

}  // namespace dp
