// Замер О4 (Р24; docs/research/notes/8_exclusive_access.md, раздел 5):
// доступы, которые правила-кандидаты исключительного изменяемого доступа
// отвергли бы. Журнал ведёт `dpl debug access`; с ним E0502 и заморозка
// корня в обходе (E0412) не отвергают программу, а пишутся событиями.
// В выпуск остаётся только выбранное правило.
#pragma once

#include <string>
#include <vector>

#include "support/source.h"

namespace dp::sema {

struct AccessEvent {
    // call — два аргумента-ссылки в одном вызове; loop — доступ к корню
    // обхода в его теле не через переменную цикла; par — запись во внешнюю
    // переменную в теле `parallel for`; view — снимок вида в понижении.
    std::string rule;
    // call: overlap (одно место или часть и целое), fields (разные поля),
    // const_index (разные постоянные индексы), computed_index;
    // loop: structural (длина корня: push, insert, remove, pop, clear,
    // `&mut` всего корня, присваивание корню, новый ключ Map), element
    // (элемент или поле корня), nested (второй обход того же корня), read
    // (чтение корня в `for … in &mut`);
    // par: var (переменная целиком), index_loopvar (элемент по переменной
    // цикла), index_computed (элемент по другому индексу), structural,
    // read_root (чтение корня `parallel for … in &mut`);
    // view: slice, ref (`&x`), addr (`&место` для C).
    std::string kind;
    Span span;           // доступ
    Span other;          // второй аргумент; заголовок цикла
    std::string root;    // имя корня
    std::string detail;  // call: mut_mut, mut_ref, mut_view; loop: list, map, slice и mut у `&mut`-обхода
};

struct AccessLog {
    std::vector<AccessEvent> events;
};

}  // namespace dp::sema
