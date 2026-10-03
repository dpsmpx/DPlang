# t04. Пул с хэндлами и поколениями

Пул фиксированной ёмкости хранит значения `i32`. Наружу выдаётся хэндл
`Handle { index: i32, generation: i32 }`.

- Поколение слота начинается с 1 и увеличивается на 1 при каждом
  удалении.
- `create` занимает свободный слот с наименьшим индексом.
- Хэндл устарел, если слот свободен или поколение не совпадает.

## Интерфейс

- `pool_new(capacity: i32) -> Pool`
- `pool_create(p: &mut Pool, value: i32) -> ошибка Full | Handle`
- `pool_get(p: &Pool, h: Handle) -> optional i32`
- `pool_set(p: &mut Pool, h: Handle, value: i32) -> bool` — `false`, если устарел.
- `pool_destroy(p: &mut Pool, h: Handle) -> bool` — `false`, если устарел.
- `pool_count(p: &Pool) -> i32` — число живых.

## Проверки

Драйвер — `expected.txt`.
