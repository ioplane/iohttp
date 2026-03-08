# Техническое задание
## Библиотека OpenAPI 3.2 для C23 (`liboas`) как дополнение к `iohttp`

---

## 1. Назначение

Данный документ определяет требования к библиотеке **`liboas`** — библиотеке на **C23** для:

- загрузки, парсинга и компиляции **OpenAPI Description**;
- публикации спецификации API в машинно-читабельном виде;
- валидации HTTP-запросов и HTTP-ответов относительно OpenAPI-описания;
- проверки требований безопасности OpenAPI на основе уже вычисленного runtime-контекста запроса;
- интеграции с HTTP-сервером **`iohttp`**.

`liboas` проектируется как **надстройка над `iohttp`**, а не как отдельный HTTP/TLS/transport стек.
Библиотека не должна владеть сокетами, циклом ввода-вывода, TLS-handshake, QUIC-сессиями, PROXY protocol или жизненным циклом соединения.

Ключевой принцип: **`iohttp` отвечает за транспорт, TLS, HTTP framing и маршрутизацию; `liboas` отвечает за OpenAPI-модель, валидацию и публикацию спецификации.**

---

## 2. Контекст проекта

Архитектура проекта состоит из двух самостоятельных, но связанных подсистем:

1. **`iohttp`** — собственный HTTP-сервер на C23, ориентированный на Linux и построенный вокруг **`io_uring`**.
2. **`liboas`** — библиотека OpenAPI, работающая поверх абстракций `iohttp`.

Из этого следуют обязательные архитектурные решения:

- `liboas` **не должна иметь прямую обязательную зависимость от wolfSSL**;
- `liboas` **не должна иметь прямую обязательную зависимость от io_uring**;
- `liboas` должна интегрироваться через **адаптер к `iohttp`**, а не через TLS backend;
- сведения о TLS/mTLS, полученные в `iohttp` через wolfSSL, должны передаваться в `liboas` в виде **абстрактного security context**;
- сведения о реальном IP клиента, полученные через **PROXY protocol**, должны передаваться в `liboas` как уже нормализованная transport-метаинформация запроса.

Таким образом, для целевой архитектуры проекта **основным интеграционным модулем является `liboas-adapter-iohttp`, а не `liboas-adapter-wolfssl`**.

---

## 3. Цели реализации

`liboas` должна обеспечивать:

1. поддержку **OpenAPI Specification 3.2.0** как основной целевой версии;
2. поддержку **JSON Schema Draft 2020-12** в объеме, достаточном для production-валидации API;
3. быстрый перевод OpenAPI-документа в runtime-представление для hot path;
4. request validation:
   - path parameters,
   - query parameters,
   - headers,
   - cookies,
   - request body;
5. response validation:
   - status code,
   - headers,
   - body,
   - content negotiation;
6. проверку **Security Requirement Object** на базе внешнего security context;
7. публикацию `/openapi.json` и опционально `/openapi.yaml`;
8. удобную интеграцию с UI и SPA:
   - Swagger UI,
   - ReDoc,
   - собственные React/Vue/Svelte-приложения;
9. предсказуемое потребление памяти и отсутствие неконтролируемых аллокаций на hot path;
10. безопасную обработку malformed и hostile спецификаций.

---

## 4. Не-цели первой production-ветки

В первую стабильную версию **не должны входить**:

- генерация клиентского кода;
- генерация серверных заглушек;
- ORM/БД-интеграции;
- собственный HTTP-сервер;
- собственный TLS backend;
- собственная реализация PROXY protocol;
- прямое управление wolfSSL-сессиями;
- прямое управление io_uring;
- HTML-рендеринг документации внутри ядра;
- полноценный линтер уровня Spectral;
- runtime, требующий отдельного path matcher поверх уже найденного маршрута `iohttp`.

Допускаются отдельные tooling-утилиты и CLI, но не в ядре `liboas`.

---

## 5. Нормативная база и внешние спецификации

### 5.1 OpenAPI / JSON Schema

- OpenAPI Specification 3.2.0  
  <https://spec.openapis.org/oas/v3.2.0.html>
- JSON Schema Draft 2020-12  
  <https://json-schema.org/draft/2020-12>
- JSON Schema Core (2020-12)  
  <https://json-schema.org/draft/2020-12/json-schema-core.html>
- JSON Schema Validation (2020-12)  
  <https://json-schema.org/draft/2020-12/draft-bhutton-json-schema-validation-01.html>
- JSON Schema Meta-Schema  
  <https://json-schema.org/draft/2020-12/schema>

### 5.2 HTTP / URI / сериализация

- RFC 9110 — HTTP Semantics  
  <https://www.rfc-editor.org/rfc/rfc9110.html>
- RFC 9112 — HTTP/1.1  
  <https://www.rfc-editor.org/rfc/rfc9112>
- RFC 9113 — HTTP/2  
  <https://www.rfc-editor.org/rfc/rfc9113>
- RFC 9114 — HTTP/3  
  <https://www.rfc-editor.org/rfc/rfc9114>
- RFC 3986 — URI: Generic Syntax  
  <https://www.rfc-editor.org/rfc/rfc3986>
- RFC 6570 — URI Template  
  <https://www.rfc-editor.org/rfc/rfc6570>
- RFC 6901 — JSON Pointer  
  <https://www.rfc-editor.org/rfc/rfc6901>
- RFC 6902 — JSON Patch  
  <https://www.rfc-editor.org/rfc/rfc6902>
- RFC 6265 — HTTP State Management Mechanism  
  <https://www.rfc-editor.org/rfc/rfc6265.html>
- RFC 7578 — multipart/form-data  
  <https://www.rfc-editor.org/rfc/rfc7578>

### 5.3 Безопасность и auth

- RFC 7617 — Basic Authentication  
  <https://www.rfc-editor.org/rfc/rfc7617>
- RFC 6750 — OAuth 2.0 Bearer Token Usage  
  <https://www.rfc-editor.org/rfc/rfc6750>
- RFC 8628 — OAuth 2.0 Device Authorization Grant  
  <https://www.rfc-editor.org/rfc/rfc8628>
- RFC 8446 — TLS 1.3  
  <https://www.rfc-editor.org/rfc/rfc8446>
- wolfSSL Documentation  
  <https://www.wolfssl.com/docs/>
- wolfSSL Manual / API Reference  
  <https://www.wolfssl.com/documentation/manuals/wolfssl/>

### 5.4 Интеграционная среда

- HAProxy PROXY protocol specification  
  <https://www.haproxy.org/download/3.4/doc/proxy-protocol.txt>
- Linux `io_uring(7)`  
  <https://man7.org/linux/man-pages/man7/io_uring.7.html>

---

## 6. Архитектурные принципы

### 6.1 Разделение ответственности

`liboas` должна быть разделена на следующие логические зоны:

1. **Input layer** — чтение JSON/YAML и построение сырого DOM.
2. **Semantic model** — нормализованная модель OpenAPI-документа.
3. **Compiler** — компиляция документа в runtime-представление.
4. **Runtime validator** — исполнение валидаторов на запросах и ответах.
5. **Adapter layer** — интеграция с `iohttp` и внешними источниками документов.

### 6.2 Иммутабельность compiled model

После успешной компиляции:

- semantic model и compiled model считаются **immutable**;
- compiled model должна быть безопасной для **concurrent read-only access**;
- обновление спецификации должно происходить через полную перекомпиляцию нового экземпляра.

### 6.3 Минимальная связанность

Ядро `liboas` не должно знать о:

- типах `WOLFSSL*`;
- SQE/CQE и деталях `io_uring`;
- конкретной HTTP/1.1, HTTP/2, HTTP/3 framing-реализации;
- внутреннем устройстве роутера `iohttp`.

Ядро должно видеть только:

- нормализованный runtime-запрос;
- нормализованный runtime-ответ;
- абстрактный security context;
- metadata операции, найденной на стороне `iohttp`.

### 6.4 Zero-surprise интеграция

Интеграция с `iohttp` должна строиться так, чтобы:

- не было второго route lookup в hot path;
- не было второго path parser при уже найденной операции;
- не было прямых аллокаций пропорционально размеру body без лимитов;
- не было блокирующих операций файловой системы или сети в request path.

### 6.5 Safe by default

По умолчанию библиотека должна применять лимиты на:

- размер документа;
- глубину дерева JSON/YAML;
- число узлов;
- глубину и число `$ref`;
- число внешних документов;
- размер диагностического отчета;
- размер декодированного request body;
- глубину вложенности схем;
- размер string/array/object instance при валидации.

---

## 7. Целевая модель модулей

### 7.1 Обязательные модули ядра

- `liboas-core` — базовые типы, ошибки, allocators, limits;
- `liboas-parse` — JSON/YAML parsing;
- `liboas-model` — каноническая объектная модель;
- `liboas-resolve` — `$ref`, anchors, URI resolution;
- `liboas-validate` — семантическая валидация OpenAPI-документа;
- `liboas-schema` — компиляция и выполнение JSON Schema validators;
- `liboas-runtime` — request/response/security validation;
- `liboas-emit` — сериализация в JSON/YAML;
- `liboas-diag` — диагностика и error reports.

### 7.2 Обязательные адаптеры

- `liboas-adapter-iohttp` — основной адаптер к типам `iohttp`;
- `liboas-adapter-filesystem` — загрузка документов с ФС;
- `liboas-adapter-memory` — загрузка документов из памяти / embedded resources.

### 7.3 Опциональные модули

- `liboas-yaml` — YAML backend;
- `liboas-regex-*` — backend регулярных выражений;
- `liboas-ui` — вспомогательные функции публикации `/openapi.json`, ETag, UI-config;
- `liboas-tooling` — CLI для lint/compile/validate/smoke.

### 7.4 Чего не должно быть в основном дереве модулей

В основном обязательном профиле **не должно быть** модуля `liboas-adapter-wolfssl`.

Если когда-либо потребуется интеграция `liboas` вне `iohttp`, допускается вынесение отдельного вспомогательного модуля вида:

- `extras/liboas-wolfssl-bridge`

Такой мост может конвертировать сведения из `WOLFSSL*` в абстрактный `oas_security_ctx_t`, но он не должен быть обязательной частью архитектуры.

---

## 8. Профиль совместимости OpenAPI

### 8.1 Целевая версия

Целевая версия: **OpenAPI 3.2.0**.

Библиотека обязана:

- корректно принимать `openapi: 3.2.0`;
- принимать 3.1.x в режиме backward-compatible parsing;
- явно диагностировать неподдержанные конструкции;
- не подменять ошибки спецификации молчаливым поведением.

### 8.2 Обязательные особенности OpenAPI 3.2

В архитектуре должны быть учтены как минимум:

- `$self` у корневого документа;
- `jsonSchemaDialect`;
- `additionalOperations`;
- метод `query`;
- sequential / streaming media types через `itemSchema`;
- `deviceAuthorization` в OAuth flows;
- расширенная модель тегов и специфика расширений `x-*`.

### 8.3 JSON Schema профиль

Основной профиль: **Draft 2020-12**.

#### Обязательная поддержка в первой production-ветке

- примитивные типы;
- `enum`, `const`;
- числовые ограничения;
- string constraints;
- array constraints;
- object constraints;
- `allOf`, `anyOf`, `oneOf`, `not`;
- `if` / `then` / `else`;
- `dependentRequired`, `dependentSchemas`;
- `$ref`, `$id`, `$anchor`;
- `prefixItems`, `items`;
- `unevaluatedProperties`, `unevaluatedItems`;
- `format` как annotation по умолчанию;
- опциональный strict mode, где часть `format` включается как assertion.

#### Допускаемая поэтапная реализация

- `$dynamicRef`, `$dynamicAnchor`;
- полный vocabulary-aware режим;
- полный output format по JSON Schema spec;
- расширенные форматы и пользовательские vocabularies.

---

## 9. Архитектура интеграции с `iohttp`

### 9.1 Базовая схема

`iohttp` выполняет:

- прием соединения;
- PROXY protocol decode;
- TLS handshake через wolfSSL;
- ALPN / HTTP negotiation;
- request parsing;
- router lookup;
- middleware chain;
- handler dispatch.

`liboas` выполняет:

- lookup/получение уже найденной operation metadata;
- parameter extraction and validation;
- request body validation;
- response validation;
- security requirement evaluation;
- публикацию спецификации.

### 9.2 Критический принцип: один route lookup

При интеграции с `iohttp` не должно быть второго path matcher в hot path.

Рекомендуемая схема:

1. На этапе старта `liboas` компилирует документ в `oas_compiled_api_t`.
2. Для каждого route в `iohttp` хранится pointer/reference на `oas_operation_t`.
3. В runtime `iohttp` сначала находит route своим роутером.
4. Затем middleware `liboas` получает **уже найденную** operation и запускает валидацию.

Это позволяет избежать:

- дублирования маршрутизации;
- рассинхронизации path semantics между `iohttp` и `liboas`;
- лишних сравнений строк в hot path.

### 9.3 Обязательный модуль `liboas-adapter-iohttp`

Адаптер должен обеспечивать:

- маппинг `io_request_t` → `oas_runtime_request_t`;
- маппинг `io_response_t` → `oas_runtime_response_t`;
- маппинг `iohttp` security/TLS/auth context → `oas_security_ctx_t`;
- middleware hooks для request validation;
- middleware hooks для response validation;
- mount/publish helpers для `/openapi.json`, `/openapi.yaml` и UI endpoints;
- регистрацию связи `io_route_t` ↔ `oas_operation_t`.

---

## 10. Security context и отказ от `liboas-adapter-wolfssl`

### 10.1 Почему `liboas-adapter-wolfssl` не нужен

`mutualTLS` в OpenAPI описывает **требование к безопасности операции**, а не способ выполнения TLS-handshake.

Следовательно:

- `liboas` не должна настраивать wolfSSL;
- `liboas` не должна извлекать сертификаты напрямую из `WOLFSSL*` в основной интеграции;
- `liboas` должна оценивать лишь **нормализованный факт прохождения mTLS и свойства peer-а**, переданные со стороны `iohttp`.

### 10.2 Нормализованный security context

`iohttp` должен формировать security context наподобие:

```c
typedef struct oas_tls_peer_info {
    bool tls_present;
    bool mtls_present;
    bool cert_verified;
    int  verify_error;

    const char *subject_dn;
    const char *subject_cn;
    const char *issuer_dn;
    const char *serial_hex;
    const char *sha256_fingerprint;

    const char *san_dns_first;
    const char *san_uri_first;
} oas_tls_peer_info_t;

typedef struct oas_auth_info {
    const char *scheme;      // "basic", "bearer", ...
    const char *token;
    const char *username;
} oas_auth_info_t;

typedef struct oas_security_ctx {
    const oas_tls_peer_info_t *tls;
    const oas_auth_info_t     *auth;

    const char *api_key_header_name;
    const char *api_key_header_value;
    const char *api_key_query_name;
    const char *api_key_query_value;
    const char *api_key_cookie_name;
    const char *api_key_cookie_value;
} oas_security_ctx_t;
```

Дополнительно `iohttp` должен иметь возможность передавать:

- реальный клиентский IP после PROXY protocol;
- peer port;
- сведения о listener policy;
- флаг trusted proxy.

### 10.3 Обязанности `liboas` при security evaluation

`liboas` должна уметь:

- проверить, удовлетворен ли `mutualTLS`;
- проверить `http` security schemes (`basic`, `bearer`);
- проверить `apiKey` в header/query/cookie;
- оценить логическое правило OpenAPI:
  - все схемы внутри одного объекта Security Requirement обязательны одновременно;
  - из списка объектов достаточно одной альтернативы.

### 10.4 Что остается в `iohttp`

В `iohttp` должны оставаться:

- wolfSSL context configuration;
- certificate loading;
- verify policy;
- OCSP/CRL policy;
- session handling;
- peer certificate extraction;
- mTLS handshake success/failure logic.

---

## 11. Runtime-модель `liboas`

### 11.1 Уровни представления

#### 11.1.1 Raw DOM

Сырой JSON/YAML DOM для парсинга и первичной диагностики.

#### 11.1.2 Semantic Model

Нормализованная модель:

- document meta;
- servers;
- paths;
- operations;
- parameters;
- request bodies;
- responses;
- security schemes;
- components;
- resolved references.

#### 11.1.3 Compiled Runtime Plan

Предкомпилированная структура для быстрого исполнения:

- операция;
- таблица параметров;
- декодеры сериализации;
- compiled JSON Schema validators;
- media type dispatch;
- security evaluation plan;
- response rules by status code.

### 11.2 Обязательные runtime-структуры

Минимально должны существовать:

- `oas_document_t`;
- `oas_compiled_api_t`;
- `oas_operation_t`;
- `oas_runtime_request_t`;
- `oas_runtime_response_t`;
- `oas_validation_result_t`;
- `oas_security_ctx_t`.

---

## 12. API взаимодействия с `iohttp`

### 12.1 Загрузка и компиляция

```c
int oas_document_load_json(
    oas_allocator_t *alloc,
    const void *data,
    size_t len,
    oas_document_t **out_doc,
    oas_diag_t *diag);

int oas_compile_document(
    const oas_document_t *doc,
    const oas_compile_options_t *opts,
    oas_compiled_api_t **out_api,
    oas_diag_t *diag);
```

### 12.2 Привязка к `iohttp`

```c
int oas_iohttp_mount_api(
    io_server_t *srv,
    const oas_compiled_api_t *api,
    const oas_iohttp_mount_options_t *opts);

int oas_iohttp_bind_route(
    io_route_t *route,
    const oas_operation_t *operation);
```

### 12.3 Runtime validation

```c
int oas_validate_request(
    const oas_operation_t *op,
    const oas_runtime_request_t *req,
    const oas_security_ctx_t *sec,
    oas_validation_result_t *out);

int oas_validate_response(
    const oas_operation_t *op,
    const oas_runtime_response_t *resp,
    oas_validation_result_t *out);
```

### 12.4 Публикация спецификации

```c
int oas_iohttp_publish_openapi_json(
    io_server_t *srv,
    const char *path,
    const oas_compiled_api_t *api,
    const oas_publish_options_t *opts);
```

---

## 13. Request validation

### 13.1 Path parameters

Обязательная поддержка:

- извлечение path params, уже найденных роутером `iohttp`;
- сверка обязательности и имен;
- декодирование percent-encoding;
- приведение к строковому представлению для последующей schema validation;
- поддержка style/explode в объеме, применимом к path.

### 13.2 Query parameters

Обязательная поддержка:

- `form`, `spaceDelimited`, `pipeDelimited`, `deepObject` в пределах OpenAPI-профиля;
- explode/non-explode;
- multiple values;
- schema validation после десериализации.

### 13.3 Header parameters

Обязательная поддержка:

- case-insensitive lookup;
- запрет попыток валидировать заголовки, запрещенные/специальные по HTTP policy;
- корректная обработка повторяющихся заголовков.

### 13.4 Cookie parameters

Обязательная поддержка:

- парсинг `Cookie` header;
- извлечение отдельных cookie по имени;
- validation по schema.

### 13.5 Request body

Обязательная поддержка в P1/P2:

- `application/json`;
- `application/*+json`;
- `text/plain`;
- `application/x-www-form-urlencoded`;
- `multipart/form-data` в базовом профиле.

Опционально:

- бинарные media types;
- streaming/sequential media types;
- `text/event-stream`;
- `multipart/mixed` streaming.

---

## 14. Response validation

### 14.1 Что проверяется

- допустимость status code для operation;
- выбор exact status / range / `default`;
- обязательные response headers;
- media type;
- schema тела ответа.

### 14.2 Режимы работы

Должны поддерживаться режимы:

- `OFF` — отключено;
- `LOG_ONLY` — ошибки только логируются;
- `ENFORCE` — некорректный ответ приводит к ошибке сервера/замене ответа;
- `SAMPLED` — выборочная проверка по sampling policy.

### 14.3 Ограничения hot path

Для больших response body должна быть возможность:

- отключить полную response validation;
- валидировать только headers/status;
- валидировать только первые N байт/объектов при потоковых ответах;
- использовать compile-time и runtime limits.

---

## 15. Streaming и OpenAPI 3.2 sequential media types

Так как `iohttp` будет работать поверх `io_uring`, а значит естественно поддерживать неблокирующие потоки данных, в `liboas` нужно заложить архитектурную поддержку OpenAPI 3.2 streaming semantics.

### 15.1 Обязательный минимум первой production-ветки

В первой production-ветке допускается:

- полная поддержка `schema` для complete content;
- архитектурная готовность к `itemSchema`;
- API расширения для будущей потоковой валидации;
- явная диагностика, если streaming validation не поддержана для конкретного media type.

### 15.2 Целевой профиль дальнейшей версии

В дальнейшей версии библиотека должна уметь:

- валидировать элементы потока по `itemSchema` независимо друг от друга;
- поддерживать `multipart/mixed` streaming;
- поддерживать `text/event-stream` как последовательный тип;
- работать без обязательной буферизации всего тела.

### 15.3 Ограничение ответственности

`liboas` не должна читать сокет напрямую. Потоковое чтение должен контролировать `iohttp`, а `liboas` должна получать элементы/чанки через адаптерный API.

---

## 16. Публикация OpenAPI и UI

### 16.1 Обязательные endpoints

`liboas-adapter-iohttp` должна уметь публиковать:

- `/openapi.json`;
- опционально `/openapi.yaml`;
- опционально статический UI mountpoint.

### 16.2 Интеграция со SPA

Так как `iohttp` должен поддерживать SPA-раздачу, `liboas` должна быть совместима с конфигурацией, где:

- Swagger UI / ReDoc раздаются как статический SPA;
- фронтенд на React/Vue/Svelte запрашивает `/openapi.json`;
- ETag/Cache-Control согласованы с версией compiled API;
- fallback на `index.html` не должен ломать `/openapi.json` и `/openapi.yaml`.

### 16.3 ETag / cache

Рекомендуется:

- strong или stable weak ETag на serialized spec;
- `Last-Modified` по времени компиляции/релиза;
- корректный `Content-Type`.

---

## 17. Память, производительность и конкурентность

### 17.1 Принципы

- никаких неограниченных аллокаций на hot path;
- per-request arena допускается только через внешний allocator/pool;
- compiled validators должны быть максимально precomputed;
- lookup по operation должен быть O(1) или близким к нему после route bind.

### 17.2 Модель конкурентности

Так как `iohttp` будет использовать `io_uring`, в `liboas` нужно исходить из того, что один и тот же compiled API может использоваться:

- в одном event loop;
- в нескольких reactor threads;
- в нескольких worker threads в режиме read-only.

Следовательно:

- compiled API должен быть thread-safe для чтения;
- mutable diagnostic буферы не должны разделяться между потоками без синхронизации;
- runtime result objects должны принадлежать вызывающей стороне.

### 17.3 Подход к памяти

Должны поддерживаться:

- allocator injection;
- arena allocator;
- system malloc backend;
- compile-time лимиты;
- runtime лимиты.

---

## 18. Диагностика и ошибки

### 18.1 Категории ошибок

Нужно разделить ошибки на:

- parse errors;
- document semantic errors;
- `$ref` resolution errors;
- schema compilation errors;
- request validation errors;
- response validation errors;
- security evaluation failures;
- internal errors.

### 18.2 Требования к диагностике

Каждая ошибка должна, по возможности, содержать:

- machine-readable code;
- severity;
- human-readable message;
- pointer/path до места в документе;
- pointer/path до места во входном instance;
- возможность агрегировать несколько ошибок.

### 18.3 RFC 7807 совместимость

Рекомендуется предусмотреть возможность экспорта validation error в формат, совместимый с **Problem Details for HTTP APIs**:

- RFC 7807  
  <https://www.rfc-editor.org/rfc/rfc7807>
- RFC 9457  
  <https://www.rfc-editor.org/rfc/rfc9457>

---

## 19. Требования к build и поставке

### 19.1 Язык и совместимость

- основной язык: **C23**;
- предупреждения компилятора должны быть чистыми в строгих режимах;
- сборка должна поддерживаться минимум через CMake;
- желательна сборка как static library и shared library.

### 19.2 Зависимости

Обязательные внешние зависимости для core-профиля должны быть минимальны.

Допускается следующая структура зависимостей:

- JSON parser backend;
- YAML parser backend — опционально;
- regex backend — опционально.

Нельзя делать wolfSSL или io_uring обязательной зависимостью `liboas-core`.

---

## 20. Тестирование

### 20.1 Обязательные типы тестов

- unit tests для parser/model/compiler/runtime;
- golden tests на разбор OpenAPI-документов;
- compatibility tests на известных примерах OpenAPI;
- request/response validation tests;
- negative tests на malformed документы;
- fuzzing для parser и `$ref` resolver;
- stress tests на конкурентное read-only использование compiled model.

### 20.2 Интеграционные тесты с `iohttp`

Обязательны интеграционные тесты на:

- publish `/openapi.json`;
- request validation middleware;
- response validation middleware;
- `mutualTLS` через security context, переданный из `iohttp`;
- работу за trusted reverse proxy с PROXY protocol;
- SPA/UI совместимость.

### 20.3 Наборы сценариев

Нужно покрыть:

- JSON body;
- form body;
- multipart body;
- path/query/header/cookie parameters;
- security alternatives;
- `default` responses;
- `oneOf` / `anyOf` / `allOf`;
- циклы `$ref`;
- large schemas;
- hostile inputs.

---

## 21. Этапы реализации

### P0 — Foundation

- core types;
- JSON parser backend;
- minimal document model;
- diagnostics;
- compile skeleton.

### P1 — Practical OpenAPI Runtime

- paths/operations/components;
- `$ref` resolver;
- request validation для path/query/header/cookie;
- JSON body validation;
- `liboas-adapter-iohttp` minimal;
- publish `/openapi.json`.

### P2 — Production readiness

- response validation;
- security requirement evaluation;
- Basic/Bearer/apiKey/mutualTLS;
- multipart/form-data;
- richer JSON Schema profile;
- better diagnostics.

### P3 — Streaming and advanced semantics

- `itemSchema` architecture activation;
- streaming validators;
- sampled response validation;
- external document policies;
- offline compile tooling.

### P4 — Ecosystem

- YAML round-trip improvements;
- optional UI helpers;
- CLI / lint / smoke utilities;
- optional extras, включая мосты к иным runtime.

---

## 22. Рекомендуемая структура каталогов

```text
liboas/
  include/liboas/
    oas.h
    oas_core.h
    oas_diag.h
    oas_document.h
    oas_compile.h
    oas_runtime.h
    oas_security.h
    oas_emit.h

  src/core/
    oas_status.c
    oas_alloc.c
    oas_limits.c

  src/parse/
    oas_json_parse.c
    oas_yaml_parse.c

  src/model/
    oas_document.c
    oas_components.c
    oas_operation.c

  src/resolve/
    oas_ref_resolver.c
    oas_uri.c

  src/schema/
    oas_schema_compile.c
    oas_schema_validate.c

  src/runtime/
    oas_request_validate.c
    oas_response_validate.c
    oas_security_eval.c

  src/emit/
    oas_emit_json.c
    oas_emit_yaml.c

  src/diag/
    oas_diag.c

  adapters/iohttp/
    include/liboas/adapters/iohttp.h
    src/oas_iohttp_request_map.c
    src/oas_iohttp_response_map.c
    src/oas_iohttp_security_map.c
    src/oas_iohttp_mount.c

  adapters/filesystem/
  adapters/memory/
  tooling/
  tests/
```

---

## 23. Итоговые архитектурные решения

Для текущего проекта должны быть зафиксированы следующие решения:

1. `liboas` интегрируется с сервером через **`liboas-adapter-iohttp`**.
2. Прямой обязательный модуль **`liboas-adapter-wolfssl` не создается**.
3. `iohttp` остается владельцем transport, TLS, PROXY protocol и routing.
4. `liboas` использует только нормализованные runtime-абстракции.
5. В hot path допускается только **один route lookup** — на стороне `iohttp`.
6. OpenAPI security evaluation работает поверх абстрактного security context.
7. Архитектура должна быть заранее готова к streaming semantics OpenAPI 3.2, но без искусственного усложнения P1.

---

## 24. Критерии приемки

Реализация `liboas` считается соответствующей данному ТЗ, если:

1. загружает и компилирует корректные OpenAPI 3.2 документы;
2. выявляет документные и schema-ошибки с детальной диагностикой;
3. публикует `/openapi.json` через `iohttp`;
4. валидирует запросы относительно operation metadata, полученной через `iohttp`;
5. корректно проверяет security requirements без прямой зависимости от wolfSSL;
6. поддерживает compiled API как thread-safe read-only структуру;
7. не дублирует маршрутизацию и не вводит второй path matcher в hot path;
8. проходит unit, integration, fuzz и negative tests;
9. не требует прямой зависимости от `io_uring` и `wolfSSL` в ядре `liboas`.

