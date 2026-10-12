# triangle

Графическая программа v0.2 (шаг 7, Р64): SDL3 и Vulkan через привязки
`bindings/` (`docs/bindings/README.md`). Треугольник шейдером рисуется в
кадр 64×64 вне окна. Кадр копируется в окно через цепочку показа Vulkan,
читается в память и сверяется с эталоном `reference/frame.ppm`.

```
triangle frame.ppm   # один кадр: снимок в файл и сверка
triangle             # окно до закрытия
```

- **Кадр одинаков на любом драйвере.** Цвета сплошные, а их значения
  переводятся в UNORM без округлений на границе. Вершины лежат на углах
  пикселей, и ни один центр пикселя не попадает на ребро. Треугольник —
  1152 пикселя (½·48·48); контрольная сумма FNV-1a пикселей RGB — константа
  `REFERENCE` в `main.dpm`.
- **Шейдеры** — `shaders/triangle.vert`, `shaders/triangle.frag`; их SPIR-V
  лежит константами в `shaders.dpm`. Пересборка: `python3
  examples/triangle/shaders/build.py` (нужен `glslangValidator`, пакет
  `glslang-tools`).
- **Показ в окне** — если у видео SDL есть расширение поверхности Vulkan:
  `offscreen` — `VK_EXT_headless_surface`, `windows`, `android`, `x11`,
  `wayland`. Без него кадр рисуется только вне окна.
- **Проверка** — случай `bindings/triangle/run` в `tests/run.py`
  (`tests/bindings.py`). Видео SDL — `offscreen`, Vulkan — lavapipe;
  снимок побайтно равен эталону. Библиотеки ставит
  `tools/setup/host_libs.sh`.
- **Вид кода** — не канонический `dpl fmt`: `fmt` пока не переносит
  длинные строки и свернул бы каждую структуру Vulkan в одну (`TODO.md`).

Сборка и запуск на Linux:

```
dpl build -o triangle main.dpm
SDL_VIDEO_DRIVER=offscreen ./triangle frame.ppm
```
