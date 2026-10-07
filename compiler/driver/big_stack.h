// Исполнение функции в потоке с большим стеком (CLI и worker онлайн-
// сервиса). Проходы компилятора рекурсивны: глубину дерева ограничивает
// предел вложенности (syntax/parser.h), а стек под него даёт этот поток.
#pragma once

#include <cstddef>

namespace dp {

// Стек компилятора: хватает на kDefaultMaxNesting уровней во всех проходах
// с запасом, в том числе в сборке с ASan (docs/compiler/README.md, «Предел
// вложенности»). Память — виртуальная: занимаются только тронутые страницы.
inline constexpr size_t kCompilerStackBytes = size_t(512) << 20;

// fn(arg) в новом потоке со стеком stack_bytes; вызывающий ждёт его конца.
// false — поток не создан, fn не вызвана.
bool run_on_stack(size_t stack_bytes, void (*fn)(void*), void* arg);

// То же, но если поток не создать — вызов прямо, в текущем потоке (CLI).
void run_with_stack(size_t stack_bytes, void (*fn)(void*), void* arg);

}  // namespace dp
