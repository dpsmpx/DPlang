// Парсер (docs/spec/core.md, раздел 2): рекурсивный спуск с приоритетами
// операторов. Разбор не зависит от типов (Р7). После ошибки парсер
// пропускает остаток строки или объявления и продолжает, так что одна
// ошибка даёт одно сообщение, а модуль разбирается до конца.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "support/diag.h"
#include "support/source.h"
#include "syntax/ast.h"

namespace dp {

// Предел вложенности по умолчанию (CLI): высокий, только против
// переполнения стека в проходах компилятора (docs/compiler/README.md,
// «Предел вложенности»). Онлайн-профиль задаёт свой, меньше.
inline constexpr uint32_t kDefaultMaxNesting = 2000;

struct ParseOptions {
    // Наибольшая глубина дерева модуля: выражения, инструкции, блоки,
    // образцы и типы внутри друг друга. Глубже — E0002, разбор модуля
    // останавливается, объявления не сохраняются.
    uint32_t max_nesting = kDefaultMaxNesting;
};

std::unique_ptr<ast::Module> parse_module(const SourceFile& file, FileId id, std::string name, Diagnostics& diags,
                                          const ParseOptions& opt = ParseOptions{});

// Глубина дерева модуля без рекурсии (явный стек). Первый в порядке обхода
// узел глубже limit или nullptr. Новый дочерний узел в ast.h — и сюда
// (syntax/depth.cpp).
const ast::Node* deeper_than(const ast::Module& m, uint32_t limit);

}  // namespace dp
