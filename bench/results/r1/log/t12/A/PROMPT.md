# Задача t12 на DPlang, вариант A

Задание опыта: написать код по постановке, зная язык только по спецификации в конце этого файла.

## Правила

- Работай только в папке `/home/user/C-/build/experiment/r1/t12/A`: читай этот файл и `src/`, правь файлы в `src/`. Других файлов не открывай, по диску и в сети не ищи.
- Компилятор и другие программы сам не запускай. Проверка — только командой

      python3 /home/user/C-/bench/experiment.py check /home/user/C-/build/experiment/r1/t12/A

  Она переводит код прототипом языка в C++, собирает его со скрытым драйвером, запускает и сравнивает вывод с ожидаемым; печатает «принято» или первую ошибку.
- Попыток проверки — не больше 5, каждая записывается. Проверяй, когда решение готово целиком, а не для разведки.
- Работа кончается на «принято» или когда попытки кончились. Ответ — одна строка: номер последней попытки и её итог.

## Код

В `src/sol.cm` — реализация с ошибками. Найди и исправь ошибки, не переписывая решение заново; сигнатуры не меняй.

Ошибки типов прототип находит при сборке переведённого C++: сообщение называет функцию, в которой ошибка, а текст — от компилятора C++.

## t12. Исправление ошибки: скользящее среднее

Дана реализация с ошибками (начальный код задачи). Нужно найти и
исправить ошибки, не переписывая решение заново: мера задачи — точность
правки.

### Что должно получаться

- `moving_average(values: срез i32, window: i32) -> список i32` — для
  каждого `i` от `0` до `len(values) - window` включительно: сумма
  `values[i .. i + window)` в `i64`, делённая на `window` с отбрасыванием
  дробной части к нулю.
- `window < 1` или `window > len(values)` — пустой список.

### Проверки

Драйвер — `expected.txt`.

## DPlang, вариант A — краткая спецификация

Подмножество языка DPlang для опыта с синтаксисом (v0.0). Пишите решение
только тем, что описано здесь. Решение переводится в C++ и собирается с
драйвером задачи; драйвер вызывает публичные функции и читает поля
публичных структур по именам из заготовки. Ловушка — выход за границы,
переполнение, деление на ноль, ни одна ветвь `match` не подошла — провал
задачи.

### 1. Лексика

- Комментарий — `//` до конца строки, только на отдельной строке.
- Имена: буквы, цифры, `_`; ключевые слова: `fn struct enum const import
  pub let var if else while for in match return break continue defer fail
  try parallel none true false mut`.
- Литералы: целые `42`, `0xFF`, `1_000`; с плавающей точкой `1.5`, `2e10`;
  строка `"text"` (экранирование `\n \t \\ \"`); байт `'a'` (тип `u8`);
  `true`, `false`; `none`.
- Каждая простая инструкция и объявление `const`, `import` кончаются
  на `;`. Поля структур и варианты перечислений разделяются `,`.

### 2. Объявления

Модуль — файл; объявления верхнего уровня: функции, структуры,
перечисления, константы, `import`. `pub` делает объявление видимым
драйверу и другим модулям.

```dplang
pub struct Item {
    id: i32,
    count: i32 = 1,
    name: str = "",
}

pub enum Event {
    Tick,
    Move(i32, i32),
    Hit { target: i32, damage: i32 },
}

const LIMIT: i32 = 64;

// Три режима параметров: изменяемый, только чтение, значение.
pub fn add_count(items: &mut List[Item], key: &Item, extra: i32) -> i32 {
    for it in &mut items {
        if it.id == key.id {
            it.count = min(it.count + extra, LIMIT);
            return it.count;
        }
    }
    return 0;
}

pub fn reset(items: &mut List[Item]) {
    items.clear();
}
```

- Поле структуры может иметь значение по умолчанию (`= выражение`).
- Варианты перечисления: без данных, с позиционными данными `V(T, U)`,
  с полями `V { f: T }`.
- Сигнатура: `fn имя(п: Т, ...) -> Р`; без результата — без `-> Р`.
- Режим параметра пишется перед типом: `T` — значение, `&T` — ссылка
  только для чтения, `&mut T` — изменяемая ссылка.
- Ссылки живут только в параметрах: в поле, переменной или результате
  ссылку хранить нельзя.

### 3. Типы

| тип | что это |
|---|---|
| `i8 i16 i32 i64`, `u8 u16 u32 u64` | целые фиксированной ширины |
| `int` | целое размера адреса (64 бита): тип `len` и целого литерала без типа |
| `f32 f64` | IEEE 754; литерал с точкой — `f64` |
| `bool` | `true`, `false` |
| `str` | строка байтов UTF-8; `s[i]` — `u8`, `s[a..b]` — `str` |
| `List[T]` | динамический массив, владеет элементами |
| `[]T` | срез — представление без владения; `List[T]` передаётся туда, где ждут `[]T` |
| `?T` | может не быть: значение или `none` |
| `E!T` | значение или ошибка из перечисления `E`; `!void` — только ошибка |
| `m.T` | тип `T` из модуля `m` |

Неявного `null` нет. Обобщённых типов, методов у своих типов, замыканий,
глобальных переменных (кроме `const`) и перегрузки операторов нет.

### 4. Переменные

```dplang
fn demo(xs: []i32) -> i64 {
    let n = len(xs);
    var sum: i64 = 0;
    var i = 0;
    while i < n {
        sum += i64(xs[i]);
        i += 1;
    }
    let first = xs[0];
    sum += i64(first);
    return sum;
}
```

- `let x = e` — неизменяемая, `var x = e` — изменяемая; тип можно
  указать: `var x: i32 = 0`.
- Целый литерал без типа даёт `int`, литерал с точкой — `f64`; для узких
  типов укажите тип или преобразуйте: `i32(n)`.
- Присваивание: `= += -= *= /= %= &= |= ^= <<= >>=`. `_ = e` — явно
  отбросить значение, в том числе результат с ошибкой.

### 5. Выражения

Приоритет — от слабого к сильному:

| уровень | операторы |
|---|---|
| 1 | `\|\|` |
| 2 | `&&` |
| 3 | `== != < <= > >=` — не сцепляются: `a < b < c` — ошибка |
| 4 | `??` — значение из `?T` или запасное: `opt ?? 0` |
| 5 | `+ - \| ^ +% -%` |
| 6 | `* / % << >> & *%` |
| 7 | унарные `- ! ~ try`, пометки `&x`, `&mut x` |

- `<<`, `>>` и `&` — того же уровня, что `*`: `x << 7 * i` значит
  `(x << 7) * i`. Ставьте скобки.
- Целые: переполнение — ловушка; `+% -% *%` — сложение, вычитание и
  умножение по модулю 2^N; `/` округляет к нулю; деление на ноль и сдвиг
  на ширину типа и больше — ловушка. В смешанной арифметике операнды
  приводятся к общему типу.
- Преобразование `T(x)` проверяется: значение вне `T` — ловушка
  (`f64` → целое отбрасывает дробь). `T.wrap(x)` — заворачивание в целый
  тип `T`: `u32.wrap(-1)` — `4294967295`.
- Индекс `a[i]` проверяется по границам; срезы `a[lo..hi]`, `a[..hi]`,
  `a[lo..]`.
- Строки: `+` — склейка, `==` — сравнение. `==` сравнивает и структуры
  и перечисления целиком; `<` у структур — по полям по порядку.
- Вызов: пометка аргумента совпадает с режимом параметра — `f(&x)`
  для `&T`, `f(&mut x)` для `&mut T`, без пометки для значения;
  несовпадение — ошибка.
- Литерал структуры: `Item { id: 3, name }` (`name` — то же, что
  `name: name`); поля со значением по умолчанию можно не писать.
- Вариант перечисления пишется с типом: `Event.Tick`,
  `Event.Move(1, 2)`, `Event.Hit { target: 3, damage: 9 }`.
- Литерал списка: `[1, 2, 3]`, пустой `[]` — тип из контекста.

### 6. Инструкции

```dplang
fn walk(path: []Event) -> i32 {
    var x = 0;
    var hits: List[i32] = [];
    for e in &path {
        match e {
            Event.Tick => continue,
            Event.Move(dx, _) => x += dx,
            Event.Hit { target, damage } => {
                if damage > 0 {
                    hits.push(target);
                }
            }
        }
    }
    if let last = hits.pop() {
        x += last;
    } else if x < 0 {
        x = 0;
    }
    for _ in 0..len(hits) {
        x -= 1;
    }
    return x;
}
```

- `if`, `else if`, `else`; условие без скобок. `if let v = opt { ... }`
  — ветвь, если значение есть; `v` — само значение.
- `while условие { ... }`, `break`, `continue`, `return`.
- `for i in a..b` — от `a` до `b - 1`; `for _ in a..b` — без переменной.
- `for x in &list` — читать элементы, `for x in &mut list` — изменять;
  по строке — байты `u8`.
- `parallel for i in a..b { ... }` — итерации в разных потоках. Итерация
  читает общие данные и пишет только своё (например, `part[i]`); итог не
  должен зависеть от порядка потоков.
- `match` — по значению: ветви `образец => инструкция,` или `образец => { ... }`.
  Образцы: `_`, литерал, имя (связывает значение), `none`, `some(p)`,
  `ok(p)`, `err(e)`, `Event.Move(p, q)`, `Event.Hit { target, damage: d }`, `Event.Tick`,
  `p | q` (без связывания имён). Ветви должны покрывать все случаи.
- `defer инструкция` или `defer { ... }` — выполнится при выходе из блока.

### 7. Ошибки

```dplang
pub enum ReadError {
    Empty,
    TooLong,
}

pub fn read_name(s: str) -> ReadError!str {
    let t = s.trim();
    if len(t) == 0 {
        fail ReadError.Empty;
    }
    if len(t) > 16 {
        fail ReadError.TooLong;
    }
    return t;
}

pub fn greet(s: str) -> ReadError!str {
    let name = try read_name(s);
    return "hi " + name;
}

pub fn count_valid(names: []str) -> i32 {
    var ok_count: i32 = 0;
    for s in &names {
        match read_name(s) {
            ok(_) => ok_count += 1,
            err(e) => {
                if e == ReadError.TooLong {
                    ok_count -= 1;
                }
            }
        }
    }
    return ok_count;
}
```

- Ошибка — значение: вариант перечисления ошибок. Исключений нет.
- `fail E.V` возвращает ошибку из функции с результатом `E!T`.
- `try f()` — значение или немедленный возврат той же ошибки.
- `match` с `ok(v)` и `err(e)`; `e == E.V` — проверить вариант.
- Молча проигнорировать результат с ошибкой нельзя: обработайте его,
  передайте `try` или отбросьте явно: `_ = f()`.
- Драйвер различает ошибки по имени варианта.

### 8. Встроенное

- `len(x)` — длина списка, среза или строки (`int`).
- `repeat(v, n)` — список из `n` копий `v`; тип — из контекста или из `v`.
- `min(a, b)`, `max(a, b)`, `abs(a)`; `str(x)` — запись целого или
  `bool` строкой; `parse_int(s)` — `?i64` из десятичной записи со знаком.
- `is_digit(b)`, `is_alpha(b)`, `is_space(b)`, `is_alnum(b)` — для `u8`.
- Список: `xs.push(v)`, `xs.pop()` (`?T`), `xs.insert(i, v)`,
  `xs.remove(i)` (`T`), `xs.clear()`, `xs.sort()` (устойчиво, по
  возрастанию), `xs.contains(v)`.
- Строка: `s.trim()`, `s.find(t)` (`?int`), `s.starts_with(t)`,
  `s.split(sep)` (`List[str]`).
- `assert(условие)` — ловушка, если ложно.

### 9. Модули

Файл `world.cm` — модуль `world`. В другом модуле:
`import world;`, затем `world.spawn(...)`, тип — `world.World`. Видно
только `pub`.

### Приложение. Образцовые программы

#### s1_shapes

```dplang
// Фигуры: перечисление с данными, полный разбор match, плавающая точка.
pub enum Shape {
    Circle { r: f64 },
    Rect { w: f64, h: f64 },
    Square(f64),
}

const PI: f64 = 3.141592653589793;

pub fn area(s: Shape) -> f64 {
    match s {
        Shape.Circle { r } => return PI * r * r,
        Shape.Rect { w, h } => return w * h,
        Shape.Square(a) => return a * a,
    }
}

pub fn name(s: Shape) -> str {
    match s {
        Shape.Circle => return "circle",
        Shape.Rect => return "rect",
        Shape.Square => return "square",
    }
}

// Фигура с наибольшей площадью; нет значения для пустого списка.
pub fn largest(shapes: []Shape) -> ?Shape {
    if len(shapes) == 0 {
        return none;
    }
    var best = shapes[0];
    for s in &shapes {
        if area(s) > area(best) {
            best = s;
        }
    }
    return best;
}

pub fn scale(s: Shape, k: f64) -> Shape {
    match s {
        Shape.Circle { r } => return Shape.Circle { r: r * k },
        Shape.Rect { w, h } => return Shape.Rect { w: w * k, h: h * k },
        Shape.Square(a) => return Shape.Square(a * k),
    }
}
```

#### s2_duration

```dplang
// Разбор длительности «1h30m15s»: строки, байты, ошибки-значения, try.
pub enum DurationError {
    Empty,
    BadNumber,
    BadUnit,
}

fn unit_seconds(c: u8) -> ?i64 {
    match c {
        'h' => return 3600,
        'm' => return 60,
        's' => return 1,
        _ => return none,
    }
}

// Части — число и единица h, m или s; пробелы по краям не считаются.
pub fn parse_duration(text: str) -> DurationError!i64 {
    let s = text.trim();
    if len(s) == 0 {
        fail DurationError.Empty;
    }
    var total: i64 = 0;
    var i = 0;
    while i < len(s) {
        let start = i;
        while i < len(s) && is_digit(s[i]) {
            i += 1;
        }
        if i == start || i == len(s) {
            fail DurationError.BadNumber;
        }
        let n = parse_int(s[start..i]) ?? -1;
        if n < 0 {
            fail DurationError.BadNumber;
        }
        if let k = unit_seconds(s[i]) {
            total += n * k;
        } else {
            fail DurationError.BadUnit;
        }
        i += 1;
    }
    return total;
}

// Сумма длительностей; первая ошибка прерывает подсчёт.
pub fn total_seconds(items: []str) -> DurationError!i64 {
    var sum: i64 = 0;
    for item in &items {
        sum += try parse_duration(item);
    }
    return sum;
}

pub fn format_duration(seconds: i64) -> str {
    let h = seconds / 3600;
    let m = seconds % 3600 / 60;
    let s = seconds % 60;
    var out = "";
    if h > 0 {
        out += str(h) + "h";
    }
    if m > 0 {
        out += str(m) + "m";
    }
    if s > 0 || out == "" {
        out += str(s) + "s";
    }
    return out;
}
```

#### s3_bank

```dplang
// Счета и переводы: список структур, изменяемые ссылки, ошибки без
// изменения состояния, разбор результата в match, заворачивающая
// арифметика.
pub struct Account {
    id: i32,
    balance: i64,
}

pub enum BankError {
    NoAccount,
    Insufficient,
    BadAmount,
}

pub struct Bank {
    accounts: List[Account],
    log: List[str],
}

pub struct Op {
    from: i32,
    to: i32,
    amount: i64,
}

pub fn bank_new() -> Bank {
    return Bank { accounts: [], log: [] };
}

pub fn open(b: &mut Bank, id: i32, initial: i64) -> BankError!void {
    if initial < 0 {
        fail BankError.BadAmount;
    }
    b.accounts.push(Account { id, balance: initial });
    b.log.push("open " + str(id));
}

fn find(b: &Bank, id: i32) -> ?int {
    for i in 0..len(b.accounts) {
        if b.accounts[i].id == id {
            return i;
        }
    }
    return none;
}

// Перевод; при ошибке состояние не меняется.
pub fn transfer(b: &mut Bank, from: i32, to: i32, amount: i64) -> BankError!void {
    if amount <= 0 {
        fail BankError.BadAmount;
    }
    let f = find(&b, from) ?? -1;
    let t = find(&b, to) ?? -1;
    if f < 0 || t < 0 {
        fail BankError.NoAccount;
    }
    if b.accounts[f].balance < amount {
        fail BankError.Insufficient;
    }
    b.accounts[f].balance -= amount;
    b.accounts[t].balance += amount;
    b.log.push("transfer " + str(from) + "->" + str(to) + " " + str(amount));
}

// Переводы по порядку; возвращает число отклонённых.
pub fn apply_all(b: &mut Bank, ops: []Op) -> i32 {
    var rejected: i32 = 0;
    for op in &ops {
        match transfer(&mut b, op.from, op.to, op.amount) {
            ok(_) => {}
            err(e) => {
                rejected += 1;
                if e == BankError.Insufficient {
                    b.log.push("insufficient " + str(op.from));
                }
            }
        }
    }
    return rejected;
}

pub fn total(b: &Bank) -> i64 {
    var sum: i64 = 0;
    for a in &b.accounts {
        sum += a.balance;
    }
    return sum;
}

// FNV-1a по балансам: умножение по модулю 2^64 — явной операцией *%.
pub fn checksum(b: &Bank) -> u64 {
    var h: u64 = 14695981039346656037;
    for a in &b.accounts {
        h = (h ^ u64.wrap(a.balance)) *% 1099511628211;
    }
    return h;
}
```
