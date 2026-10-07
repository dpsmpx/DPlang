# Онлайн-компилятор: контракт для dpsmpx.com

Контракт API v1 для страницы в dpsmpx/SocialMediaImage (Р49).
Устройство сервиса — [`README.md`](README.md). Здесь — только то, что
нужно клиенту: запросы, ответы, ошибки, CORS, пределы.

**Пока сервис выключен** (`available: false`, Р49). Страница сначала
спрашивает `GET /api/v1/info` и не показывается, если `available` не
`true`. Адрес API задаёт инфраструктура при развёртывании; ниже — `API`.

## 1. Версии

- **Версия API** — в пути: `/api/v1/`. Внутри v1 ответ может получить
  новые необязательные поля — клиент обязан пропускать незнакомые поля
  ответа. Несовместимое изменение — новый путь `/api/v2/`.
- **Версия компилятора** и **версия языка** — в каждом ответе:
  `compiler.version`, `compiler.language`. Запрос называет версию языка
  в поле `language`; другая версия — ошибка `unsupported_language`.
- **Детерминизм:** при одинаковом запросе и тех же версиях всё, кроме
  `meta`, совпадает байт в байт. Ответ можно кэшировать на клиенте по
  паре (запрос, `compiler`).

## 2. `GET API/api/v1/info`

```json
{
  "api": "v1",
  "available": false,
  "compiler": {"version": "0.1.0-dev", "language": "0.1"},
  "limits": {
    "request_bytes": 409600, "source_bytes": 65536, "lines": 2000,
    "identifier_bytes": 128, "nesting": 256, "diagnostics": 100,
    "output_bytes": 262144
  }
}
```

`limits` — пределы профиля: редактор может предупреждать заранее.
Числа — из `service/config/online-v1.json`, они могут меняться без
смены версии API.

## 3. `POST API/api/v1/compile`

Заголовки: `Content-Type: application/json` (кодировка — только UTF-8),
`Content-Length`. Без cookies и авторизации: `fetch(…, {credentials:
'omit'})`.

Тело — объект ровно из четырёх строковых полей; лишнее поле — ошибка:

| поле | значение |
|---|---|
| `language` | версия языка, сейчас `"0.1"` |
| `mode` | `"check"` — проверка без сборки и исполнения |
| `file_name` | имя модуля с `.dpm`: буква или `_`, затем буквы, цифры, `_`; например `"main.dpm"` |
| `source` | текст модуля в UTF-8 |

JSON — строгий (RFC 8259): без комментариев и лишних запятых, без
повторных ключей, суррогаты `\u` — только парами. `JSON.stringify`
браузера даёт ровно такой.

Модуль один: `import` другого модуля даёт обычную диагностику E0302
«cannot find module».

### Ответ: проверка прошла

Статус 200 — и когда в программе ошибки: это результат, а не сбой.

```json
{
  "api": "v1",
  "compiler": {"version": "0.1.0-dev", "language": "0.1"},
  "result": {
    "success": false,
    "diagnostics": [
      {"severity": "error", "code": "E0401", "message": "expected 'i32', found 'str'",
       "file": "main.dpm", "line": 2, "col": 18, "end_line": 2, "end_col": 21, "notes": []}
    ],
    "truncated": false,
    "total": 1
  },
  "meta": {"duration_ms": 4}
}
```

| поле | значение |
|---|---|
| `success` | `true` — ошибок нет (предупреждения могут быть) |
| `diagnostics` | сообщения по порядку: файл, позиция, код |
| `severity` | `error`, `warning`, `note` |
| `code` | код диагностики, например `E0401` |
| `message` | текст по-английски (Р47) |
| `file`, `line`, `col`, `end_line`, `end_col` | место: строки и столбцы с 1; столбец — номер символа Unicode в строке (не байт и не единица UTF-16); конец не включается. У сообщения без места этих полей нет |
| `notes` | пояснения: `message` и, если есть, то же место |
| `truncated`, `total` | `true` — показаны не все: сообщений больше `diagnostics` или ответ упёрся в `output_bytes`; `total` — сколько всего |
| `meta.duration_ms` | время проверки; не входит в детерминированную часть |

Редактор на JavaScript считает столбцы в UTF-16: символ вне BMP (эмодзи)
занимает две единицы — переводить при подсветке.

### Ответ: ошибка

```json
{
  "api": "v1",
  "compiler": {"version": "0.1.0-dev", "language": "0.1"},
  "error": {"code": "limit_exceeded", "message": "number of source lines exceeds the limit of 2000",
            "limit": {"name": "lines", "max": 2000}},
  "meta": {"duration_ms": 1}
}
```

`limit` — только у ошибок предела; `meta` — только если запрос дошёл до
компилятора. `message` — по-английски, для показа как есть.

| `code` | статус | когда | что делать клиенту |
|---|---|---|---|
| `invalid_request` | 400 (408, 411, 431 — ошибки HTTP) | неверный JSON или поля, неверный HTTP | ошибка клиента; не повторять |
| `unsupported_language` | 400 | `language` не та | обновить страницу |
| `unsupported_media_type` | 415 | не `application/json` в UTF-8 | ошибка клиента |
| `payload_too_large` | 413 | тело больше `request_bytes` | показать предел |
| `limit_exceeded` | 422 | исходник, строки, длина имени, память компиляции | показать `limit` |
| `compile_timeout` | 422 | проверка дольше предела CPU или времени | показать; программа слишком тяжёлая |
| `rate_limited` | 429 | много запросов с адреса или всего | повторить через `Retry-After` секунд |
| `queue_full`, `queue_timeout` | 503 | сервис занят | повторить через `Retry-After` |
| `service_unavailable` | 503 | сервис выключен или перегружен соединениями | скрыть страницу; `GET /api/v1/info` |
| `compiler_internal_error` | 500 | сбой компилятора или сервиса | показать «внутренняя ошибка»; не повторять тот же запрос |
| `not_found`, `method_not_allowed` | 404, 405 | неверный путь или метод | ошибка клиента |

Предел вложенности (`nesting`) — не ошибка запроса: программа получает
диагностику E0002 в `result`, как от `dplang check`.

## 4. CORS и заголовки

- `Access-Control-Allow-Origin` — только для `https://dpsmpx.com`;
  preflight `OPTIONS` разрешает `POST` и заголовок `Content-Type`. Другой
  сайт ответ не прочитает.
- Учётные данные не используются: ни cookies, ни `Authorization`.
- CSP страницы: `connect-src` должен включать адрес `API`.
- Ответы — `Cache-Control: no-store`; соединение закрывается после
  ответа.

## 5. Пределы и честное поведение клиента

- Не слать запрос на каждое нажатие клавиши: проверка — по кнопке или с
  задержкой после ввода (не чаще раза в секунду).
- На 429 и 503 — ждать `Retry-After`; не повторять сразу.
- Предупреждать до отправки по `limits` из `info`: размер, строки.

## 6. Приватность (для политики сайта)

Исходник используется только для проверки: не сохраняется, не пишется в
журналы и метрики. В журнале сервиса нет адреса клиента — только хэш с
солью, которая меняется при каждом перезапуске.

## 7. Пример

```js
const res = await fetch(API + '/api/v1/compile', {
  method: 'POST',
  credentials: 'omit',
  headers: {'Content-Type': 'application/json'},
  body: JSON.stringify({language: '0.1', mode: 'check', file_name: 'main.dpm', source: editor.value}),
});
const reply = await res.json();
if (reply.result) show(reply.result.diagnostics, reply.result.truncated, reply.result.total);
else showError(reply.error.code, reply.error.message, res.headers.get('Retry-After'));
```
