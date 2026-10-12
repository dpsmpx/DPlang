# Привязки

Модули dpl для библиотек C, сгенерированные из закреплённого входа
(Р58): Vulkan из `vk.xml`, дальше SDL3 и JNI. Результат лежит в
репозитории и проходит ревью; сборка программы от генератора и сети не
зависит. Средства языка — `extern fn`, `extern type`, `@c`, заимствующие
поля, `@count`, `[N]T`, `@union`, общее начало (Р59, Р61, Р62;
`docs/spec/core.md`, разделы ffi).

## 1. Раскладка

```
bindings/gen/vulkan.py        генератор: vk.xml → vulkan/vulkan.dpm; --check — сверка
bindings/gen/check.py         все генераторы в режиме --check (шаг run.sh «привязки»)
bindings/vulkan/vk.xml        вход, закреплён: 1.3.275 (docs/THIRD_PARTY.md)
bindings/vulkan/extensions.txt расширения сверх ядра 1.0–1.3 (Р62)
bindings/vulkan/vulkan.dpm    модуль `vulkan` — не править
```

- **Импорт** — `import vulkan`. Загрузчик ищет модуль рядом с главным,
  затем в стандартной библиотеке, затем в каталоге привязок:
  `<привязки>/vulkan/vulkan.dpm` (`module.import`). Каталог —
  `$DPL_BINDINGS`, по умолчанию `bindings/` рядом со сборкой
  (`docs/compiler/cli.md`).
- **Перегенерация** — `python3 bindings/gen/vulkan.py`. Новое расширение
  — строка в `extensions.txt` и перегенерация; новая версия Vulkan —
  новый `vk.xml`, строка в `docs/THIRD_PARTY.md` и перегенерация.
- **Проверки.** `bindings/gen/check.py` (шаг `tools/check/run.sh`):
  повторная генерация даёт те же байты. Проход `bindings` в
  `tests/run.py` (`tests/bindings.py`):
  - `vulkan/layout` — размер, выравнивание, смещение и размер каждого
    поля всех структур сверяются с `vulkan.h` (пакет `libvulkan-dev`);
  - `vulkan/run` — программа `tests/bindings/vulkan/` на программном
    драйвере lavapipe (`mesa-vulkan-drivers`): экземпляр, устройство с
    цепочкой `pNext`, буфер, заливка командой в очереди, чтение через
    `vkMapMemory`.

  Без пакетов случай пропускается, и итог прогона это называет.

## 2. Vulkan: перевод типов

Объём — ядро 1.0–1.3 и расширения из `extensions.txt` (Р62), только
`api="vulkan"`. Имена типов, функций и констант — как в C; поля и
параметры — `snake_case` (`pNext` → `p_next`, `maxImageDimension2D` →
`max_image_dimension2d`), совпадение с ключевым словом — с `_` в конце.
Цели dpl 64-битные: `size_t` — `u64`, неотправляемые дескрипторы —
указатели.

| в C | в dpl |
|---|---|
| `uint32_t`, `float`, `size_t`, `char` … | `u32`, `f32`, `u64`, `u8` … |
| `VkBool32`, `VkDeviceSize`, `VkFlags`, `VkFlags64` | `u32`, `u64`, `u32`, `u64` |
| перечисление `VkFormat` | `i32` и константы `pub const VK_FORMAT_…: i32`; у поля — комментарий с именем типа |
| маска `VkXFlags`, биты `VkXFlagBits` | `u32` (`u64` у 64-битных) и константы битов |
| дескриптор `VkInstance` | `pub extern type VkInstance`; в полях и параметрах `optional` и в выводе C — `?VkInstance` |
| `PFN_vk…` в поле | `extern fn(...)`, необязательный — `?extern fn(...)` |
| структура, объединение | `@c pub struct`, `@union pub struct` |
| `sType` с `values` | `s_type: i32 = VK_STRUCTURE_TYPE_…` — значение по умолчанию |
| `const void* pNext` | `?&VkBaseInStructure`: цепочка ввода, `p_next: &features` (общее начало) |
| `pNext` структуры `returnedonly` | `?VkVoid` — цепочки вывода нет, только `none` |
| `const char*` (null-terminated) | `str`; параметр `optional` — `?str` |
| `const char* const*` с длиной | `[]str` |
| `const T*` с длиной | `[]T`; поле-счётчик — `@count(n)`, если указатель не `optional` и без `noautovalidity` |
| `const T*` без длины | `&T`, `optional` — `?&T` |
| `const void*` с длиной | `[]u8` |
| `T[N]` | `[N]T` (`char[N]` — `[N]u8`) |
| `void*` (`pUserData`) и вывод C внутри входной структуры | `?VkVoid` — указатель C без операций |
| параметр `T*` (вывод) | `&mut T`; дескриптор — `&mut ?H`; с длиной — `&mut List[T]` (`List[?H]`) |
| параметр `void**` (`ppData`) | `&mut ?VkVoid` |
| `VkResult` | `i32` |

Исключение: `VkShaderModuleCreateInfo.pCode` — `@count(code_size)
p_code: []u8`, SPIR-V байтами из файла (указатель в C тот же).

Счётчик без `@count` (указатель `optional`, `noautovalidity` или длина
выражением) пишут сами: так `viewport_count` динамического состояния
задаётся без `p_viewports`.

## 3. Vulkan: чего нет

Пропущенное перечислено в конце `vulkan.dpm`:

- `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr` — результат —
  указатель на функцию без сигнатуры;
- функции, которым нужен `&mut` заимствующей структуры
  (`vkGetPhysicalDeviceFeatures2`): заимствующая структура бывает только
  аргументом (Р62); вместо них — функции ядра 1.0
  (`vkGetPhysicalDeviceFeatures`).

Цепочки вывода `pNext` (свойства и возможности расширений через `…2`)
не выражаются: `&mut` в поле в языке нет. Компоновка на Windows
(`vulkan-1`) и Android (динамический `libvulkan.so`) — шаг 7 v0.2.

## 4. Пример

```
import vulkan

fn main() {
    var inst: ?vulkan.VkInstance = none
    let rc = vulkan.vkCreateInstance(&vulkan.VkInstanceCreateInfo {
        p_application_info: &vulkan.VkApplicationInfo {
            p_application_name: "demo",
            api_version: vulkan.VK_API_VERSION_1_3,
        },
        pp_enabled_layer_names: [],
        pp_enabled_extension_names: [],
    }, none, &mut inst)
    if let i = inst {
        vulkan.vkDestroyInstance(i, none)
    }
}
```

Пустые срез и список уходят в C как NULL: «сначала число, потом
массив» — `vkEnumeratePhysicalDevices(inst, &mut n, &mut devs)` с
пустым `devs`, затем `devs = repeat(none, int(n))` и второй вызов.
