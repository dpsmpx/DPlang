# Привязки

Модули dpl для библиотек C, сгенерированные из закреплённого входа
(Р58): Vulkan из `vk.xml`, SDL3 из заголовков, JNI из `jni.h` AOSP. Результат лежит в
репозитории и проходит ревью; сборка программы от генератора и сети не
зависит. Средства языка — `extern fn`, `extern type`, `@c`, заимствующие
поля, `@count`, `[N]T`, `@union`, общее начало, `@slot` (Р59, Р61–Р63;
`docs/spec/core.md`, разделы ffi).

## 1. Раскладка

```
bindings/gen/common.py         общее генераторов: имена, запись и --check
bindings/gen/check.py          все генераторы в режиме --check (шаг run.sh «привязки»)
bindings/gen/vulkan.py         vk.xml → vulkan/vulkan.dpm
bindings/gen/sdl3.py           sdl3/include/SDL3 → sdl3/sdl3.dpm
bindings/gen/jni.py            jni/jni.h → jni/jni.dpm
bindings/gen/cheader.py        разбор заголовков C: препроцессор, объявления, выражения
bindings/vulkan/vk.xml         вход, закреплён: 1.3.275 (docs/THIRD_PARTY.md)
bindings/vulkan/extensions.txt расширения сверх ядра 1.0–1.3 (Р62)
bindings/vulkan/vulkan.dpm     модуль `vulkan` — не править
bindings/sdl3/include/SDL3/    вход, закреплён: заголовки SDL 3.4.18 (35 файлов)
bindings/sdl3/LICENSE.txt      лицензия SDL (Zlib)
bindings/sdl3/headers.txt      заголовки, функции и константы которых входят в привязку
bindings/sdl3/sdl3.dpm         модуль `sdl3` — не править
bindings/jni/jni.h             вход, закреплён: AOSP libnativehelper, android-15.0.0_r1
bindings/jni/jni.dpm           модуль `jni` — не править
```

- **Импорт** — `import vulkan`. Загрузчик ищет модуль рядом с главным,
  затем в стандартной библиотеке, затем в каталоге привязок:
  `<привязки>/vulkan/vulkan.dpm` (`module.import`). Каталог —
  `$DPL_BINDINGS`, по умолчанию `bindings/` рядом со сборкой
  (`docs/compiler/cli.md`).
- **Перегенерация** — `python3 bindings/gen/vulkan.py`,
  `python3 bindings/gen/sdl3.py`. Новое расширение Vulkan — строка в
  `extensions.txt`, новый заголовок SDL3 — строка в `headers.txt` (и
  его включения в `include/SDL3`), затем перегенерация; новая версия —
  новый вход, строка в `docs/THIRD_PARTY.md` и перегенерация.
- **Проверки.** `bindings/gen/check.py` (шаг `tools/check/run.sh`):
  повторная генерация даёт те же байты. Проход `bindings` в
  `tests/run.py` (`tests/bindings.py`):
  - `vulkan/layout`, `sdl3/layout`, `jni/layout` — размер,
    выравнивание, смещение и размер каждого поля всех структур
    сверяются с заголовками C: `vulkan.h` (пакет `libvulkan-dev`) и
    закреплёнными заголовками SDL3 и JNI; у JNI ещё номер каждой записи
    `@slot` — место поля в таблице;
  - `vulkan/run` — программа `tests/bindings/vulkan/` на программном
    драйвере lavapipe (`mesa-vulkan-drivers`): экземпляр, устройство с
    цепочкой `pNext`, буфер, заливка командой в очереди, чтение через
    `vkMapMemory`;
  - `sdl3/run` — программа `tests/bindings/sdl3/`: видео offscreen,
    окно Vulkan, событие пользователя через очередь, поверхность Vulkan
    окна на lavapipe (`VK_EXT_headless_surface`);
  - `jni/run` — программа `tests/bindings/jni/` на JVM хоста (libjvm из
    JDK): статические методы Java, строка Java, исключение, `GetEnv` и
    `DestroyJavaVM` через таблицу `JavaVM`.

  Без библиотек случай пропускается, и итог прогона это называет.
  Ставит их `tools/setup/host_libs.sh`: пакеты Vulkan и SDL3 той же
  версии, что заголовки, из релизного архива с проверкой SHA-256.

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
| структура, объединение | `@c("vulkan/vulkan.h") pub struct`, `@union("vulkan/vulkan.h") pub struct` (заголовок C — Р76) |
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

## 4. SDL3: перевод типов

Функции и константы — из заголовков `headers.txt`; типы — из всех
закреплённых заголовков, сколько нужно (события тянут типы джойстика,
пера, сенсора). Препроцессор — для 64-битных little-endian целей.
Заголовки C не размечают смысл указателей, поэтому генератор читает
тип и описание параметра (`\param`) в документации заголовка; что
однозначно не выражается — пропускается с причиной в конце `sdl3.dpm`.

| в C | в dpl |
|---|---|
| `Uint8` … `Sint64`, `int`, `float`, `bool`, `size_t` | `u8` … `i64`, `i32`, `f32`, `bool`, `u64` |
| перечисление | `i32` и константы |
| `#define` с числом | константа: типа предшествующего `typedef` целого (`SDL_InitFlags` → `SDL_INIT_*: u32`), иначе по суффиксу (`u` — `u32`, `SDL_UINT64_C` — `u64`, без суффикса — `i32`) |
| `#define` со строкой | `pub const SDL_HINT_…: str` |
| скрытая структура (`SDL_Window`), `typedef struct X *H`, `typedef void *H` | `pub extern type`; результат — `?H` (NULL при ошибке) |
| структура, объединение | `@c("SDL3/SDL.h") pub struct`, `@union("SDL3/SDL.h") pub struct` (Р76); поля — `snake_case` |
| указатель в поле | на скрытую структуру — `?H`; иначе `?SDL_Void` — указатель без операций (строки и буферы событий в dpl не читаются) |
| параметр `const char*` | `str`; в описании есть NULL — `?str` |
| параметр `const T*` | `&T`, NULL в описании — `?&T`; «array» в описании — `[]T` |
| параметр `T*` (число, структура) | `&mut T` — значение для записи (`SDL_GetWindowSizeInPixels`, `SDL_PollEvent`) |
| параметр `void*` (`userdata`) | `?SDL_Void` |
| указатель на функцию (`SDL_TimerCallback`) | `extern fn(...)`, NULL в описании — `?extern fn(...)` |
| `VkInstance`, `VkSurfaceKHR*`, `const VkAllocationCallbacks*` | `vulkan.VkInstance`, `&mut ?vulkan.VkSurfaceKHR`, `?&vulkan.VkAllocationCallbacks` |
| результат `const char*` | `str`; NULL в описании — `?str` |

## 5. SDL3: чего нет

- Переменное число аргументов (`SDL_SetError`), `va_list`.
- Результаты-указатели на данные SDL: массивы (`SDL_GetDisplays`,
  `SDL_GetKeyboardState`), структуры (`SDL_GetCurrentDisplayMode`) и
  массив строк `SDL_Vulkan_GetInstanceExtensions` — расширения
  поверхности пишут сами: `VK_KHR_surface` и расширение платформы
  (`VK_EXT_headless_surface` у offscreen).
- Строка, которую освобождает вызывающий (`char*`), и буфер строки.
- Объекты с открытыми полями (`SDL_Surface`, `SDL_Palette`): их функции.
- Обратные вызовы со строками и указателями на числа
  (`SDL_AddHintCallback`).

Привязка — к динамической `libSDL3`: программа берёт её у системы.
Сборка для Windows и Android (`SDL3.dll`, `libSDL3.so` с `SDLActivity`)
— шаг 7 v0.2.

## 6. JNI

Вход — `jni.h` AOSP (Apache-2.0; `jni.h` OpenJDK под GPL не берётся,
`docs/policy/LICENSING.md`). Функции `JNIEnv` и `JavaVM` — записи таблиц
`JNINativeInterface` и `JNIInvokeInterface`: `@slot(N)` (Р63), `N` —
место поля в таблице, считая резервные. Имена параметров — из обёрток
C++ того же заголовка (`FindClass(const char* name)`, макросы
`Call…Method…`).

| в C | в dpl |
|---|---|
| `jboolean`, `jbyte`, `jchar`, `jshort`, `jint`, `jsize`, `jlong`, `jfloat`, `jdouble` | `u8`, `i8`, `u16`, `i16`, `i32`, `i32`, `i64`, `f32`, `f64` |
| `jobject` и его имена в C (`jclass`, `jstring`, `jthrowable`, массивы) | `?jobject` — в C это один тип `void*` |
| `jmethodID`, `jfieldID` | `?jmethodID`, `?jfieldID` |
| `JNIEnv*`, `JavaVM*` | `JNIEnv`, `JavaVM` — первый параметр функций `@slot` |
| `jvalue` | `@union pub struct jvalue`: `[jni.jvalue { l: s }]` — аргументы `…A` |
| `const char*` | `str` |
| `const jT*` | `[]T` |
| `jT* buf`, `char* buf` (`Get…Region`) | `&mut List[T]` — копия данных Java |
| `JNIEnv**`, `JavaVM**`, `void** env` (`GetEnv`) | `&mut ?JNIEnv`, `&mut ?JavaVM` |
| `JavaVMInitArgs`, `JavaVMOption`, `JavaVMAttachArgs` | `@c`-структуры; `options` — `@count(n_options) []JavaVMOption` |

Пропущено (перечень — в конце `jni.dpm`): варианты с переменным числом
аргументов и `va_list` (есть `…A`), функции, отдающие указатель на
данные JVM (`GetStringUTFChars`, `Get…ArrayElements`, критические
участки), и их `Release…`, `RegisterNatives` (адрес функции как `void*`
— методы `native` находит JVM по символу `@export("Java_…")`),
`JNI_GetDefaultJavaVMInitArgs`. `JNI_OnLoad` пишет сама библиотека:
`@export fn JNI_OnLoad(vm: jni.JavaVM, reserved: ?jni.JniVoid) i32`.

`JNI_CreateJavaVM` и `JNI_GetCreatedJavaVMs` — `@link("jvm")`: на
хосте — libjvm JDK; библиотека идёт в сборку, только если их зовут.

## 7. Пример

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
