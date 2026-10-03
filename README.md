# C-

Свой язык программирования для продолжения разработки VoxelRPG: краткий
синтаксис и упор на работу с ИИ — экономия токенов, скорость сборки,
надёжность и проверяемость; сборка приложений для Android и Windows;
изменения языка не заставляют переписывать игру.

## Готовность

| что | состояние |
|---|---|
| постановка и требования | решены (`docs/direction/DECISIONS.md`, Р0–Р3) |
| исследование | сделано, предложения П1–П15 ждут решения автора |
| спецификация | нет |
| компилятор | нет |
| перенос VoxelRPG | не начат; только по решению в VoxelRPG |

Текущий этап — [`ROADMAP/ROADMAP.md`](ROADMAP/ROADMAP.md).

## Документы

- [`CLAUDE.md`](CLAUDE.md) — правила работы в репозитории.
- [`docs/direction/GOAL.md`](docs/direction/GOAL.md) — ради чего язык и как
  мерить цели.
- [`docs/direction/DECISIONS.md`](docs/direction/DECISIONS.md) — что
  решено.
- [`docs/research/LANGUAGE_RESEARCH.md`](docs/research/LANGUAGE_RESEARCH.md)
  — исследование: лучшее из других языков и предложения.
- [`TODO.md`](TODO.md) — что не закончено.

## Проверки

```sh
./tools/check/run.sh
```
