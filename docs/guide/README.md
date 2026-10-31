# dpl: краткое руководство

Как писать на dpl: приёмы с примерами. Нормативный текст —
`docs/spec/core.md` (ядро v0.1). Если руководство и спецификация
расходятся, права спецификация; руководство тогда исправляется.

## Файл, модуль, программа

```dpl
// Файл world.dpm — модуль world. Комментарий — от // до конца строки.
import os                        // модуль рядом с главным или из стандартной библиотеки

pub struct Unit {                // pub — видно другим модулям
    hp: i32
    alive: bool = true           // поле со значением по умолчанию
}

const LIMIT: i32 = 100

pub fn spawn(units: &mut List[Unit], hp: i32) int {
    units.push(Unit { hp })      // alive — по умолчанию; `hp` — то же, что `hp: hp`
    return len(units) - 1
}

fn main() {                      // вход программы — main главного модуля
    var units: List[Unit] = []
    let i = spawn(&mut units, 5)
    println("unit " + str(i))
}
```

- Инструкция занимает строку. `;` нет. Порядок объявлений не важен.
- Из другого модуля имена берутся через модуль: `world.spawn(...)`,
  `world.Unit { hp: 1 }`, `world.Kind.Wall`. Видно только `pub`.
- `main` без параметров, без результата или с `E!void`. Ошибка из `main` —
  код выхода 1.

## Переменные и типы

```dpl
let a = 5                // неизменяемая; тип int (= i64)
var b: i32 = 0           // изменяемая
var c: str               // без значения — присвоить до чтения
b += 1
```

- Типы: `i8`…`i64`, `u8`…`u64`, `int` (= `i64`), `f32`, `f64`, `bool`,
  `str` (байты UTF-8), `List[T]`, `Map[K, V]`, `?T`, `E!T`, свои `struct`
  и `enum`.
- Литерал берёт тип из контекста: в `b + 1` единица — `i32`.
- **Неявно только без потерь:** `i32 → i64`, `u8 → i32`, `f32 → f64`.
  Остальное — явно: `i32(x)`, `u8(n)`, `f64(n)`, `int(x)`.
  `len(xs)` — `int`, поэтому `let n: i32 = i32(len(xs))`.
- **Затенения нет:** имя не повторяет другое локальное имя, видимое в
  функции (E0310). Возьмите другое имя.
- Значения копируются: присваивание и передача `List`, `Map` и `str` копируют
  содержимое. Общих изменяемых данных нет.

## Целые и переполнение

- `+ - *`, унарный минус и `T(x)` вне диапазона — **ловушка**: программа
  останавливается с кодом 101 и местом в исходнике.
- `/` отбрасывает дробную часть к нулю, знак `%` — знак делимого.
  Деление на 0 — ловушка.
- Арифметика по модулю 2^N — `+%`, `-%`, `*%` и `T.wrap(x)`
  (`u32.wrap(-1)` — 4294967295). Сдвиги `<<`, `>>`, биты `& | ^ ~`.
- Проверка без ловушки — `checked_add`, `checked_sub`, `checked_mul`,
  `checked_div`, `checked_neg`: результат `?T`, `none` — если вне типа
  (у `checked_div` — и при делителе 0).

```dpl
fn total(xs: []i64) ?i64 {
    var sum: i64 = 0
    for x in &xs {
        if let s = checked_add(sum, x) {
            sum = s
        } else {
            return none                  // переполнение — данные, не ловушка
        }
    }
    return sum
}

let q = checked_div(a, b) ?? 0           // делитель 0 — none
let m: i32 = 2147483647
let bad = checked_mul(m, 2) == none      // true: вне i32
```

## Строки и байты

```dpl
let s = "a,b,,c"
let first: u8 = s[0]             // байт; литерал байта — 'a', '\n'
let part = s[2..3]               // срез — новая строка; s[..2], s[2..]
let parts = s.split(",")         // ["a", "b", "", "c"]
let t = "  x ".trim()            // "x"
if let at = s.find(",") {        // ?int — позиция первого вхождения
    println(s[..at])
}
var out = ""
out += "n=" + str(42)            // str(x) — запись числа, bool, строки
for c in &s {                    // байты строки
    if is_digit(c) { ... }       // is_alpha, is_space, is_alnum
}
let n = parse_int("-17") ?? 0    // ?i64: знак и цифры, иначе none
```

- Сравнение строк — `==`, `<` (по байтам). Строка неизменяема:
  новая строка — склейкой `+` или срезом.
- Байт в строку: `s[i..i + 1]`. Число байта — `str(c)`.

## Списки

```dpl
var xs: List[i32] = []
xs.push(3)
xs.insert(0, 1)                  // в позицию
let last = xs.pop()              // ?i32
let y = xs.remove(0)             // i32; индекс вне — ловушка
xs.sort()                        // устойчиво; структуры — по полям по порядку
let zeros = repeat(0, 8)         // List из 8 копий
let has = xs.contains(3)
for i in 0..len(xs) { xs[i] += 1 }
for x in &xs { println(str(x)) } // только чтение
for x in &mut xs { x *= 2 }      // изменение элементов
```

- Индекс вне диапазона — ловушка. Срез `xs[a..b]` — представление
  `[]T` только для передачи в функцию с параметром `[]T`.
- Вложенные списки: `var grid: List[List[i32]] = repeat(repeat(0, w), h)`,
  `grid[y][x] = 1`.
- `xs.clear()` оставляет ёмкость, `xs.reserve(n)` — ёмкость заранее.
  Временный список кадра держат между кадрами и очищают `clear()`:
  после прогрева он не выделяет памяти.

## Отображения `Map[K, V]`

```dpl
var bal: Map[str, i64] = []      // только пустой литерал
bal["bob"] = 5                   // вставка или замена
bal["bob"] += 2                  // ключ должен быть: иначе ловушка
let x = bal["ann"]               // нет ключа — ловушка
let y = bal.get("ann") ?? 0      // ?V, без ловушки
if !bal.contains("ann") { bal["ann"] = 0 }
let old = bal.remove("bob")      // ?V; порядок остальных сохраняется
for k in &bal { println(k + " " + str(bal[k])) }   // ключи по возрастанию
for k in &bal.keys() { bal[k] = 0 }                 // менять — в обходе копии ключей
```

- Ключ — целое, `bool` или `str`. Обход и `keys()` — всегда по
  возрастанию ключа: сортировать вывод не нужно.
- В теле `for k in &m` менять `m` нельзя (E0412).
- `len(m)` — число ключей; `m.clear()` — очистить. Сравнивать
  отображения нельзя.

## Пулы `Pool[T]` и хэндлы `Handle[T]`

Долгоживущие ссылки на объекты (сущности, снаряды, ресурсы по номеру) —
хэндлы пула: устаревший хэндл обнаруживается, а не читает чужое.

```dpl
struct Enemy { hp: i32 }
var enemies: Pool[Enemy] = []    // только пустой литерал
let h = enemies.add(Enemy{hp: 5})       // Handle[Enemy]
enemies[h].hp -= 2               // место; устаревший хэндл — ловушка
if let e = enemies.get(h) { println(str(e.hp)) }   // ?T, без ловушки
let alive = enemies.contains(h)
let gone = enemies.remove(h)     // ?Enemy; h теперь устарел
for e in &mut enemies { e.hp += 1 }                 // живые по номеру слота
for x in &enemies.handles() {    // add и remove — в обходе копии хэндлов
    if enemies[x].hp <= 0 { let _ = enemies.remove(x) }
}
```

- Хэндл — номер слота и поколение: `h.index`, `h.generation` (`u32`,
  только чтение). Хэндлы сравниваются `==`; ключом `Map` не бывают.
- `add` берёт свободный слот с наименьшим номером; поколение слота растёт
  при `remove` и `clear`, поэтому старый хэндл того же слота — устаревший.
- `Handle[Enemy]` нельзя подать в `Pool[Item]` — ошибка типа.
- `len(p)` — число живых; `p.clear()` освобождает все слоты, ёмкость
  остаётся — пул годится для объектов кадра.
- Свой пул на `List` писать не нужно: проверка поколения встроена.

## Функции и ссылки

```dpl
fn total(xs: []i32) int { ... }          // срез: принимает List
fn bump(c: &mut Counter, by: i32) { c.n += by }
fn show(c: &Counter) str { return str(c.n) }

var c = Counter { n: 0 }
bump(&mut c, 2)                  // пометка у аргумента — как у параметра
println(show(&c))
println(str(total(xs)))
```

- `&T` — чтение без копии, `&mut T` — изменение. Ссылки бывают только у
  параметров; вернуть или сохранить ссылку нельзя.
- В одном вызове нельзя передать `&mut x` вместе с другой ссылкой на
  то же место `x` (E0502); разные поля и постоянные индексы можно:
  `swap(&mut p.x, &mut p.y)`.
- Внутри функции параметр `&mut` пишется как обычная переменная:
  `c.n += 1`, `xs.push(v)`; дальше передаётся снова с `&mut`.

## Обход и изменение

```dpl
for x in &mut xs { x += 1 }                   // менять — через переменную цикла
for i in 0..len(xs) { xs.push(xs[i]) }        // менять сам xs — обходом индексов
let first = xs[0]
for x in &mut xs { x += first }               // читать xs — до цикла
let old = xs
println(str(sum(old, grow(&mut xs))))         // вид и изменение xs в одном вызове — через копию
```

- Пока идёт `for x in &xs` или `for x in &mut xs`, сам `xs` менять нельзя,
  а в `&mut`-обходе — и читать: только через `x` (E0412). Другое поле той
  же структуры можно: `for b in &mut w.bodies { w.hits += 1 }`.
- Срез `xs[a..b]`, список `xs` как срез или `&xs[i]` в аргументе и
  изменение `xs` правее в том же выражении — ошибка (E0502): сначала
  копия `let old = xs`.
- В `parallel for i in 0..n` снаружи пишется и читается только `out[i]`
  по переменной цикла (E0412); итоги складываются после цикла. Из тела
  не выходят — `break`, `return`, `try` (E0516); файлы и C — до или
  после цикла (E0517).

## Структуры и перечисления

```dpl
pub struct Point {
    x: i32
    y: i32
}

pub enum Shape {
    Empty
    Circle(Point, i32)               // позиционные данные
    Rect { min: Point, max: Point }  // поля
}

let p = Point { x: 1, y: 2 }
let s: Shape = .Circle(p, 5)         // .V — когда тип известен из контекста
let r = Shape.Rect { min: p, max: Point { x: 3, y: 4 } }
var q = p
q.x = 10                             // q — копия
```

- Свой тип не содержит себя напрямую (E0423). Дерево — в `List`:
  `children: List[Node]` или узлы в одном списке с индексами.
- `==`, `<` работают для структур, перечислений и списков по частям.

## `match`

```dpl
match s {
    .Empty => println("empty")
    .Circle(c, r) => println(str(r))
    .Rect { min, max } => {
        println(str(max.x - min.x))
    }
}
match code {
    0 => println("zero")
    1 | 2 => println("small")
    _ => println("other")
}
```

- Ветви покрывают все значения (E0415): все варианты, `some` и `none`,
  `ok` и `err`; для чисел и строк нужна ветвь `_`.
- Полнота не смотрит во вложенные образцы. Вместо
  `ok(.Pair { … })` пишите `ok(v) => match v { … }`.

## «Может не быть»: `?T`

```dpl
fn find(xs: []i32, v: i32) ?int {
    for i in 0..len(xs) {
        if xs[i] == v {
            return i                 // T оборачивается в ?T сам
        }
    }
    return none
}

if let i = find(xs, 3) { println(str(i)) } else { println("нет") }
let j = find(xs, 4) ?? -1
match find(xs, 5) {
    some(i) => println(str(i))
    none => println("нет")
}
```

`opt == none` и `opt != none` — проверка без распаковки.

## Ошибки: `E!T`

```dpl
pub enum ParseError {
    Empty
    BadDigit
}

pub fn digit(s: str) ParseError!i32 {    // pub — тип ошибки пишется явно
    if len(s) == 0 {
        fail .Empty
    }
    if !is_digit(s[0]) {
        fail .BadDigit
    }
    return i32(s[0] - '0')
}

fn twice(s: str) ParseError!i32 {
    let d = try digit(s)                 // ошибка — сразу наверх
    return d * 2
}

match digit("7") {
    ok(d) => println(str(d))
    err(e) => {
        if e == .Empty { println("пусто") } else { println("не цифра") }
    }
}
_ = digit("x")                           // отбросить результат — только явно
```

- `E!void` — успех без значения: функция просто доходит до конца или
  делает `return`.
- Непубличная функция может писать `!T`: тип ошибки выводится из `fail`
  и `try` в теле.
- Результат `E!T` нельзя потерять молча (E0506): `try`, `match` или
  `_ =`. Сравнивать результат `E!T` целиком нельзя — только вариант
  ошибки `e == .V` внутри `err(e)`.

## Циклы и прочее

```dpl
while i < n { i += 1 }
for i in 0..n { if i == 3 { continue } }
for i in i32(0)..n32 { ... }             // тип переменной — общий тип границ
defer println("выход")                   // при выходе из блока, в обратном порядке
assert(n >= 0)                           // ложь — ловушка
let m = min(a, b) + max(a, b) + abs(d)
```

- Функция с результатом не должна дойти до конца без `return` или
  `fail` (E0504).
- `print`, `println` — в stdout; `eprintln` — в stderr.
- Плавающие: `str(0.1)` — `0.1`, `str(1e16)` — `1e+16` (кратчайшая
  запись, Р48).

## Потоки: `parallel for`, `tasks`, общие значения

`parallel for` — итерации одновременно: тело пишет снаружи только
`out[i]` по переменной диапазона (у обхода `&mut xs` — сам элемент), не
выходит из цикла, не трогает файлы и C (выше, «Обход и изменение»).

Задачи — блок `tasks`: `spawn f(…)` — вызов функции dpl в своём потоке,
блок кончается, когда кончились его задачи (Р70).

```dpl
var sums: List[i64] = repeat(i64(0), workers)
let next = atomic(i64(0))
tasks {
    for w in 0..workers {
        spawn work(&jobs, &next, &mut sums[w])
    }
    spawn log_progress(&next)      // код блока тоже идёт одновременно
}
// здесь все задачи кончились
```

- Функция задачи ничего не возвращает: итог — через `&mut`.
- `&x`, `&mut x` — только переменных до блока. Место `&mut` не трогает
  больше никто, место `&` никто не меняет — ни задачи, ни код блока
  (E0521). В цикле — `&mut out[w]` по переменной диапазона.
- Из блока не выходят раньше (E0516): отмена — общий флаг
  `Atomic[bool]`, который задачи проверяют сами.
- Вывод задачи выходит после блока, по порядку запуска.

Общие значения создаются встроенными и передаются только по `&` (копий
нет, E0518):

| значение | создание | методы |
|---|---|---|
| `Atomic[T]` (целое, `bool`) | `let c = atomic(i64(0))` | `load`, `store`, `add` и `sub` (→ прежнее), `swap`, `cas(old, new)` |
| `Mutex[T]` | `let m = mutex(v)` | блок `lock m as t { … }`: `t` — данные до конца блока |
| `Channel[T]` | `let ch: Channel[T] = channel(n)` | `send(v)` (ждёт места), `recv()` → `?T` (`none` — закрыт и пуст), `try_recv()`, `close()` |

Повторный `lock` тем же потоком, отправка в закрытый канал, повторный
`close` и «все потоки ждут» — ловушки. Дескриптор C (`extern type`) в
задачу — только если тип помечен `@send`.

## Детерминированный код: `@det`

```dpl
@det import physics          // весь модуль physics — класс D

@det
fn score(xs: []i64) i64 { ... }   // одна функция — класс D
```

- Класс D даёт один и тот же результат на всех целях побитово. Из него
  нельзя `print`, `println`, `eprintln` и `os` (E0425): верните значение
  и выводите его снаружи.
- Функция класса D вызывает только функции класса D.

## Функции C

```dpl
@link("m")
extern fn floor(x: f64) f64          // функция C: строка без тела
extern fn strlen(s: str) u64          // str — копия с нулём в конце
extern type File                      // дескриптор C, без операций
extern fn fopen(path: str, mode: str) ?File
@c
struct Size {                         // раскладка C
    w: u32
    h: u32
}
```

- Указателей нет: буфер — `[]T` (чтение), `&mut List[T]` (запись),
  значение — `&T`, `&mut T`. Длина — отдельным параметром, как в C.
- Функцию C нельзя вызвать из `@det`. `dpl debug run` её не исполняет —
  только `dpl build`.

## Ресурсы: `@linear` и `errdefer`

```dpl
@linear
struct Buf {                          // ресурс: закрыть ровно один раз
    mem: Mem
    size: u64
}

fn release(b: Buf) {
    let Buf { mem, size: _ } = b      // разбор — только в модуле Buf
    free(mem)
}

fn filled(n: u64) BufError!Buf {
    let b = try alloc(n)
    errdefer release(b)               // только если функция вернёт ошибку
    try fill(&b, n)                   // по ссылке ресурс не забирается
    return b
}

let b = try filled(16)
defer release(b)                      // закроется при выходе из блока
```

- Значение `@linear` забирают ровно один раз на каждом пути: передачей
  по значению, `return`, `let c = b`. Забыли — E0511, забрали дважды
  или читаете после — E0512.
- В цикле нельзя закрыть ресурс, открытый до цикла (E0513): закройте
  после цикла или откройте внутри.
- Поле `@linear` не забирают отдельно и не заменяют (E0513) — только
  разбором `let S { ... } = v`.
- Ресурс не кладут в `List`, `Map` и данные варианта (E0514); поле с
  ресурсом — только у структуры `@linear`.

## Файлы: `os` и `os.File`

```dpl
import os

fn copy_file(from: str, to: str) os.IoError!void {
    let data = try os.read_bytes(from)        // файл целиком: List[u8]
    try os.write_bytes(to, data)              // новый или усечённый
}

fn save(path: str, parts: &List[List[u8]]) os.IoError!void {
    var f = try os.create(path)               // os.File — @linear
    var failed: ?os.IoError = none
    for p in &parts {
        match os.write(&mut f, p) {           // по ссылке файл не забирается
            ok(_) => {}
            err(e) => {
                failed = e
                break
            }
        }
    }
    if let e = failed {
        _ = os.close(f)
        fail e
    }
    try os.close(f)                           // ошибка записи на диск — здесь
}
```

- Маленькие файлы — целиком: `os.read_file` (текст UTF-8),
  `os.read_bytes`, `os.write_bytes`, `os.write_file`. Большие — частями
  через `os.File`: `create` или `open`, `write`, `read(&mut f, &mut buf,
  n)` (дописывает до `n` байтов, 0 — конец), `close`.
- `File` закрывают ровно один раз (E0511, E0512); в цикле, открытый до
  него, — нельзя (E0513): ошибку запомните, выйдите из цикла и закройте
  после.
- Ошибки — `os.IoError`: `NotFound`, `PermissionDenied`, `InvalidUtf8`,
  `NoSpace`, `Other`. Из `@det` и тела `parallel for` файлов нет (E0425,
  E0517): прочтите до, запишите после.
- Числа в байты — сдвигами от младшего байта; плавающие — через
  `f64.bits(x)` и `f64.from_bits(u)` (у `f32` — то же).

## Чего в dpl нет и чем заменить

| нет | вместо этого |
|---|---|
| методов у своих типов, `impl` | функции: `fn area(s: &Shape) int` |
| обобщённых функций и своих обобщённых типов | отдельная функция на тип; для объектов по номеру — `Pool[T]` |
| замыканий и лямбд | именованная функция |
| кортежей | структура: `struct Pair { a: int, b: int }` |
| `if` и `match` как выражения, `?:` | `var x = …`, затем присваивание в ветвях |
| форматной строки | склейка: `"x=" + str(x)` |
| типа символа | байт `u8`; текст — `str` |
| `++`, `--`, `loop` | `i += 1`, `while true` |
| изменяемых глобальных | `const` или параметр `&mut` |
| исключений и `null` | `E!T` с `try`, `?T` с `none` |

- `&` берётся только у переменной, поля или элемента. Результат вызова
  сначала кладётся в `let`: `let d = f()`, затем `g(&d)`. Исключение —
  обход: `for k in &m.keys()`.

## Частые приёмы

```dpl
fn words(s: str) List[str] {             // split(" ") даёт "" между пробелами подряд
    var out: List[str] = []
    for w in &s.split(" ") {
        if len(w) > 0 { out.push(w) }
    }
    return out
}

fn join(parts: []str, sep: str) str {
    var out = ""
    for i in 0..len(parts) {
        if i > 0 { out += sep }
        out += parts[i]
    }
    return out
}

fn index_of(xs: []str, v: str) ?int {
    for i in 0..len(xs) {
        if xs[i] == v { return i }
    }
    return none
}
```

- Список из значений — литерал: `[x]`, `[a, b]`. `n` пробелов —
  `for i in 0..n { s += " " }`.
- `?T` сравнивается со значением: `peek(&p) == '+'`, `m.get(k) == 3`.
- Счётчик: `counts[w] = (counts.get(w) ?? 0) + 1`.
- Снимок и откат — копией: `let saved = state` … `state = saved`.
  Стек снимков — `List[State]`, откат — `if let s = stack.pop() { state = s }`.
- Разбор строки — структура с позицией и функции с `&mut`; ошибка —
  `E!T` и `try`:

```dpl
struct Parser {
    src: str
    pos: int
}

fn peek(p: &Parser) ?u8 {
    if p.pos < len(p.src) { return p.src[p.pos] }
    return none
}

fn number(p: &mut Parser) ParseError!i64 {
    let start = p.pos
    while p.pos < len(p.src) && is_digit(p.src[p.pos]) { p.pos += 1 }
    if p.pos == start { fail .Expected }
    if let v = parse_int(p.src[start..p.pos]) { return v }
    fail .TooBig                               // больше i64
}

fn sum(p: &mut Parser) ParseError!i64 {
    var total = try number(&mut p)
    while peek(&p) == '+' {
        p.pos += 1
        let n = try number(&mut p)
        if let t = checked_add(total, n) { total = t } else { fail .TooBig }
    }
    return total
}
```

## Частые ошибки компилятора

| код | что значит | как исправить |
|---|---|---|
| E0401 | типы не сходятся (часто `int` и `i32`) | явное преобразование `i32(x)` |
| E0408 | пометка аргумента не та | `&x` для `&T`, `&mut x` для `&mut T`, без пометки — значение |
| E0504 | функция может дойти до конца без `return` | `return` в конце |
| E0506 | потерян результат `E!T` | `try`, `match` или `_ =` |
| E0310 | имя уже есть в функции | другое имя |
| E0412 | изменение неизменяемого или коллекции в её обходе | `var` вместо `let`, параметр `&mut`; обход индексов |
| E0415 | `match` не покрывает все случаи | ветвь `_` или недостающий вариант |
| E0308 | у `pub fn` нет типа ошибки | `MyError!T` вместо `!T` |
| E0511 | ресурс `@linear` не закрыт на каком-то пути | закрыть, `defer close(r)`; на ошибке — `errdefer` |
| E0512 | ресурс уже закрыт или перенесён | передавать по ссылке `&r` |

Объяснение любого кода — `dpl explain E0401`.
