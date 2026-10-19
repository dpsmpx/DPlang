# 8. Исключительный изменяемый доступ: материал к О4

Заметки исследования, 18 октября 2026. Каждое внешнее утверждение — со
ссылкой. Пометка «(вывод)» — заключение исследования, а не источника.
Пометка «(предложение)» — выбор, который делает автор.

Это материал к решению О4 (исключительный изменяемый доступ, первая задача
v0.3 по `ROADMAP/ROADMAP.md`), а не само решение: здесь варианты, их цена и
способ замера. Решает автор (О4, Р24). Пробы dpl сделаны на
`build/release/dpl`, собранном 17 октября после коммита b461bc5; после него
`compiler/` не менялся. Код проб приведён в пунктах, файлы проб в
репозиторий не входят. Внешние источники просмотрены и сверены 18 октября
2026; в поисковые запросы уходили только общие темы.

## 1. Что уже есть в dpl и что открыто

### Что держит сейчас

- **E0502 — конфликт в одном вызове.** В одном вызове нельзя передать `&mut
  x` вместе с другой ссылкой в `x`: в её поле, в элемент или срезом `x`.
  Спецификация называет это «малой частью исключительного доступа (О4)»;
  полностью он придёт в v0.3. Корни ссылок и срезов внутри заимствующей
  `@c`-структуры проверяются так же.
  `docs/spec/core.md` (ref.alias, ffi.struct);
  `compiler/sema/check_expr.cpp` (`Checker::ref_args`)
- **E0502 сравнивает корни, а не пути.** Отвергаются и непересекающиеся поля
  — `swap2(&mut p.x, &mut p.y)`, `put(&mut p.x, &p.y)`, — и разные элементы:
  `swap2(&mut xs[0], &mut xs[1])`. Корень учитывает переменную цикла
  (`alias_of`) и срезы; срез `str` — копия и корня не даёт. С b461bc5
  учитываются и заимствования в частях литерала (`[View { p: &a }][0..1]`).
  `compiler/sema/check_expr.cpp` (`root_local`, `ref_args`); проба `dpl
  check`; коммит b461bc5
- **Значение-аргумент вычисляется до доступа — как в Swift.** Приняты
  `xs.push(len(xs))`, `xs[len(xs) - 1] = 0`, `xs[xs[0]] = 4`, `add(&mut xs,
  len(xs))` и `add(&mut xs, xs[0])` (при `List[int]`). Порядок такой: место
  получателя и место присваивания вычисляются первыми, затем аргументы;
  `&mut x` даёт место на время вызова. Ложных отказов Rust этого рода
  (раздел 2) в dpl нет. Сохранить это свойство в правиле О4 — предложение.
  `docs/spec/core.md` (expr.order); проба `dpl check`, `debug run`
- **Заморозка корня на время обхода (E0412).** В `for k in &m` (Р54) и `for
  x in &xs[a..b]` (Р65) любое изменение корня, его полей и элементов в теле
  — ошибка. Срез `str` — копия, его корень не замораживается. Механизм —
  стек `frozen_` на время тела.
  `docs/spec/core.md` (stmt.for); `docs/direction/DECISIONS.md` Р54, Р65;
  `compiler/sema/check.cpp` (`frozen_`)
- **Снимок вида в понижении.** Срез, `&x` для `&T` и `&место` для C, стоящие
  левее действия, которое меняет их корень, строятся над копией места
  (`snap_`, `view_base`). С a15f5c4 копия делается, только если правее стоит
  `&mut` на этот корень или изменяющий метод с ним в роли получателя. «E0502
  на вложенные вызовы» Р65 отклонил.
  `docs/spec/core.md` (expr.order); `docs/ir/README.md`, раздел 3;
  `DECISIONS.md` Р65; коммиты eed6697, 1fb0d8c, a15f5c4
- **Ссылки и срезы не хранятся (Р44).** `&T` и `&mut T` бывают только у
  параметров и у переменной `for … in &` (E0501). Срез — только тип
  параметра (E0510). Заимствующая `@c`-структура живёт только в аргументе
  (E0328). Поэтому доступ длится одно из двух: вызов или тело цикла (вывод).
  `DECISIONS.md` Р44; `docs/spec/core.md` (ref.marks, ref.slices,
  ffi.struct)
- **Изменяемых глобальных и замыканий нет.** Верхний уровень модуля —
  функции, структуры, перечисления, константы и импортированные модули.
  Константы вычисляются при компиляции. Замыканий нет и у обратных вызовов
  C. Отсюда главное: вызванная функция видит данные вызывающего только через
  параметры, и правило проверяется внутри одной функции (вывод).
  `docs/spec/core.md` (module.scope, decl.const); `docs/ir/README.md`,
  раздел 3; `DECISIONS.md` Р61
- **Значения копируются (Р45).** Перемещение с явным `clone()` отклонено:
  «больше правил и отказов, решения опыта v0.0 местами перестали бы
  собираться». Поэтому конфликт исправляется копией (вывод).
  `DECISIONS.md` Р45; `docs/spec/core.md` (memory.values)
- **У линейных — своя исключительность (E0513).** Ресурс, взятый ссылкой в
  аргументе (`&r`, поле-ссылка литерала, его список, `some(...)`), нельзя
  поглотить в том же аргументе или правее. Так же держат корень получатель,
  место присваивания и индекс: `r.items.push(take(r))`, `r.items[take(r)] =
  5`. Формы добавлялись по находкам ревью и атак.
  `docs/spec/core.md` (ffi.linear); `compiler/support/codes.def` (E0513);
  `compiler/sema/linear.cpp`; `DECISIONS.md` Р60; коммиты 1fb0d8c, cb78d32,
  24d95cb, b461bc5
- **Обход `List` держится проверкой длины.** В `for x in &xs` и `for x in
  &mut xs` переменная — место `xs[k]`, длина проверяется на каждом шаге. Р65
  так и советует менять список в цикле.
  `docs/spec/core.md` (stmt.for); `docs/ir/README.md`, раздел 3, «Циклы»;
  `DECISIONS.md` Р65

### Что открыто (пробы)

- **Тихая запись не в тот элемент.** Цикл `for x in &mut xs { x = grow(&mut
  xs) }`, где `grow` вставляет в начало, проходит проверку. Вывод `100 100
  100 100 100 7 1 2 3` одинаков в интерпретаторе и native. Память цела, но
  это инвалидация итератора — то, против чего Р9 называет исключительность.
  проба `dpl check`, `debug run`, `build`
- **Ловушка вместо ошибки компиляции.** `for y in &mut ys { y = shrink(&mut
  ys) }` и `for x in &xs { clear_list(&mut xs); println(str(x)) }` падают с
  «index 0 is out of bounds for length 0», код 101. Для среза Р65 «живой
  обход» отклонил, для `List` он остался.
  проба `debug run`, `build`; `DECISIONS.md` Р65
- **Два перекрывающихся изменяемых доступа.** Вложенные `for a in &mut zs {
  for b in &mut zs { a += b } }` приняты; вывод `7 21 62` зависит от
  порядка. `for w in &ws { ws.push(w) }` без `break` растёт до ловушки
  нехватки памяти (trap.memory). По SE-0176 оба случая — нарушения (раздел
  2).
  проба `dpl check`, `debug run`; `docs/spec/core.md` (trap.memory)
- **Чтение корня в изменяющем обходе.** `for x in &mut xs { x += xs[0] }`
  принят. Первый элемент прибавил старое `xs[0]`, остальные — новое: `[1, 4,
  0]` стало `[2, 6, 2]`. Это детерминировано, но зависит от порядка; в
  `parallel for` с потоками это гонка (вывод).
  проба `dpl check`, `debug run`
- **`parallel for` проверяется как `for`.** Приняты `parallel for i in 0..10
  { total += i; acc[0] += i; acc.push(i) }` и `parallel for x in &mut xs { x
  += xs[0]; total += x }`; грамматика допускает `parallel for x in &mut xs`.
  В v0.1–v0.2 итерации идут по очереди, и это допустимый порядок. С потоками
  v0.3 каждая такая запись станет гонкой данных, то есть неопределённым
  поведением в сгенерированном C, против Р11 (вывод).
  `docs/spec/core.md` (stmt.for, грамматика); `DECISIONS.md` Р11; проба `dpl
  check`
- **Скрытая копия списка.** В `sum(xs, grow(&mut xs))` (`sum` берёт срез) и
  `first(&xs, grow(&mut xs))` IR содержит копию всего списка на каждый вызов
  (`copy %0` перед `slice`). У `&ys[0]` копируется элемент, а он сам может
  быть списком. До a15f5c4 снимок брался при любом вызове правее, и в цикле
  выходило квадратично. Р65 отклонил снимок цикла за то, что копия «не видна
  в коде», а снимок в вызове оставил.
  проба `dpl debug ir`; коммит a15f5c4; `DECISIONS.md` Р65
- **Ложные отказы E0502** — непересекающиеся поля и разные константные
  индексы (выше). Это первые кандидаты для счёта по О4.

### Чего требует О13

- **Доступ на время области задач.** В задачу данные идут по значению. Общие
  данные — только для чтения на время области: «исключительность
  гарантирует, что пишущего нет». Изменяемые — непересекающимися частями
  (разбиение срезов и пулов). Общее изменяемое — атомики, мьютекс, каналы.
  Сложность компилятора заложена «средней»: модель «переиспользует проверки
  ссылок без побега и исключительности».
  `docs/direction/OPEN_DECISIONS.md` (О13); `docs/design/LANGUAGE.md`,
  раздел 3.2
- **В классе D — только упорядоченное объединение.** Параллельный цикл и
  отображение: разбиение фиксированного размера, объединение по
  фиксированному дереву. Атомики с гонками, мьютексы и каналы — класс N.
  Накопитель `total += i` в `parallel for`, который сейчас принят, с
  потоками станет гонкой в любом классе (Р11). В D ему отвечает только
  упорядоченная свёртка (вывод).
  `docs/policy/DETERMINISM.md`, раздел 3; `DECISIONS.md` Р11, Р12, Р29
- **Эталон t14 — готовый случай для замера.** Эталон пишет из `parallel for
  t in 0..threads` в `partial[b] = s` при вычисленном `b` (`b = t`, затем `b
  += threads`) и в `local[t][…] += 1`. Решение модели (k2-r1, попытка 1)
  пишет в `parts[w]` по переменной цикла. Правило «внешний список — только
  по индексу, равному переменной цикла» примет решение модели и `local[t]`,
  но отвергнет `partial[b]` (вывод).
  `bench/tasks/t14_parallel_fold/dp/sol.dpm`;
  `bench/results/k2-r1/log/t14/dp/attempt-1/sol.dpm`
- **Изменяемые части — новая форма.** Срез только читается (stmt.assign),
  поэтому изменяемый кусок (`&mut xs[a..b]` или обход по частям) затронет
  Р44 и E0510 (вывод).
  `docs/spec/core.md` (stmt.assign, ref.slices); `DECISIONS.md` Р44
- **FFI и потоки.** Хост зовёт функции `@export` из одного потока за раз (до
  v0.3). Копии, построенные для вызова C, освобождаются после вызова, так
  что удерживать указатель после вызова C не может; прямо это в спецификации
  не записано. `List` и `Map` с линейными элементами — E0514 до пулов v0.3.
  Понадобятся (вывод): правило многопоточного входа в `@export`, решение о
  привязке `extern type` к потоку (дескрипторы SDL и Vulkan), исключительный
  доступ к линейным элементам пулов.
  `docs/spec/core.md` (ffi.lib, ffi.struct, ffi.linear);
  `compiler/support/codes.def` (E0514); `DECISIONS.md` Р60, Р61

### Рамка решения

- **Р9.** Проверка заимствований Rust не копируется автоматически. Кандидат
  назван прямо — «исключительность изменяемого доступа к параметрам (против
  инвалидации итераторов и срезов)»; решение — по исследованию и замеру.
  `docs/direction/DECISIONS.md` Р9, Р24
- **О4: мера — доля ложных отказов и найденных ошибок** на программах
  v0.1–v0.3, срок — v0.3. Рекомендация О4 — исключительность «на границе
  вызова». LANGUAGE.md 2.2 добавляет удобство кода с хэндлами и цену
  проверок на нагрузках `BENCHMARKS.md`. Ворота v0.3: нет ошибок
  безопасности памяти в безопасном коде (К-4), доля `unsafe` в пределах
  (Р-6).
  `docs/direction/OPEN_DECISIONS.md` (О4); `docs/design/LANGUAGE.md`, раздел
  2.2; `ROADMAP/ROADMAP.md`, v0.3
- **Р6: надёжность прежде удобства.** Возможность, которая ускоряет
  разработку, но создаёт «скрытое поведение» или «резко усложняет
  компилятор», в текущем виде не принимается. Удобство ИИ (п. 8) уступает
  целям 1–7. Но Р45 отклонил перемещения и ради меньшего числа отказов:
  отказы — тоже цена.
  `DECISIONS.md` Р6, Р45
- **Две границы строгости.** Слабое правило: по Р-5 ошибка памяти в
  безопасном коде — критическая и ведёт к пересмотру модели (О4); по К-4 её
  повтор после исправления ведёт к пересмотру до следующей версии, а при
  неудаче — к заимствованиям в духе Rust. Строгое правило: по Р-6 обходы и
  `unsafe` больше 5% строк вне привязок (порог — предложение) — правило
  ослабляют.
  `docs/direction/RISKS.md` (Р-5, Р-6, К-4)
- **Постановка аудита.** О4 — «главная защита от К-4»: правила, которые
  закрывают семейство по случаям (E0502 на границе вызова, снимок в
  понижении), «станут одним правилом доступа». Висячий вид — первая ошибка
  памяти в безопасном коде; по К-4 до срабатывания оставался один шаг. Поиск
  дал 8 причин и обходы E0502, E0510, E0513. Исправление стоило трёх заходов
  субагентов: поиск, ревью в 5 направлениях с двумя скептиками на находку,
  атака.
  `docs/audits/2026-10-17-v0.2-audit.md`, разделы 3, 4, 5, 7
- **Бюджет.** Компилятор — 17 957 непустых строк `.cpp`, `.h`, `.def` без
  фаззера, 72% бюджета ядра (около 25 тыс., Р-4, предложение). `frozen`,
  `root_local` и `ref_args` — около 80 непустых строк `check_expr.cpp` и
  около 10 в `check.cpp`. Снимок — около 20 строк с `snap_` и `view_base` в
  `ir/lower.cpp`. По постановке аудита правило О4 должно их заменить, а не
  лечь рядом.
  `docs/audits/2026-10-17-v0.2-audit.md`, раздел 6;
  `docs/direction/RISKS.md` Р-4; подсчёт по `compiler/`
- **ИИ — главный пользователь.** E0502 в руководстве — один пункт; у
  пояснения есть подсказка исправления («pass a copy of the value instead»).
  Руководство в задании — половина токенного разрыва с C++ (+18,4%).
  Сообщения — по-английски. Предложение: правило О4 должно укладываться в
  несколько строк и давать машинно применимую правку.
  `docs/guide/README.md`; `compiler/support/codes.def`;
  `docs/direction/RISKS.md` Р-17; `DECISIONS.md` Р47

### Расхождения в документах

- **LANGUAGE.md 2.2** записывает «нет конфликтующего изменяемого доступа —
  статически (исключительность на границе вызова)» как действующую гарантию,
  а спецификация называет её малой частью до v0.3. Там же сказано, что
  исключительность на границе вызова «закрывает инвалидацию срезов и
  итераторов». Но тело `for` — не вызов, и пробы выше показывают инвалидацию
  при одном правиле границы вызова.
  `docs/design/LANGUAGE.md`, раздел 2.2; `docs/spec/core.md` (ref.alias)
- **О4 и ROADMAP v0.3 ещё называют «линейные ресурсы»**, хотя основные
  правила решены в v0.2 (Р57, Р60). Для v0.3 остались линейные элементы
  пулов (E0514). Формулировку стоит уточнить.
  `OPEN_DECISIONS.md` (О4); `ROADMAP/ROADMAP.md`; `DECISIONS.md` Р57, Р60
- **Порядок обхода хэш-таблицы.** В `DETERMINISM.md` и О12 — «порядок
  вставки», в Р54 — по возрастанию ключа. О12 говорит о таблице стандартной
  библиотеки v0.3, Р54 — о встроенной `Map`, но строка `DETERMINISM.md` не
  различает их. По порядку источников прав `DECISIONS.md`.
  `docs/policy/DETERMINISM.md`, раздел 3; `OPEN_DECISIONS.md` (О12);
  `DECISIONS.md` Р54
- **Р65 против полного правила.** Р65 советует менять список в `for x in
  &xs` и отклонил E0502 на вложенные вызовы. Варианты Б и В (раздел 4)
  требуют сначала правки `DECISIONS.md` с записью в историю.
  `DECISIONS.md` Р65; `CLAUDE.md`, «Запросы автора»

## 2. Подходы в других языках

### Swift: закон исключительности

- **Закон.** Два доступа к одной переменной не перекрываются, если хотя бы
  один — запись. Передача inout — запись на всё время вызова. Для `x += x`
  доступ «does not begin until immediately before the call», поэтому это не
  конфликт; `swap(&x, &x)` — конфликт. Вызов изменяющего метода — запись на
  всё время метода. По книге Swift запись в inout начинается после
  вычисления остальных аргументов.
  SE-0176, Implemented (Swift 4.0), 2017:
  https://github.com/swiftlang/swift-evolution/blob/main/proposals/0176-enforce-exclusive-access-to-memory.md
  ·
  The Swift Programming Language, Memory Safety (swift-book, main):
  https://raw.githubusercontent.com/swiftlang/swift-book/main/TSPL.docc/LanguageGuide/MemorySafety.md
- **Статика и динамика.** Статически — локальные, inout-параметры и свойства
  структур. Динамически — свойства классов, глобальные и локальные,
  захваченные замыканиями; с 4.1–4.2 статика покрыла и захват неубегающими
  замыканиями. В dpl нет ни классов, ни изменяемых глобальных, ни замыканий
  (раздел 1).
  SE-0176 (там же)
  https://www.swift.org/blog/swift-5-exclusivity/
  (05.02.2019)
- **Поля и элементы.** Доступы к разным хранимым свойствам структуры могут
  перекрываться. Книга сужает условие: только хранимые свойства локальной
  переменной, не захваченной убегающими замыканиями. Изменение элемента
  массива — доступ ко всему массиву; замена `swap(&a[i], &a[j])` — метод
  `swapAt(i, j)` (SE-0173). У resilient-типов (непрозрачная раскладка)
  перекрывающиеся доступы к разным свойствам не разрешены.
  SE-0176 (там же); swift-book, Memory Safety (там же)
- **Модель inout — копирование входа-выхода.** По книге inout определён как
  copy-in copy-out; передача по ссылке — оптимизация. Исключительность и
  делает их неотличимыми.
  swift-book, Declarations, In-Out Parameters:
  https://raw.githubusercontent.com/swiftlang/swift-book/main/TSPL.docc/ReferenceManual/Declarations.md
- **Дыра внутри открытого доступа.** В `dictionary[key, default:
  dictionary[k]!] += 1` значение по умолчанию вычисляется уже во время
  доступа на изменение. Диагностику добавили только в Swift 5.3.
  Swift CHANGELOG, Swift 5.3 (#54108):
  https://github.com/swiftlang/swift/blob/main/CHANGELOG.md
- **Хронология.** 4.0 (2017) — закон, статическая диагностика, динамика
  только в Debug. 4.1–4.2 — статика сильнее. В 4.2 предупреждение о
  перекрытии стало ошибкой: объявлено 15.05.2018, код с ним перестал
  собираться. 5.0 (2019) — динамика и в Release: необнаруженные нарушения
  падают с «Simultaneous accesses». В блоге 5.0 есть пример
  `append(removingFrom:)`, который при передаче массива самому себе
  зацикливался (аналог `ws.push(w)` в dpl).
  https://www.swift.org/blog/swift-5-exclusivity/
  (05.02.2019) ·
  https://forums.swift.org/t/upgrading-exclusive-access-warning-to-be-an-error-in-swift-4-2/12704
  (15.05.2018)
- **Устройство динамики.** Учёт ведётся двумя битами на переменную:
  «unaccessed / read / modified». Операции неатомарные, гонки ловить учёт
  «not required». SE-0176 допускает поточно-локальную структуру учёта.
  Отслеживать подпути динамически потребовало бы «a fiendish amount of
  bookkeeping», что несовместимо с целями производительности. Оптимизатор
  снимает часть проверок (AccessEnforcementOpts, AccessEnforcementWMO).
  Ownership Manifesto (без даты):
  https://github.com/swiftlang/swift/blob/main/docs/OwnershipManifesto.md
  · SE-0176 (там же) ·
  https://github.com/swiftlang/swift/blob/main/lib/SILOptimizer/Transforms/AccessEnforcementOpts.cpp
- **Неявная копия скрывает конфликт.** Один и тот же код принят для
  копируемой структуры и отвергнут для `~Copyable`. Объяснение в ответах —
  немутирующий вызов может идти над неявной копией `self` (со ссылкой на
  SE-0377); официального разбора нет (08.2025). В другой ветке пользователь
  просил кончать доступ inout раньше конца вызова; ответ — так задумано,
  доступ «notionally the entire call» (09.2024).
  https://forums.swift.org/t/why-is-the-compilers-exclusive-access-enforcement-different-for-copyable-structs/81547
  ·
  https://forums.swift.org/t/exclusive-memory-access-checks-too-restrictive/74466
- **Замыкания.** Статика для неубегающих замыканий потребовала двух
  отдельных ограничений (NRR, NPCR). Частое трение в 4.2 — замыкание,
  переданное изменяющему методу, читает ту же переменную. Обход — прочитать
  значение до вызова или передать изменяемое замыканию параметром inout.
  SE-0176; форум 15.05.2018 (ссылки выше)
- **Некопируемые типы.** Без копий правила грубее: заём хранимого свойства
  занимает всю структуру, «one field of a struct cannot be borrowed while
  another is being mutated».
  SE-0390, Implemented (Swift 5.9):
  https://github.com/swiftlang/swift-evolution/blob/main/proposals/0390-noncopyable-structs-and-enums.md
- **Span.** Неубегающий (`~Escapable`) вид без копии: контейнер нельзя
  менять или уничтожить, пока вид жив, плюс проверка границ на каждом
  доступе. В dpl эту нишу закрывают срезы-параметры, `for x in &xs` и
  параметры-ссылки с лексической областью (вывод).
  SE-0447, Implemented (Swift 6.2):
  https://github.com/swiftlang/swift-evolution/blob/main/proposals/0447-span-access-shared-contiguous-storage.md

### Hylo, Val и изменяемая семантика значений

- **Модель.** Запрещено разделение, а не изменение. Ссылки второго сорта
  создаются только на границе вызова и не хранятся, поэтому переменные «can
  never share mutable state». LANGUAGE.md 2.2 берёт отсюда ссылки второго
  сорта (основа Р44); значения Р45 — та же дисциплина (вывод).
  Racordon и др., «Native Implementation of Mutable Value Semantics»,
  ICOOOLPS 2021, arXiv:2106.12678 (v1, 23.06.2021):
  https://arxiv.org/abs/2106.12678
  ·
  JOT 21(2), 2022:
  https://research.google/pubs/mutable-value-semantics/
- **Swiftlet: проверка путей.** Путь (корень, поля, индексы) не должен
  встречаться дважды среди inout-аргументов вызова; часть и целое
  конфликтуют (`swapX(&v, &v.y)`). Индексы одного массива допустимы, только
  если система типов докажет, что они не совпадают: `swap(&x[0], &x[1])`
  можно, `swap(&x[f(0)], &x[1])` нельзя. Авторы признают ложные отказы
  (`x[0]` против `x[0+1]`). Замыкания захватывают только копией,
  неизменяемо.
  Racordon и др., «Implementation Strategies for Mutable Value Semantics»,
  JOT 21(2), 2022, DOI 10.5381/jot.2022.21.2.a2:
  http://www.jot.fm/issues/issue_2022_02/article2.pdf
- **Цена копий.** inout передаётся как «(possibly interior) pointer»,
  поэтому при copy-on-write вызывающий защитно копирует неуникальное
  хранилище. Swiftlet «significantly slower than Swift in most benchmarks»:
  нет оптимизации, которая доказывает уникальность. Если dpl перейдёт к
  copy-on-write, `&mut` на вложенный элемент потребует того же (вывод).
  JOT 2022, разделы 6.3, 7.4–7.5 (там же)
- **Val: каталог отказов.** Соглашения let, inout, sink, set. `swap(&x.a,
  &x.b)` допустимо. `&v.remove(v[2])` — ошибка, надо передать `v[2].copy()`.
  `swap(&x[i], &x[j])` — ошибка, а `&x.index_swap(i, j)` допустимо: «i == j
  can be checked for dynamically». Неявных копий нет без явного
  `@implicitcopy`.
  The Val Object Model, WG21 P2676R0, 13.10.2022:
  https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2676r0.pdf
- **Hylo: уникальность inout.** `offset_inout(&v, by: v)` — «overlapping
  `inout` access to `v`», подсказка — передать `v.copy()`. Смысл и подсказка
  E0502 в dpl те же.
  Документация без номера версии:
  https://docs.hylo-lang.org/language-tour/functions-and-methods
- **Проекции.** Индексатор или свойство отдаёт часть значения через `yield`
  («inout — yield a part of self for mutation, bounded by lifetime of
  self»). Это кандидат для dpl после v0.3: Р44 запрещает возвращать ссылки
  (вывод).
  P2676R0, раздел 3.6 ·
  https://docs.hylo-lang.org/language-tour/subscripts
- **Проверка заимствований Hylo.** Безопасность проверяет абстрактный
  интерпретатор над Hylo IR, ограничения времён жизни заданы «призрачными»
  инструкциями, аннотаций времён жизни нет. Сайт: компилятор и библиотека
  «still in their early stages».
  Racordon, Abrahams, IWACO @ SPLASH 2023 (аннотация):
  https://2023.splashcon.org/details/iwaco-2023-papers/5/Borrow-checking-Hylo
  ·
  https://www.hylo-lang.org/
  (без даты)

### Rust и Mojo

- **Two-phase borrows — заплата порядка вычисления.** Изменяемый заём
  сначала резервируется как общий, чтобы прошло `v.push(v.len())`. Создаётся
  он только в трёх случаях: автозаём получателя метода, перезаимствование в
  аргументах, неявный заём в перегруженном составном присваивании. RFC 2025
  рассмотрел «вычислять получателя после аргументов» и не принял: автор
  предпочёл усложнить анализ, а не менять порядок выполнения; совместимость
  ограничивает перестановку.
  rustc-dev-guide (текущая):
  https://rustc-dev-guide.rust-lang.org/borrow-check/two-phase-borrows.html
  ·
  RFC 2025 (06.06.2017):
  https://rust-lang.github.io/rfcs/2025-nested-method-calls.html
- **Замер на rustc 1.97.0** (2d8144b78, 2026-07-07; учебные примеры вне
  репозитория, повторён 18.10.2026). Принято `v.push(v.len())`. Отвергнуто:
  `v[v.len() - 1] = 0`, `v[v[0]] = 4`, `Vec::push(&mut v, v.len())` (E0502);
  `v.push(v.pop().unwrap())`, `std::mem::swap(&mut v[0], &mut v[1])`
  (E0499); `s.show(&s.name)` при `&mut self` (E0502). Для `v[v.len() - 1]`,
  `v[v[0]]` и `v.push(v.pop().unwrap())` rustc сам советует локальную
  переменную; для `v[v[0]]` и `swap` — ещё и `split_at_mut`. В dpl записи
  первого рода принимаются (раздел 1).
  замер сессии 18.10.2026
- **Тест rustc про индекс в индексе.** `i[i[3]] = 4;` для типа с `IndexMut`
  помечено E0502 с комментарием «Should be accepted with g2p». Ревизия g2p
  (`-Z two-phase-beyond-autoref`) отключена «until two-phase-beyond-autoref
  support is better». Заголовок теста называет её поведение «undesirable»:
  двухфазность утекла бы за пределы получателя. Обобщённые two-phase borrows
  не включены.
  https://rust.googlesource.com/rust/+/HEAD/tests/ui/borrowck/two-phase-nonrecv-autoref.rs
- **Хранимые ссылки — главный источник сложности.** Все четыре «problem
  cases» NLL — о ссылках, которые держатся в переменных или возвращаются.
  Случай #3 (ссылка возвращается вызывающему из одной ветви) стабильный NLL
  отвергает до сих пор; Polonius Alpha его принимает. Polonius Alpha включён
  в nightly с 04.08.2026 (выключается `-Zpolonius=off`). Худшее замеченное
  замедление вне 10 000 популярных крейтов — 2–3x; цель проекта — не больше
  10–20%. Анализ неполон. Без хранимых и возвращаемых ссылок (Р44) этот
  класс не возникает (вывод).
  RFC 2094 (02.08.2017):
  https://rust-lang.github.io/rfcs/2094-nll.html
  ·
  https://blog.rust-lang.org/2026/08/04/enabling-polonius-alpha-on-nightly
  ·
  https://goals.rust-lang.org/2026/polonius.html
- **Заём всего self.** «Rust currently does not allow tracking borrows
  across function calls»: `&mut foo` занимает всю структуру. View types —
  цель 2026 только для эксперимента, «not to allow people to use view types
  in production», сроков стабилизации нет.
  https://goals.rust-lang.org/2026/view-types-experiment.html
- **Библиотечные замены двух `&mut`.** `slice::swap(a, b)` меняет элементы
  по индексам («If a equals to b, it's guaranteed that elements won't change
  value»), за границами — паника. `split_at_mut` и `chunks_mut` дают
  непересекающиеся изменяемые части.
  Rust std 1.99.1 (13.10.2026):
  https://doc.rust-lang.org/std/primitive.slice.html
- **Опыт игр.** LogLog (26.04.2024): проверка заимствований «forces a
  refactor at the most inconvenient times». Передача `self` вместе с заёмом
  его поля не компилируется, обход — клонирование строки каждый кадр.
  `RefCell` с двумя перекрывшимися `borrow_mut()` падает, часто после
  рефакторинга. Generational arena позволяет «completely side-step the
  borrow checker».
  https://loglog.games/blog/leaving-rust-gamedev/
- **Mojo: исключительность аргументов.** У изменяемой ссылки в вызове нет
  других ссылок на то же значение: `append_twice(my_string, my_string)` —
  ошибка. В 24.5 это было предупреждением с обещанием ошибки позже; 24.6
  проверяет «strict argument exclusivity at compile time». Исключение —
  тривиальные регистровые типы (`Int`, `Bool`): они всегда передаются
  копией.
  Mojo Manual, Ownership (1.1.0):
  https://mojolang.org/docs/manual/values/ownership
  ·
  https://www.mojolang.org/releases/v0.24.5/
  (13.09.2024) ·
  https://modular.com/blog/hands-on-with-mojo-24-6
  (21.01.2025)
- **Два элемента одного списка.** `copy_to(l[0], l[1])` отвергается: у обоих
  один origin `l`. Lattner: проверка не должна заглядывать в `__getitem__`
  («we don't want the type checker to look at full interprocedural
  information like that»); простейший обход — копия первого элемента.
  Форум Modular (21.12.2024–09.01.2025):
  https://forum.modular.com/t/argument-exclusivity-with-arguments-from-two-list-elements/368

### Параллельный цикл и изменение при обходе: Chapel, Java, Swift 6

- **Chapel: намерения `forall`.** Внешние скаляры захватываются значением на
  момент создания задачи. Изменяемый массив по умолчанию берётся `ref`, то
  есть элементы можно писать из тела. Менять внешнюю переменную позволяет
  намерение `ref`, копить — `reduce`: у каждой задачи своя теневая
  переменная, итог объединяется по задачам. Это готовый образец для развилки
  6.
  Chapel 2.10, Language Specification, Data Parallelism:
  https://chapel-lang.org/docs/language/spec/data-parallelism.html
- **Java: структурное изменение и fail-fast.** Итератор `ArrayList` бросает
  `ConcurrentModificationException` при «structural modification» —
  добавлении, удалении элементов или изменении размера. Простая запись
  значения элемента структурным изменением не считается. Срабатывание — «on
  a best-effort basis», только для поиска ошибок. Это образец варианта Г и
  средней строгости в развилке 5.
  Java SE 21, `ArrayList`:
  https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/util/ArrayList.html
- **Swift 6: гонки — при компиляции.** Region-based isolation позволяет
  передать не-`Sendable` значение в другой домен изоляции, если компилятор
  докажет, что передающий его больше не использует. Для dpl передача в
  задачу по значению (О13) это закрывает проще (вывод).
  SE-0414, Implemented (Swift 6.0):
  https://github.com/swiftlang/swift-evolution/blob/main/proposals/0414-region-based-isolation.md

### Регионы и ограниченные ссылки: Vale, Austral, C#, SPARK, Cyclone

- **Vale: поколения вместо исключительности.** Ссылка помнит поколение
  объекта, проверка — 4 инструкции (у вложенных структур — 5–6). Изменяемый
  алиасинг разрешён. BenchmarkRL: +10,84% к небезопасному коду против
  +25,29% у подсчёта ссылок. Это механизм хэндлов Р9; исключительности он не
  даёт (вывод).
  https://verdagon.dev/blog/generational-references
  (обновлено 09.07.2023)
- **Vale: регионы; Valen.** `pure`-вызов замораживает на чтение данные,
  существовавшие до вызова, и снимает проверки поколений на них;
  прототипирована только часть 1. Vale закрыт («archived»). Преемник Valen
  использует group borrowing вместо поколений и регионов: «extremely
  experimental» прототип, не выпущен, замыкания ещё не покрыты (17.09.2026).
  Сама идея group borrowing — предложение Nick Smith для Mojo: аннотации
  групп в сигнатурах, без уникальных ссылок и noalias; на 28.08.2025 не
  реализовано.
  https://verdagon.dev/blog/zero-cost-borrowing-regions-part-1-immutable-borrowing
  (28.04.2023) ·
  https://raw.githubusercontent.com/ValeLang/Vale/master/README.md
  ·
  https://verdagon.dev/blog/golden-spike-reviving-vale-valen
  (17.09.2026) ·
  https://verdagon.dev/blog/group-borrowing
  (28.08.2025)
- **Austral: заём на лексический блок.** В `borrow X as X' in R do B end`
  переменная `X'` доступна только в B, а `X` в B использовать нельзя;
  изменяемая форма — `borrow!`. Имя региона R существует только в B.
  Сложного анализа потока управления нет; проверка линейности — около 600
  строк OCaml, «much of which is error reporting».
  https://austral-lang.org/spec/spec.html
  (Borrow Statement) ·
  https://borretti.me/article/how-australs-linear-type-checker-works
  (29.01.2023)
- **C#: побег закрыт, исключительности нет.** Четыре безопасных контекста
  (declaration-block, function-member, return-only, caller-context) и
  `scoped` вместо имён времён жизни; правила исключительности для
  `ref`-параметров в предложении нет. Ограничения `ref struct` касаются
  обобщений, лямбд и локальных функций, `async`/`await` и итераторов; часть
  ослаблена в C# 13. Правила C# 11 — заявленные «compatibility breaks»:
  прежде собиравшийся код получил CS8352.
  https://github.com/dotnet/csharplang/blob/main/proposals/csharp-11.0/low-level-struct-improvements.md
  ·
  https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/builtin-types/ref-struct
  (13.08.2026) ·
  https://endjin.com/blog/dotnet-csharp-11-ref-fields-scoped-keyword
  (16.09.2024, обновлено 29.03.2026)
- **SPARK: ближайший образец.** Два фактических параметра, которые могут
  пересекаться, допустимы, только если оба формальных неизменяемы или хотя
  бы один неизменяем и передаётся копией (by-copy). Значит, два изменяемых
  (`out`, `in out`) не пересекаются никогда. Те же ограничения — между
  параметром и глобальным, которое читает или пишет подпрограмма. Это
  Verification Rules: их проверяет GNATprove, а не компилятор Ada.
  Пересечение определено по путям: поле — компонент пути, индекс — «the
  literals that each index expression evaluates to». Обоснование: алиасинг
  «inconsistent with the underlying flow analysis and proof models», которые
  считают разные имена разными сущностями.
  SPARK RM 28.0w, §6.4.2 и §3.10:
  https://docs.adacore.com/spark2014-docs/html/lrm/subprograms.html
  ·
  https://docs.adacore.com/spark2014-docs/html/lrm/declarations-and-types.html
- **SPARK: цена без времён жизни.** Заём заморожен на время области
  заёмщика. Структуры с циклами и общими подструктурами (двусвязный список)
  не выражаются. «More complex traversals, involving stacks for example,
  cannot be written iteratively in SPARK». В dpl эти случаи берут хэндлы и
  арены (Р9; вывод).
  SPARK UG 28.0w, §5.9:
  https://docs.adacore.com/spark2014-docs/html/ug/en/source/access.html
  ·
  Dross, Kanig, CAV 2020:
  https://pmc.ncbi.nlm.nih.gov/articles/PMC7363197/
- **Cyclone.** Уникальные указатели «often force awkward coding idioms»; в
  драйверах с арифметикой указателей отслеживаемые указатели «sometimes …
  difficult or impossible». Лучше всего работали арены с алиасными
  указателями. Что регионы закрывают время жизни, но не алиасинг, — вывод.
  Swamy и др., SCP 2006:
  https://www.cs.umd.edu/projects/PL/cyclone/scp.pdf
- **Хэндлы — алиасинг через индекс.** Racordon (2025): граф и двусвязный
  список выражаются без ссылок — через индексы в контейнере. На Swift такой
  список быстрее списка на ссылках примерно на порядок, главным образом
  из-за локальности кэша. Два хэндла могут указывать на одну ячейку; статика
  ловит это, только если доступ идёт путём от арены (вывод).
  Programming 2025 Companion, DOI 10.4230/OASIcs.Programming.2025.25:
  https://drops.dagstuhl.de/storage/01oasics/oasics-vol134-programming2025/OASIcs.Programming.2025.25/OASIcs.Programming.2025.25.pdf

## 3. Эмпирика

### Что закрывает правило

- **Use-after-free — крупнейший отдельный класс.** Chromium: около 70%
  серьёзных ошибок безопасности — память, половина из них — UAF (912 ошибок
  high/critical стабильного канала с 2015 года; дата среза на странице не
  указана). Google: UAF «rarely a mistake by a single programmer» — поздняя
  правка ломает раннее допущение. MiraclePtr: память процесса браузера
  +4,5–6,5% на Windows и +3,5–5% на Android (2022); «effectively mitigated
  57%» UAF в привилегированных процессах (2024).
  https://www.chromium.org/Home/chromium-security/memory-safety/
  ·
  https://security.googleblog.com/2022/09/use-after-freedom-miracleptr.html
  (13.09.2022) ·
  https://security.googleblog.com/2024/10/safer-with-google-advancing-memory.html
  (15.10.2024)
- **0-day 2021.** Project Zero: 39 из 58 — порча памяти, из них 17 UAF (29%
  всех).
  https://projectzero.google/2022/04/the-more-you-know-more-you-know-you.html
  (19.04.2022)
- **Microsoft.** Около 70% уязвимостей с CVE, которые исправляет Microsoft,
  — безопасность памяти; доля почти та же больше десяти лет.
  https://www.microsoft.com/en-us/msrc/blog/2019/07/we-need-a-safer-systems-programming-language
  (18.07.2019)
- **Пространственное — не задача О4.** На пространственные ошибки приходится
  40% эксплойтов in-the-wild по ошибкам памяти за десятилетие (по данным
  Project Zero). Проверки границ в libc++ стоили в среднем 0,30% и нашли
  больше 1000 ошибок. В dpl это проверки границ и ловушки (Р11, Р36).
  https://security.googleblog.com/2024/11/retrofitting-spatial-safety-to-hundreds.html
  (15.11.2024)
- **Инвалидация итератора в дикой природе.** CVE-2026-2441 (Chrome, Blink,
  `CSSFontFeatureValuesMap`): изменение HashMap во время обхода вызывало
  перехэширование, освобождавшее хранилище под итератором. Исправлено
  глубокой копией вместо сырого указателя (Chrome 145.0.7632.75). Дата
  исправления 13.02.2026 и внесение в CISA KEV 17.02.2026 — по странице Wiz
  (см. «Осторожно»). Систематической доли этого класса среди UAF нет; Trail
  of Bits называет его «common», но без счёта.
  https://www.exploit-db.com/exploits/52542
  ·
  https://www.wiz.io/vulnerability-database/cve/cve-2026-2441
  ·
  https://blog.trailofbits.com/2020/10/09/detecting-iterator-invalidation-with-codeql/
  (09.10.2020)
- **Mozilla, стили Firefox.** 51 из 69 исторических ошибок безопасности
  (73,9%) «would not have been possible», будь компонент сразу на Rust.
  https://hacks.mozilla.org/2019/02/rewriting-a-browser-component-in-rust/
  (28.02.2019)
- **Android.** Доля уязвимостей памяти — 76% в 2019 году, 24% в 2024-м; в
  2025-м впервые ниже 20% (данные почти итоговые). Причина — новый код на
  безопасных языках. У средних и больших правок на Rust примерно вчетверо
  меньше откатов, чем на C++.
  https://security.googleblog.com/2024/09/eliminating-memory-safety-vulnerabilities-Android.html
  (25.09.2024) ·
  https://blog.google/security/rust-in-android-move-fast-fix-things/
  (13.11.2025)

### Цена динамической проверки

- **Swift, 2018.** Проверки в Release на 656 микротестах: ClassArrayGetter2
  +840,3%, HashTest +95,4%, Hanoi +46,5%. После оптимизаций остались
  «several regressions larger than 10%». Худшие случаи — доступ к свойствам
  классов.
  https://forums.swift.org/t/enabling-run-time-exclusivity-checking-in-release-mode/17626
  (03.11.2018)
- **Swift, 2025.** Разборщик plist на файле 35 МБ: 0,352 с с проверками
  против 0,188 с без них. Причина — локальная функция, захватившая `var`.
  Помог вынос функции с передачей состояния inout-параметром.
  https://forums.swift.org/t/parsing-performance-and-exclusivity-checks/79333
  (17.04.2025)

### Люди и заимствования

- **Времена жизни дороже владения.** 118 нарушений в 100 вопросах Stack
  Overflow, 77 (65%) — вычисление времён жизни. Времена жизни хотя бы иногда
  путают 85,1% опрошенных (владение — 52,4%). Сообщения компилятора о
  временах жизни «всегда» понимают 10,0%, о владении — 39,6%.
  Zhu и др., ICSE 2022:
  https://par.nsf.gov/servlets/purl/10321050
- **Сложные псевдонимы.** РКИ, 428 студентов: в задаче со сложными
  псевдонимами пользователи сборщика мусора Bronze чаще укладывались во
  время; завершившие тратили около 4 часов против 12.
  Coblenz и др., ICSE 2022:
  https://arxiv.org/abs/2110.01098
- **Объяснять через права на пути.** Модель «flow-sensitive permissions on
  paths into memory» подняла баллы Ownership Inventory в среднем на 9% (N =
  342, d = 0,56).
  Crichton и др., OOPSLA 2023:
  https://arxiv.org/abs/2309.04134
- **Google, больше 1000 разработчиков.** Владение и заимствования — в тройке
  трудностей вместе с макросами и async. Качеством диагностики недовольны
  9%.
  https://opensource.googleblog.com/2023/06/rust-fact-vs-fiction-5-insights-from-googles-rust-journey-2022.html
  (27.06.2023)

### Модели и заимствования

- **16,7% ошибок компиляции Rust у моделей — владение и времена жизни**
  (2165 из 12 942, базовый запрос). Всего 86 726 ошибочных образцов, семь
  моделей, четыре языка. У C, C++ и Java в этой категории 0–2 случая.
  Алиасинг от времён жизни не отделён.
  Nogueira и др., arXiv:2608.00661v1 (01.08.2026), таблица VI:
  https://arxiv.org/html/2608.00661
- **Цикл исправления снимает большую часть.** CRUST-Bench, доля проектов с
  ошибками заимствований до и после исправления по компилятору: o3 — 21% →
  2%, Claude Opus 4 — 29% → 3%, o1 — 42% → 9%, Claude 3.7 Sonnet — 18% → 4%.
  Приложение даёт для Claude 3.7 «reduced by 75%». Пример неудачи — владение
  пользовательским типом `Graph`. SafeTrans: направленное исправление решило
  в среднем 74,2% нарушений borrow checker по всем моделям.
  https://arxiv.org/abs/2504.15254
  (v3, 01.10.2025), таблица 5, приложение D ·
  https://arxiv.org/abs/2505.10708
  (v2, 22.04.2026)
- **Шаблон dpl встречается.** RustRepoTrans среди ошибок состояния
  переменных называет «simultaneous mutable borrows» и передачу одной
  переменной как `&` и `&mut` сразу. Доминирует же непонимание различий
  языков (73,9% ошибок: зависимости, синтаксис).
  https://arxiv.org/abs/2411.13990
  (v6, 17.10.2025)
- **Подсказки компилятора — меньшинство правок.** Rust-lancet: встроенные
  подсказки rustc исправили 45 из 160 программ с нарушениями владения
  (28,1%). RustAssistant (цикл модели и компилятора) — пиковая точность
  около 74% на реальных ошибках компиляции из популярных открытых
  репозиториев Rust (все виды ошибок, не только владение).
  ICSE 2024:
  https://conf.researchr.org/details/icse-2024/icse-2024-research-track/101/Rust-lancet-Automated-Ownership-Rule-Violation-Fixing-with-Behavior-Preservation
  ·
  Deligiannis и др., arXiv:2308.05177 (09.08.2023):
  https://arxiv.org/abs/2308.05177

### Своё

- **Корпус для замера.** В `bench/` 358 файлов `.dpm` в git, из них 202 с
  `&mut`. Вхождений `&mut имя` — 1470, почти все — пометки аргументов. В
  `tests/` — 184 файла.
  подсчёт `git ls-files` и grep по `bench/`, `tests/`
- **Цена «по случаям».** Висячий вид — 8 причин и обходы трёх правил, три
  захода субагентов (раздел 1).
  `docs/audits/2026-10-17-v0.2-audit.md`, разделы 3, 5
- **Цена руководства.** +18,4% токенов к C++, половина — само руководство в
  задании.
  `docs/direction/RISKS.md` Р-17

### Чего нет

- **Пробелы.** Нет публичных данных о частоте ошибок и цене обучения для
  изменяемой семантики значений (Swift inout, Hylo) и о доле инвалидации
  итераторов среди UAF. Это мерить самим (раздел 5).

## 4. Варианты для dpl

Допущение исследования: во всех вариантах остаются Р44 и Р45, изменяемых
глобальных и замыканий нет. Поэтому каждый вариант проверяется статически
внутри функции (раздел 1). Различаются длительность доступа и судьба снимка.
Оценки объёма кода — оценки, не замер.

### А. Пути на границе вызова

- **Правило.** В одном вызове путь аргумента `&mut` (корень, поля,
  константные индексы) не совпадает с путём другого аргумента-ссылки или
  вида и не является его началом или продолжением (Swiftlet, SPARK).
- **Ловит.** То же, что E0502 сейчас; новых ошибок нет. Висячие виды во
  вложенных вызовах держит снимок, обходы — проверка длины и E0412 у `Map` и
  срезов.
- **Остаётся.** Все открытые случаи раздела 1: тихая запись, ловушки,
  вложенные `&mut`-обходы, `ws.push(w)`, `parallel for`, скрытые копии
  снимка.
- **Ложные отказы.** Меньше, чем сейчас: принимаются `swap2(&mut p.x, &mut
  p.y)`, `put(&mut p.x, &p.y)`, `swap2(&mut xs[0], &mut xs[1])`. Отказом по
  замыслу остаются `swap2(&mut xs[i], &mut xs[j])` (индексы могут совпасть)
  и `put(&mut p, &p.x)` (часть и целое).
- **Цена для ИИ.** Строка руководства прежняя, отказов меньше.
- **О13.** Не помогает: длительность «область задач» пришлось бы делать
  отдельным механизмом рядом, против постановки аудита.
- **Компилятор.** Десятки строк (оценка): путь вместо `root_local` в
  `ref_args`. Ничего не удаляет.
- **Решения.** Правок `DECISIONS.md` не нужно; защиты от К-4 не добавляет.
  Совпадает с рекомендацией О4 («на границе вызова») и с LANGUAGE.md 2.2, но
  не закрывает инвалидацию, которую LANGUAGE.md ей приписывает.

### Б. Вызов и тело обхода (снимок Р65 остаётся)

- **Правило.** А, плюс: `for x in &xs` — чтение `xs` на всё тело, `for x in
  &mut xs` — запись. В теле корень меняется только через переменную цикла
  (E0412 для всех коллекций, как сейчас у `Map` и срезов); новый обход того
  же корня — тоже доступ.
- **Ловит.** Все случаи обхода из проб: `x = grow(&mut xs)`, `y =
  shrink(&mut ys)`, `clear_list(&mut xs)` в теле, `ws.push(w)`, вложенные
  `for … in &mut zs`. Вместо тихой записи, ловушки и роста без конца —
  ошибка компиляции. Три правила (Р54, Р65, `List`) становятся одним.
- **Остаётся.** Снимок вида во вложенном вызове — скрытая копия. Проверка
  длины на шаге — вторая линия: в безопасном коде она срабатывать не должна,
  и её можно вынести из цикла (выигрыш — замерить). Обход индексов сохраняет
  ту же логическую ошибку, но явно: `for i in 0..len(xs) { xs[i] = grow(&mut
  xs) }` — индекс как хэндл (вывод).
- **Ложные отказы.** Изменение списка во время его обхода: `for x in &xs {
  if x > 0 { xs.push(x) } }`, `for c in &mut cells { if c.split {
  cells.push(child(c)) } }`. Правка — обход индексов `for i in 0..len(xs)`
  (диапазон вычисляется один раз) или обход копии; обе применимы машинно. В
  строгой форме (развилка 5) отказом становится и чтение корня в
  `&mut`-обходе — `x += xs[0]`, `len(xs)`; правка — `let first = xs[0]` до
  цикла.
- **Цена для ИИ.** Одна строка: «пока идёт обход `xs`, `xs` меняется только
  через переменную цикла; иначе — `for i in 0..len(xs)` или копия». E0412 с
  подсказкой уже есть.
- **О13.** Прямо: заморозка корня и есть «общее только для чтения на время
  области»; тело `parallel for` — такая область для всех внешних переменных,
  кроме разрешённых непересекающихся записей (вывод).
- **Компилятор.** Обобщение `frozen_` на `List` и на режим «запись»;
  различия `Map` и среза уходят. Рост малый (оценка).
- **Решения.** Правка Р65: он советует менять список в `for x in &xs`.

### В. Одно правило доступа без скрытых копий

- **Правило.** Б, плюс: вид (срез, `&x`, `&место`) — чтение до конца вызова,
  которому он передан. Действие правее в том же выражении, меняющее тот же
  корень, — E0502. Копия делается явно, до вызова.
- **Ловит.** Всё из Б. Семейство висячего вида закрыто правилом проверки, а
  не понижением, и `snap_` с `view_base` можно убрать. Если ссылку на
  линейный ресурс считать чтением, а поглощение — записью, в то же правило
  входит часть E0513 о ссылке в аргументе. Остальные формы E0513 (цикл,
  `errdefer`, поле) остаются своими.
- **Остаётся.** Только проверка длины на шаге, как вторая линия.
- **Ложные отказы.** `show(&cells[0], wipe(&mut cells))`, `sum(xs, grow(&mut
  xs))` со срезом, `first(&xs, grow(&mut xs))`. Правка — `let c = cells[0]`
  или `let old = xs` перед вызовом. Принимаются (это не виды): скаляры по
  значению (`c + bump(&mut c)`, `add(&mut xs, len(xs))`) и `List` по
  значению — его копия объявлена сигнатурой (Р45).
- **Цена для ИИ.** Ещё полстроки: «вид и изменение того же корня в одном
  выражении — ошибка; копия до вызова». Частота таких записей неизвестна —
  мерить.
- **О13.** Как Б; модель доступа одна для вызова, цикла и области задач.
- **Компилятор.** Проверка растёт на учёт видов в выражении (E0502 на
  вложенные вызовы), понижение теряет снимок; итог — мерить строками (Р-4).
- **Решения.** Р65 правится дважды: обход `List` и отклонённое там «E0502 на
  вложенные вызовы». Довод за: Р65 отклонил снимок цикла как скрытую копию,
  а В делает позицию по скрытым копиям единой (Р6).

### Г. Динамическая исключительность обхода

- **Правило.** А статически. Список под обходом или видом несёт отметку
  доступа, и изменение его длины при отметке — ловушка (как динамика Swift,
  fail-fast в Java).
- **Ловит.** Случаи обхода — во время выполнения: ловушка вместо тихой
  записи. Вложенные `&mut`-обходы — только если отметка различает чтение и
  запись.
- **Остаётся.** Ошибки видны только при запуске. `parallel for` не закрыт:
  учёт Swift неатомарный и гонок ловить не обязан, Java срабатывает «on a
  best-effort basis».
- **Ложные отказы.** При компиляции — нет; программы, которые сейчас меняют
  список в обходе, начинают падать.
- **Цена для ИИ.** Руководство не растёт, но ошибка видна только в тесте —
  больше итераций.
- **О13.** Не помогает: потокам нужна статика или атомарные отметки.
- **Компилятор и runtime.** Отметка в заголовке `List` и проверка на каждом
  изменении длины. Цифры Swift (до +840% в худшем микротесте, раздел 3)
  относятся к проверке каждого доступа к свойствам классов и с Г не
  сопоставимы; цену Г надо мерить самим.
- **Решения.** Против Р65 («ловушка вместо ошибки компиляции» отклонена для
  среза) и Р6, п. 5 (ловить до запуска).

### Д. Копирование входа-выхода вместо исключительности

- **Правило.** `&mut x` — копия на входе и запись на выходе (модель inout в
  книге Swift); `&T` и виды — копия на входе. Алиасинг между аргументами
  становится определённым: побеждает последняя запись.
- **Ловит.** Висячие виды в вызовах (как снимок сейчас, но всегда). Обходы
  не закрывает: тело цикла — не вызов.
- **Цена.** Скрытые копии списков на каждом вызове, если компилятор не
  докажет отсутствие алиасинга, — то, что Р65 отклонил для цикла; `swap(&x,
  &x)` молча «работает». По Р6 — скрытое поведение (вывод). Приведён для
  полноты, кандидатом не предлагается.
  swift-book, Declarations, In-Out Parameters (раздел 2)

### Развилки внутри вариантов

1. **Гранулярность.** Корень (сейчас) / пути с полями (Swift, SPARK) / ещё и
   разные константные индексы (Swiftlet, SPARK — по значению индекса).
2. **Два элемента по вычисленным индексам.** Отказ и библиотечные `swap_at`,
   `split_at` (Swift `swapAt`; Rust `slice::swap`, где `i == j` ничего не
   меняет, и `split_at_mut`) / проверка `i != j` с ловушкой (Val
   `index_swap`; Р36) / отказ без замены.
3. **`&T` и `&mut T` на одно место.** Отказ (Hylo, Mojo, сейчас E0502) /
   копия для малых значений (Mojo — тривиальные типы; SPARK — by-copy).
4. **Начало доступа** — после вычисления всех аргументов и индексов, как
   сейчас и в Swift. Записать в спецификацию вместе с выражениями внутри
   открытого доступа (дыра Swift до 5.3).
5. **Строгость обхода** — запрещены только структурные изменения корня
   (длина: `push`, `insert`, `remove`, `clear`; как «structural
   modification» в Java) / любое изменение корня / ещё и чтение корня в
   `&mut`-обходе. Первая форма закрывает тихую запись, ловушки и рост, но
   оставляет вложенные `&mut`-обходы и `x += xs[0]` (вывод).
6. **Запись в теле `parallel for`** (О13): только `out[i]` по переменной
   цикла / ещё вычисленный индекс с проверкой непересекаемости при
   выполнении / внешние скаляры только для чтения плюс явное намерение
   свёртки (Chapel `reduce`) / только упорядоченная свёртка библиотеки
   (класс D). t14 различает эти формы.
7. **Граница модуля.** Непрозрачная раскладка — заём целиком (Swift:
   resilient-типы в SE-0176, некопируемые в SE-0390). Для dpl важно, если
   сводка модуля не раскрывает поля (вывод).

## 5. Как решить по замеру (Р24)

Весь раздел — предложение; решает автор.

- **Счёт вместо отказа.** Каждый вариант и развилки 1, 2, 5, 6 — режимом
  проверки, который считает срабатывания и не отвергает программу. Корпус
  прогоняется один раз на все варианты. В выпуск остаётся только выбранное
  правило.
  `docs/direction/DECISIONS.md` Р24; `CLAUDE.md`, «Решения о языке»
- **Корпус — программы v0.1–v0.3.**
  - эталоны 19 задач `bench/tasks/*/dp/` (у 17 — `sol.dpm`, у t13 и t19 —
    `main.dpm`);
  - решения моделей в `bench/results/` — код, который ИИ пишет без правила;
  - `tests/` без тестов на ожидаемые отказы;
  - графическая программа v0.2 (Р64) и программа с задачами v0.3 (О13),
    когда появится;
  - атаки: пробы раздела 1 и 8 причин висячего вида.
- **Что мерить.**
  1. Срабатывания с разметкой: настоящая ошибка (тихая, ловушка, гонка,
     висячий вид) или ложный отказ (программа корректна и детерминирована).
  2. Пропуски на атаках — должны быть нулём (Р-5, К-4).
  3. Правка на каждый ложный отказ: сколько строк, применима ли машинно по
     подсказке, сохраняет ли смысл; доля строк обходов (Р-6).
  4. Цена для ИИ — для 1–2 ведущих вариантов: сессии моделей по задачам
     набора с правилом в руководстве; доля сессий с отказом О4, итерации до
     зелёного, токены на принятое изменение (мера Р-17).
  5. Цена выполнения: счёт снимков (`copy` перед видом в IR) на корпусе,
     время на нагрузках `BENCHMARKS.md`, выигрыш от выноса проверки длины.
  6. Компилятор: строк добавлено и удалено (Р-4), время `check`.
  7. О13: проходят ли t14 и программа с задачами без переписывания; TSan на
     сгенерированном C.
- **Порог.**
  - атаки и найденные пробами случаи — 100% ошибкой компиляции;
  - у каждого ложного отказа — подсказка с машинно применимой правкой;
  - обходы — меньше 5% строк вне привязок (Р-6);
  - токены на изменение — не выше нынешних в пределах разброса прогонов k2
    (Р-17);
  - компилятор после замены частных правил — не больше, чем до.

  Из вариантов, прошедших порог, — более простой (Р6, «Правила применения»).
- **Порядок.** Сначала статический прогон корпуса (дёшево), потом сессии
  моделей — только для прошедших порог.

## 6. Совет исследования (решает автор)

- **База — Б с путями А** и развилками 1 (поля и константные индексы) и 4
  (доступ после аргументов), по образцу Swift, Swiftlet и SPARK. Всё
  статично, внутри функции, без времён жизни. Закрывает все найденные случаи
  обхода и даёт О13 нужную длительность. Строгость обхода (развилка 5) — по
  счёту ложных отказов.
- **Снимок вида (В) — по счёту.** Если записи «вид левее действия» на
  корпусе редки — перейти к В: одна позиция по скрытым копиям (Р6), `snap_`
  уходит, часть E0513 входит в правило. Если часты — оставить снимок и
  записать его цену в спецификацию.
- **Г и Д не брать:** Г — ошибки только при запуске, против Р65, О13 не
  закрывает; Д — скрытые копии и обходы не закрывает.
- **`parallel for` — вместе с О4.** Правило записи в теле (развилка 6)
  решить до потоков: сейчас принимаются записи, которые с потоками станут
  гонками.
- **До работы** — если автор выберет Б или В: правка Р65 в `DECISIONS.md` с
  записью в историю (обход `List`; для В — ещё и вложенные вызовы).
  Независимо от выбора — выровнять LANGUAGE.md 2.2 со спецификацией и строку
  хэш-таблиц в `DETERMINISM.md` с Р54 и О12.

## Осторожно

- Большинство чисел раздела 3 — отчёты вендоров. «70%» Microsoft — данные
  2019 года; у Chromium дата среза не указана.
- Страница Wiz по CVE-2026-2441 помечена как сгенерированная ИИ, а её
  боковая панель даёт дату KEV «N/A» вопреки тексту. Даты сверять с Chrome
  Releases и каталогом CISA KEV.
- Racordon (2025) называет проверку исключительности в runtime Swift
  «атомарной», защищающей от гонок. Это расходится с SE-0176 («thread-local
  data structure») и манифестом («non-atomic operations», гонки ловить «not
  required»); на это место не опираться.
- Group borrowing (Valen — экспериментальный прототип; для Mojo — только
  предложение), проекции Hylo, view types Rust и Polonius Alpha — замыслы,
  эксперименты или невыпущенное. Брать идеи, а не гарантии.
- SPARK: пересечение `A(I)` и `A(J)` определено по значениям индексов, и
  правило проверяет GNATprove. Доказывает ли он `I /= J` или отвергает
  вызов, по RM не проверено.
- Замечание в тесте rustc «Should be accepted with g2p» описывает поведение
  отключённой ревизии, а не признанный и запланированный к исправлению
  отказ.
- Пробы dpl — на одной сборке. Вывод вложенных `&mut`-обходов зависит от
  входных данных. Замер rustc — учебные примеры, не код проекта.
