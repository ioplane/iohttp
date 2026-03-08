# Требования к встраиваемому HTTP-серверу на C23

## 1. Назначение

Данный документ описывает требования к встраиваемому HTTP-серверу, реализованному на **C23**, предназначенному для:

- встроенных Linux-систем;
- устройств под RTOS;
- сервисных компонентов приложений;
- web UI и REST API устройств;
- работы за reverse proxy / TLS-терминатором или в режиме собственного TLS-терминирования.

Документ задает **рекомендуемый production-профиль**: компактный, предсказуемый по ресурсам, безопасный, пригодный для раздачи SPA и интеграции в embedded-окружение.

---

## 2. Нормативная база и внешние спецификации

### HTTP / URI / аутентификация

- **RFC 9110 — HTTP Semantics**  
  <https://www.rfc-editor.org/rfc/rfc9110.html>
- **RFC 9112 — HTTP/1.1**  
  <https://www.rfc-editor.org/rfc/rfc9112>
- **RFC 3986 — URI Generic Syntax**  
  <https://www.rfc-editor.org/rfc/rfc3986>
- **RFC 7617 — Basic HTTP Authentication Scheme**  
  <https://www.rfc-editor.org/rfc/rfc7617.html>
- **RFC 6750 — Bearer Token Usage**  
  <https://www.rfc-editor.org/rfc/rfc6750>

### TLS

- **RFC 8446 — TLS 1.3**  
  <https://www.rfc-editor.org/rfc/rfc8446>
- **wolfSSL Documentation**  
  <https://www.wolfssl.com/docs/>
- **wolfSSL Manual**  
  <https://www.wolfssl.com/documentation/manuals/wolfssl/index.html>

### Proxy Protocol

- **HAProxy PROXY protocol v1/v2 specification**  
  <https://www.haproxy.org/download/3.4/doc/proxy-protocol.txt>

### Дополнительно

- Для кэширования, conditional requests, range requests и статус-кодов использовать терминологию и требования RFC 9110 / RFC 9112.
- Для URI-path normalizaton и разбора query / path / percent-encoding опираться на RFC 3986.

---

## 3. Цели реализации

Сервер должен обеспечивать:

1. корректную обработку **HTTP/1.1**;
2. встроенную поддержку **TLS через wolfSSL**;
3. встроенную поддержку **PROXY protocol v1/v2**;
4. безопасную раздачу **статических ресурсов** и **SPA-приложений** (React, Vue, Svelte);
5. удобную интеграцию с логикой устройства через callback/API слой;
6. предсказуемое потребление памяти и CPU;
7. устойчивость к malformed / oversized / slow-client запросам.

---

## 4. Архитектурные требования

### 4.1 Общая модель

Сервер должен быть реализован как набор независимых модулей:

- транспортный слой;
- TLS-слой;
- HTTP parser;
- router;
- response builder;
- static file / embedded asset provider;
- auth layer;
- logging / metrics layer;
- proxy protocol decoder.

### 4.2 Требования к API

Публичный API должен:

- позволять инициализацию через явную конфигурационную структуру;
- избегать глобального состояния или сводить его к минимуму;
- позволять передавать `user_data` в callbacks;
- возвращать явные коды ошибок;
- поддерживать compile-time feature flags для отключения ненужных возможностей;
- быть пригодным для использования как в single-thread, так и в event-driven профиле.

### 4.3 Портируемость

Необходимо отделить:

- POSIX / socket-адаптер;
- абстракцию времени и таймеров;
- файловый backend;
- TLS backend (wolfSSL);
- сетевые особенности RTOS / lwIP / embedded Linux.

---

## 5. Обязательная поддержка протокола HTTP

### 5.1 Версия протокола

Минимально обязательна поддержка:

- **HTTP/1.1**.

HTTP/2 и HTTP/3 не входят в обязательный scope.

### 5.2 Методы

Обязательно поддержать:

- `GET`
- `HEAD`
- `POST`
- `PUT`
- `DELETE`
- `OPTIONS`

Опционально:

- `PATCH`

### 5.3 Обязательные возможности parser-а

Parser должен корректно разбирать:

- request line;
- method;
- target URI;
- path;
- query string;
- HTTP version;
- заголовки;
- тело запроса;
- `Content-Length`;
- `Connection`;
- `Host`;
- `Content-Type`;
- `Transfer-Encoding: chunked` — как минимум для приема или передачи, предпочтительно для обоих направлений.

### 5.4 Обязательные возможности ответа

Сервер должен уметь формировать:

- статус-код;
- заголовки;
- тело ответа;
- `Content-Length`;
- `Content-Type`;
- `Connection: keep-alive / close`;
- `Date`;
- при необходимости `Location`, `Cache-Control`, `ETag`, `Last-Modified`, `Accept-Ranges`.

### 5.5 Статусы ошибок

Должны быть предусмотрены корректные ответы минимум для:

- `200 OK`
- `201 Created`
- `204 No Content`
- `301/302/307/308` при редиректах
- `304 Not Modified`
- `400 Bad Request`
- `401 Unauthorized`
- `403 Forbidden`
- `404 Not Found`
- `405 Method Not Allowed`
- `408 Request Timeout`
- `409 Conflict`
- `411 Length Required` — если политика сервера требует длину
- `413 Content Too Large`
- `414 URI Too Long`
- `415 Unsupported Media Type`
- `422 Unprocessable Content`
- `429 Too Many Requests` — при наличии rate limit
- `500 Internal Server Error`
- `501 Not Implemented`
- `503 Service Unavailable`
- `505 HTTP Version Not Supported`

---

## 6. Сетевой слой и модель соединений

### 6.1 Базовые требования

Сервер должен поддерживать:

- bind на заданный адрес и порт;
- ограничение числа одновременных соединений;
- backlog конфигурации;
- keep-alive;
- таймаут на accept;
- таймаут на чтение заголовков;
- таймаут на чтение тела;
- idle timeout;
- таймаут на запись ответа.

### 6.2 Модель конкурентности

Предпочтительный профиль:

- event loop / non-blocking I/O.

Допустимые профили:

- single-thread polling;
- ограниченный worker pool.

Нежелательный профиль для embedded:

- поток на каждое соединение без жестких лимитов.

### 6.3 Ограничения ресурсов

Необходимо обеспечить:

- фиксируемые лимиты буферов на соединение;
- ограничение общего числа активных запросов;
- возможность работы с минимальным использованием heap;
- отсутствие неконтролируемого роста памяти при длинных заголовках или больших телах.

---

## 7. Встроенная поддержка wolfSSL

### 7.1 Обязательность

Сервер **должен иметь встроенную поддержку wolfSSL** как основной и штатный TLS backend.

Это означает:

- TLS не должен рассматриваться как внешний адаптер “когда-нибудь потом”;
- конфигурация TLS должна быть частью публичного API сервера;
- сервер должен уметь работать как минимум в двух режимах:
  - plain HTTP;
  - HTTPS поверх wolfSSL.

### 7.2 Функциональные требования к интеграции wolfSSL

Обязательно:

- создание и управление TLS context через wolfSSL;
- загрузка сертификата и закрытого ключа;
- загрузка цепочки сертификатов при необходимости;
- отдельная конфигурация для server mode;
- TLS handshake на входящем соединении;
- корректное завершение TLS session;
- обработка ошибок handshake и I/O через wolfSSL API;
- таймауты на handshake;
- возможность ограничить версии TLS и криптопараметры;
- журналирование причин TLS-ошибок.

### 7.3 Поддерживаемые версии TLS

Рекомендуемый профиль:

- **TLS 1.3** — основной;
- **TLS 1.2** — опционально для совместимости, если это требуется окружением.

Нежелательно включать устаревшие протоколы TLS 1.0/1.1.

### 7.4 Сертификаты и ключи

Сервер должен поддерживать:

- загрузку сертификата из файла, памяти или callback-источника;
- загрузку ключа из файла, памяти или callback-источника;
- проверку корректности пары сертификат/ключ при старте;
- перезагрузку сертификатов без полной пересборки приложения, если это допустимо архитектурой.

### 7.5 Настройки безопасности TLS

Желательно обеспечить:

- конфигурируемый список cipher suites;
- отключение небезопасных алгоритмов;
- конфигурацию кривых / групп key exchange, если это поддерживается профилем wolfSSL;
- session resumption только как осознанно включаемую опцию;
- SNI — опционально, если нужен multi-tenant профиль.

### 7.6 Совместимость с embedded-профилем

Интеграция должна учитывать:

- статическую или урезанную сборку wolfSSL;
- возможность compile-time отключения лишних алгоритмов;
- ограничение RAM во время handshake;
- ограничение размера внутренних буферов;
- отсутствие ненужных malloc/free на горячем пути.

### 7.7 TLS + reverse proxy

При работе за reverse proxy сервер должен поддерживать два профиля:

1. TLS завершается на самом сервере через wolfSSL;
2. TLS завершается на внешнем proxy, а сервер принимает plain HTTP и доверяет metadata только через PROXY protocol / доверенный контур.

---

## 8. Встроенная поддержка PROXY protocol

### 8.1 Обязательность

Сервер **должен поддерживать PROXY protocol версии 1 и 2** на входящем TCP-соединении.

### 8.2 Режим работы

Поддержка PROXY protocol должна быть **явно конфигурируемой на listener**.

Недопустимо:

- автоматически “угадывать”, есть ли PROXY header;
- смешивать public-подключения и trusted-proxy-подключения на одном и том же listener без явной конфигурации.

### 8.3 Поддерживаемые возможности

Обязательно:

- прием **PROXY v1**;
- прием **PROXY v2**;
- извлечение исходного source/destination address и port;
- поддержка IPv4;
- поддержка IPv6;
- корректная обработка `LOCAL` / `PROXY` semantics для v2;
- корректный отказ на malformed header;
- short timeout на ожидание PROXY header.

### 8.4 Порядок обработки

Для listener с включенным PROXY protocol сервер должен:

1. принять TCP-соединение;
2. до начала HTTP/TLS-обработки прочитать и валидировать PROXY header;
3. извлечь peer / local endpoint metadata;
4. только после этого передать соединение дальше:
   - либо в wolfSSL handshake,
   - либо в HTTP parser.

### 8.5 Безопасность

Обязательно:

- доверять PROXY protocol **только** на явно доверенных listener-ах;
- поддерживать ACL / allowlist доверенных proxy-адресов;
- запрещать spoofing клиентского адреса с недоверенных источников;
- логировать отказы из-за некорректного PROXY header;
- ограничивать максимальный размер читаемого заголовка и общее время ожидания.

### 8.6 TLV в PROXY v2

Рекомендуется:

- уметь пропускать неизвестные TLV безопасным образом;
- иметь API для доступа к известным TLV, если они нужны приложению;
- как минимум не ломаться при наличии TLV, даже если они не используются.

### 8.7 Экспорт метаданных

Сервер должен предоставлять приложению:

- реальный peer address;
- реальный peer port;
- real/local destination address;
- флаг, пришли ли данные из PROXY protocol;
- версию PROXY protocol;
- при наличии — доступ к разобранным TLV.

---

## 9. Роутинг и обработчики

### 9.1 Базовые возможности

Router должен поддерживать:

- маршрутизацию по методу и path;
- статические маршруты;
- параметры пути (`/api/v1/users/{id}`);
- wildcard / prefix routes для статических файлов;
- fallback handler;
- error handler.

### 9.2 API обработчиков

Handler должен иметь доступ к:

- методу;
- path;
- query parameters;
- path parameters;
- заголовкам;
- телу запроса;
- client metadata;
- TLS metadata;
- PROXY metadata;
- user context.

### 9.3 Ответы обработчиков

Handler должен иметь возможность:

- отправить весь ответ целиком;
- отправить потоковый ответ;
- установить произвольные заголовки;
- вернуть status code;
- инициировать redirect;
- вернуть JSON;
- отдать файл;
- передать управление следующему middleware / hook при наличии такой модели.

---

## 10. Раздача статических ресурсов

### 10.1 Обязательные возможности

Сервер должен уметь:

- отдавать файлы из файловой системы;
- отдавать встроенные ресурсы из памяти/ROM;
- определять MIME type;
- отдавать `Content-Length`;
- отдавать `Last-Modified`;
- отдавать `ETag` — желательно;
- поддерживать `HEAD` для статических файлов;
- поддерживать кэш-заголовки;
- ограничивать доступ только внутри document root.

### 10.2 Защита файлового backend-а

Обязательно:

- canonicalization / normalization path;
- защита от `..` traversal;
- запрет выхода за пределы document root;
- защита от NUL-byte / malformed path;
- корректная работа с percent-decoding.

### 10.3 Производительность

Желательно:

- zero-copy / sendfile-подобный путь там, где доступно;
- streaming больших файлов;
- ограничение размера буферов при отдаче больших объектов.

---

## 11. Раздача SPA (React, Vue, Svelte)

### 11.1 Обязательная цель

Сервер должен иметь штатный режим раздачи **SPA-приложений**, собранных как статический frontend, включая проекты на:

- React;
- Vue;
- Svelte.

### 11.2 Функциональные требования к SPA-режиму

Обязательно:

- раздача статических build-артефактов (`index.html`, `assets/*`, `*.js`, `*.css`, `*.svg`, `*.woff2`, и т.д.);
- корректные MIME-типы для frontend-ресурсов;
- возможность указать document root для SPA;
- возможность настроить **fallback на `index.html`** для client-side routing;
- исключение API-префиксов из SPA fallback (например, `/api/*` не должен редиректиться на `index.html`);
- поддержка `HEAD` и `GET` для SPA-ресурсов;
- поддержка long-lived caching для хешированных asset-файлов;
- более консервативное кэширование для `index.html`.

### 11.3 Рекомендуемая политика кэширования SPA

Рекомендуется:

- для `index.html`: `Cache-Control: no-cache` или близкая стратегия;
- для fingerprinted assets (`app.abc123.js`, `style.abc123.css`): `Cache-Control: public, max-age=31536000, immutable`;
- поддержка `ETag` / `If-None-Match` и/или `Last-Modified` / `If-Modified-Since`.

### 11.4 Безопасность и совместимость

Желательно:

- отдача корректного `Content-Type` для JS/CSS/SVG/JSON/fonts/WASM;
- поддержка precompressed assets (`.gz`, `.br`) как опция для embedded Linux профиля;
- защита от directory listing, если это явно не включено;
- возможность конфигурировать security headers.

### 11.5 Пример поведения SPA fallback

Если запрошен путь:

- `/app/settings/profile`

и соответствующий файл не найден, но путь не относится к API и не выглядит как запрос статического артефакта, сервер должен вернуть:

- `index.html`

с кодом:

- `200 OK`

Это необходимо для работы client-side routing во frontend-фреймворках.

---

## 12. JSON и API-профиль

### 12.1 Минимальный API-профиль

Сервер должен удобно поддерживать:

- JSON responses;
- JSON request body reading;
- единый формат ошибок API;
- versioned routes (`/api/v1/...`).

### 12.2 Content negotiation

Минимально:

- корректная установка `Content-Type`;
- чтение `Accept` — опционально, если приложению это нужно.

---

## 13. Аутентификация и авторизация

### 13.1 Обязательный механизм

Должен существовать **auth hook / middleware**, позволяющий приложению реализовать собственную политику доступа.

### 13.2 Рекомендуемые встроенные схемы

Желательно поддержать:

- Basic Auth;
- Bearer Token;
- cookie/session hook;
- пользовательский callback-механизм.

### 13.3 Требования безопасности

Обязательно:

- не логировать секреты и токены целиком;
- обеспечивать работу auth-схем только поверх TLS, если это внешний контур;
- корректно возвращать `401` / `403`;
- для Basic Auth явно требовать HTTPS-профиль в production.

---

## 14. CORS и браузерная совместимость

Если сервер обслуживает browser-based UI или frontend отдельно от API, желательно поддержать:

- `Access-Control-Allow-Origin`;
- `Access-Control-Allow-Methods`;
- `Access-Control-Allow-Headers`;
- `Access-Control-Allow-Credentials`;
- `OPTIONS` preflight обработку.

CORS должен быть конфигурируемым и по умолчанию консервативным.

---

## 15. Streaming, большие тела и загрузки

### 15.1 Обязательно

Сервер должен поддерживать хотя бы один безопасный путь для работы с большими данными:

- потоковое чтение request body;
- потоковую отдачу response body.

### 15.2 Желательно

- chunked response;
- chunked request parsing;
- multipart/form-data для upload;
- ограничение скорости / размера загрузки;
- удобный OTA/upload-профиль для firmware и конфигурации.

---

## 16. Кэширование и условные запросы

Рекомендуется поддержка:

- `ETag` / `If-None-Match`;
- `Last-Modified` / `If-Modified-Since`;
- `Cache-Control`;
- `304 Not Modified`;
- `Range` / `Accept-Ranges` / `206 Partial Content` для файлов и крупных объектов.

Для SPA и web UI это существенно снижает сетевую нагрузку и улучшает UX.

---

## 17. Наблюдаемость: логи, метрики, отладка

### 17.1 Логи

Обязательно:

- access log;
- error log;
- уровни логирования;
- возможность отключения или минимизации логов для low-resource режима.

### 17.2 Что логировать

Желательно:

- время запроса;
- метод;
- path;
- статус;
- размер ответа;
- client IP;
- original client IP из PROXY protocol;
- TLS version / handshake failure reason;
- код внутренней ошибки.

### 17.3 Метрики

Рекомендуется:

- число активных соединений;
- число open keep-alive sessions;
- распределение статус-кодов;
- ошибки parser-а;
- TLS handshake failures;
- PROXY header failures;
- время обработки запроса.

---

## 18. Безопасность

### 18.1 Обязательные ограничения

Сервер должен иметь конфигурируемые лимиты на:

- длину request line;
- длину URI;
- число заголовков;
- размер одного заголовка;
- суммарный размер заголовков;
- размер request body;
- число одновременных соединений;
- число keep-alive запросов на одно соединение;
- время ожидания PROXY header;
- время TLS handshake;
- время чтения заголовков и тела.

### 18.2 Обязательные проверки

Необходимо корректно обрабатывать и отклонять:

- malformed request line;
- malformed headers;
- invalid chunked framing;
- invalid `Content-Length`;
- конфликт между `Content-Length` и `Transfer-Encoding`;
- недопустимую HTTP version;
- path traversal;
- неполный PROXY header;
- некорректный TLS handshake;
- header smuggling / request smuggling рискованные конструкции.

### 18.3 Доверенные источники

Для PROXY protocol и административных маршрутов желательно:

- allowlist trusted subnets;
- отдельные listener-ы для внутреннего и внешнего трафика.

### 18.4 Security headers

Для web UI желательно поддержать настройку:

- `X-Content-Type-Options: nosniff`;
- `Referrer-Policy`;
- `Content-Security-Policy`;
- `Strict-Transport-Security` — только в HTTPS-профиле;
- `X-Frame-Options` или эквивалентную политику.

---

## 19. Конфигурация

Минимальная конфигурация сервера должна включать:

- bind address;
- port;
- enable/disable TLS;
- wolfSSL certificate/key settings;
- enable/disable PROXY protocol per listener;
- trusted proxy list;
- timeouts;
- connection limits;
- request limits;
- static root;
- SPA mode и SPA fallback;
- API prefix exclusions для SPA mode;
- logging level;
- auth hooks / middleware;
- CORS policy.

---

## 20. Рекомендуемые compile-time feature flags

Желательно предусмотреть отдельные флаги сборки:

- `HTTPD_ENABLE_TLS`
- `HTTPD_ENABLE_WOLFSSL`
- `HTTPD_ENABLE_PROXY_PROTOCOL`
- `HTTPD_ENABLE_STATIC_FILES`
- `HTTPD_ENABLE_SPA`
- `HTTPD_ENABLE_ETAG`
- `HTTPD_ENABLE_RANGE`
- `HTTPD_ENABLE_MULTIPART`
- `HTTPD_ENABLE_JSON_HELPERS`
- `HTTPD_ENABLE_CORS`
- `HTTPD_ENABLE_ACCESS_LOG`

Это позволит собирать урезанные профили под конкретные устройства.

---

## 21. Требования к качеству реализации на C23

### 21.1 Кодовая база

Ожидается:

- аккуратное использование возможностей C23 без избыточной экзотики;
- явная модель владения памятью;
- отсутствие UB на некорректном вводе;
- sanitizer-friendly сборка для desktop CI;
- четкое разделение hot path и control path.

### 21.2 Тестирование

Обязательно:

- unit tests для parser-а;
- unit tests для router-а;
- tests для static file serving;
- tests для SPA fallback;
- tests для TLS через wolfSSL;
- tests для PROXY v1/v2;
- tests для malformed PROXY / malformed HTTP / malformed TLS cases;
- stress tests на many-connections сценарии.

### 21.3 Fuzzing

Желательно применить fuzzing для:

- HTTP parser;
- PROXY protocol parser;
- URI normalization;
- header parsing;
- chunked decoding.

---

## 22. Приемочные критерии

Реализация считается соответствующей документу, если выполняются все условия:

1. Сервер принимает и корректно обслуживает HTTP/1.1 запросы.
2. Сервер умеет работать в HTTPS-режиме через wolfSSL.
3. Сервер умеет принимать PROXY protocol v1 и v2 на выделенном listener-е.
4. Сервер корректно передает приложению реальный client IP и port из PROXY metadata.
5. Сервер умеет отдавать SPA build с fallback на `index.html`.
6. Сервер защищен от path traversal и oversized/malformed requests.
7. Сервер имеет настраиваемые лимиты, таймауты и логирование.
8. Сервер выдерживает тесты на malformed input без crash / UB / uncontrolled memory growth.

---

## 23. Рекомендуемый production scope

### Обязательно

- HTTP/1.1
- `GET/HEAD/POST/PUT/DELETE/OPTIONS`
- router
- static files
- SPA mode
- JSON helpers
- wolfSSL integration
- PROXY protocol v1/v2
- limits and timeouts
- access/error logs
- path normalization
- streaming больших ответов

### Желательно

- `ETag` / `Last-Modified`
- `Range`
- multipart upload
- CORS
- security headers
- precompressed asset serving
- metrics hooks

### Необязательно на первом этапе

- HTTP/2
- HTTP/3
- websocket
- templating engine
- reverse proxy функциональность
- CGI/FastCGI
- динамическая модульность

---

## 24. Краткая формулировка требований

Встраиваемый HTTP-сервер на C23 должен быть компактным и предсказуемым по ресурсам серверным компонентом, который:

- корректно реализует HTTP/1.1;
- имеет встроенную поддержку TLS на базе wolfSSL;
- поддерживает PROXY protocol v1/v2 на доверенных listener-ах;
- безопасно отдает статические ресурсы и SPA-приложения;
- устойчив к некорректному и враждебному сетевому вводу;
- предоставляет удобный API для интеграции с embedded-логикой устройства или сервиса.
