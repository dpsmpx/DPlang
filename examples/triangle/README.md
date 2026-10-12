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

**Windows** — `dpl build --target windows-x86_64 -o triangle.exe
main.dpm`, рядом — `SDL3.dll`. Нужны SDL3 для MinGW и библиотека импорта
`vulkan-1.dll` (`tools/setup/graphics_windows.sh`). В проверке
программа идёт под Wine на виртуальном экране Xvfb: окно Windows,
поверхность `VK_KHR_win32_surface`, Vulkan Wine передаёт lavapipe хоста.
Снимок и вывод — те же, что на Linux.

**Android** — `examples/triangle/android/build.sh` собирает
`build/android-triangle/triangle.apk`. Программа dpl — библиотека
(`dpl build --lib`), из неё `libmain.so` с `SDL_main`, который зовёт
`SDLActivity` из SDL3; пакет подписан ключом отладки. Инструменты —
`tools/setup/cross_android.sh` и `tools/setup/graphics_android.sh`.
Запуск — на устройстве с Vulkan (Р64): `adb install -r triangle.apk`,
затем значок «dpl triangle». Сверка первого кадра показывается
сообщением: `frame 64x64: 1152 triangle pixels, fnv1a 1933328421,
matches the reference: true`.
