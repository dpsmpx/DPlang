# t03. Конечный автомат поведения NPC

Состояния (сумма типов): `Idle`, `Patrol { waypoint: i32 }`,
`Chase { target: i32 }`, `Flee { ticks: i32 }`.
События (сумма типов): `Tick`, `SeeEnemy { id: i32 }`, `LoseEnemy`,
`LowHealth`.

Правила, по порядку приоритета:

1. `LowHealth` в любом состоянии → `Flee { ticks: 3 }`.
2. `SeeEnemy { id }` в любом состоянии, кроме `Flee`, → `Chase { target: id }`.
3. `Chase` + `LoseEnemy` → `Patrol { waypoint: 0 }`.
4. `Idle` + `Tick` → `Patrol { waypoint: 0 }`.
5. `Patrol { w }` + `Tick` → `Patrol { waypoint: (w + 1) % 4 }`.
6. `Flee { t }` + `Tick` → `Flee { ticks: t - 1 }`, если `t > 1`, иначе `Idle`.
7. Иначе состояние не меняется.

## Интерфейс

- `step(state: State, event: Event) -> State`
- `describe(state: State) -> str` — `idle`, `patrol 2`, `chase 7`, `flee 3`.

## Проверки

Драйвер подаёт последовательность событий и печатает состояние после
каждого (`expected.txt`).
