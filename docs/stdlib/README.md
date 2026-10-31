# Стандартная библиотека

Описание устройства стандартной библиотеки v0.1
(`ROADMAP/IMPLEMENTATION.md`, шаг 11; объём — Р19: маленькая). Код —
`stdlib/`. Библиотека v0.1 — столько, сколько нужно первой программе
(шаг 12). Остальное базовое встроено в язык: срезы, `List`, строки и их
методы, `str(x)` (`docs/spec/core.md`, раздел 9).

## 1. Устройство

- **Модуль — файл `stdlib/<имя>.dpm` на dpl** (Р46). Программа
  подключает его так же, как свой модуль: `import os`. Модуль ищется
  сначала рядом с главным модулем программы, затем в каталоге библиотеки
  (`docs/spec/core.md`, module.import).
- **Каталог библиотеки.** Для `dpl` это `$DPL_STDLIB`, по умолчанию
  `stdlib/` репозитория рядом со сборкой (`docs/compiler/cli.md`,
  раздел 1). У онлайн-сервиса библиотеки нет: он только проверяет
  программу (Р49).
- **Внутренние встроенные функции.** Ввод-вывод библиотеке даёт
  компилятор, FFI будет только в v0.2. Внутренние функции берут и
  возвращают простые типы; нормальный API — обёртки на dpl в модуле
  библиотеки.
  - Имена — с `__`.
  - Функции видны только модулям библиотеки (`ModuleInfo::stdlib`). Из
    программы их не найти: E0307.
  - Модуль библиотеки — тот, что найден в её каталоге. Главный модуль
    из этого каталога — тоже: так проверяется сама библиотека
    (`dpl check stdlib/os.dpm`).

| встроенная | что делает |
|---|---|
| `__args_len() i64` | число аргументов программы без её имени |
| `__arg(i: i64) str` | аргумент `i`; вне границ — ловушка |
| `__read_file(path: str, out: &mut str) i32` | 0 — текст файла в `out`; иначе код ошибки, и `out` не меняется |
| `__file_create(path: str) i64`, `__file_open(path: str) i64` | дескриптор файла (больше 0) на запись или чтение; иначе минус код ошибки |
| `__file_write(f: i64, data: []u8) i32` | 0 — все байты записаны; иначе код ошибки |
| `__file_read(f: i64, buf: &mut List[u8], n: i64) i64` | дописать в `buf` до `n` байтов; их число, 0 — конец файла; иначе минус код ошибки |
| `__file_close(f: i64) i32` | 0 — закрыт; иначе код ошибки (в том числе отложенной буфером записи) |

Код ошибки: 1 — `NotFound`, 2 — `PermissionDenied`, 3 — `InvalidUtf8`,
5 — `NoSpace`, остальное — `Other` (`os.io_error`).

В IR это `Intrinsic` `ArgsLen`, `ArgAt`, `ReadFile`, `FileCreate`,
`FileOpen`, `FileWrite`, `FileRead`, `FileClose` (`docs/ir/README.md`).
В runtime — `dp_args_len`, `dp_arg`, `dp_read_file`, `dp_file_*`
(`docs/runtime/README.md`). Аргументы программы runtime получает от
`main` C через `dp_set_args`.

## 2. Модуль `os` (Р51, Р71)

Аргументы программы и файлы. Ввод-вывод — недетерминированный класс
(Р12, Р29): из класса D его звать нельзя (E0425), из тела `parallel for`
и функций, которые оно зовёт, — тоже (E0517).

| объявление | что делает |
|---|---|
| `pub enum IoError { NotFound, PermissionDenied, InvalidUtf8, NoSpace, Other }` | ошибка ввода-вывода |
| `pub fn args() List[str]` | аргументы программы без её имени, по порядку |
| `pub fn read_file(path: str) IoError!str` | файл целиком, текст UTF-8 |
| `@linear pub struct File` | открытый файл: закрывается ровно один раз (ffi.linear) |
| `pub fn create(path: str) IoError!File` | новый файл на запись; существующий усекается |
| `pub fn open(path: str) IoError!File` | файл на чтение |
| `pub fn write(f: &mut File, data: []u8) IoError!void` | записать все байты `data` |
| `pub fn read(f: &mut File, buf: &mut List[u8], n: int) IoError!int` | дописать в `buf` до `n` байтов; их число, 0 — конец файла |
| `pub fn close(f: File) IoError!void` | закрыть; ошибка записи, отложенная буфером, — здесь |
| `pub fn read_bytes(path: str) IoError!List[u8]` | файл целиком байтами |
| `pub fn write_bytes(path: str, data: []u8) IoError!void` | записать файл целиком |
| `pub fn write_file(path: str, text: str) IoError!void` | записать текст в файл целиком |

Файл целиком (`read_bytes`, `write_bytes`, `write_file`) — обёртки на dpl
поверх `File`: держит в памяти весь файл. Большие данные пишутся и
читаются частями через `File` (Р71, замер — заметка
`docs/research/notes/11_big_data_io.md`).

`File`:
- дескриптор — номер в таблице открытых файлов runtime; `File`, собранный
  литералом вне `os` (литерал линейной структуры допустим, разбор — нет),
  или уже закрытый — ловушка `file is not open`, а не обращение к памяти;
- `read` с `n < 0` — ловушка `read count N is negative`; буфер больше
  предела `List` — ловушка `out of memory`;
- `NoSpace` — диск полон (`ENOSPC`, `EDQUOT`, `EFBIG`); при записи через
  буфер он может прийти только из `close`;
- в задачу (Р70) `File` передаётся переносом, как любое значение, и
  закрывается там.

Пример — запись частями. `errdefer` закрывает файл на любой ошибке, пока
файл открыт; `try os.close(f)` в конце его снимает (Р60, ffi.linear):

```
import os

fn save(path: str, chunks: &List[List[u8]]) os.IoError!void {
    var f = try os.create(path)
    errdefer _ = os.close(f)
    for c in &chunks {
        try os.write(&mut f, c)
    }
    try os.close(f)
}
```

`read_file`:
- путь — как у C `fopen`, относительный — от текущего каталога; байт 0
  в пути — `NotFound`;
- нет файла или каталога пути — `NotFound`; нет прав — `PermissionDenied`;
- текст не UTF-8 (обрезанные, избыточные записи, суррогаты, символы
  больше U+10FFFF) — `InvalidUtf8`;
- другое (например, путь — каталог) — `Other`;
- файл больше предела буфера `str` — ловушка `out of memory`
  (`trap.memory`).

Пример:

```
import os

fn main() os.IoError!void {
    for path in &os.args() {
        let text = try os.read_file(path)
        print(text)
    }
}
```

## 3. Проверки

- **Набор `run`, файлы** — `tests/run/os_files.dpm` (Р71): запись и
  чтение частями, файл целиком, усечение, пустой файл, биты `f64` через
  файл, ошибки `NotFound` и `InvalidUtf8`. Каталог для файлов — аргумент
  `{tmp}` (`os_files.args`): `tests/run.py` даёт каждому запуску свой
  пустой каталог и удаляет его в конце.
- **Набор `fail`** — `tests/fail/os_files.dpm` (E0511, E0512 у `File`),
  `tests/fail/os_files_par.dpm` (E0517 — файлы из тела `parallel for`).
- **Тесты runtime** — `runtime/trap/file_*`: чужой и закрытый дескриптор,
  отрицательное `n`.
- **Набор `run`** — `tests/run/os_io.dpm`. Его аргументы лежат в
  `os_io.args`, данные — в `tests/run/data/`. Проверяются: текст, пустой
  файл, нет файла, не UTF-8, каталог, ошибка из `main`. Та же программа
  идёт в проходах `native` и `build`, и вывод совпадает с эталонным
  интерпретатором.
- **Набор `fail`** — `tests/fail/internal.dpm`: внутренние встроенные не
  видны программе.
- **Снимок `cli`** — `tests/cli/check_stdlib_os.args`: библиотека
  проходит проверку сама.
- **Тесты runtime** (`tests/runtime/rt_test.c`) — `dp_utf8_valid` на
  граничных значениях.
- `PermissionDenied` не проверяется: тесты идут от root, а ему права
  файла не мешают.
