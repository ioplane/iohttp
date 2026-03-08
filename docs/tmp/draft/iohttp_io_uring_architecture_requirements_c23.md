# Технические рекомендации и архитектурные требования
## HTTP-сервер `iohttp` на C23, ориентированный на `io_uring`

---

## 1. Назначение документа

Данный документ фиксирует обновленные архитектурные рекомендации к проекту **`iohttp`** с учетом того, что сервер проектируется **вокруг `io_uring` как базовой модели ввода-вывода**.

Задача документа — определить:

- границы ответственности `iohttp`;
- рекомендуемую архитектуру event loop / reactor;
- правила интеграции с wolfSSL, HTTP/1.1, HTTP/2, HTTP/3 и PROXY protocol;
- требования к маршрутизации, middleware и SPA-раздаче;
- требования к интеграции с `liboas`.

Документ не заменяет детальную внутреннюю архитектурную документацию проекта, а задает **целевой архитектурный профиль production-реализации**.

---

## 2. Базовая позиция

`iohttp` должен проектироваться не как «обычный сервер, у которого есть backend на io_uring», а как **server runtime, где `io_uring` является основной операционной моделью**.

Из этого следуют обязательные решения:

- отсутствие `epoll` fallback в основном production-профиле;
- проектирование state machines, буферов и таймаутов под семантику SQE/CQE;
- отказ от скрытых блокировок в hot path;
- отделение transport/TLS/protocol/runtime уровней;
- учет особенностей ownership и lifetime объектов при межпоточном использовании.

---

## 3. Цели `iohttp`

`iohttp` должен обеспечивать:

1. production-grade HTTP runtime на C23;
2. работу поверх `io_uring` на Linux;
3. встроенную поддержку wolfSSL;
4. встроенную поддержку PROXY protocol v1/v2;
5. поддержку HTTP/1.1 как обязательного baseline;
6. архитектурную поддержку HTTP/2 и HTTP/3;
7. удобный router + middleware runtime;
8. безопасную раздачу статического контента и SPA;
9. устойчивую интеграцию с `liboas`;
10. предсказуемое потребление памяти.

---

## 4. Не-цели

В рамках ядра `iohttp` не должны быть приоритетом:

- шаблонизаторы HTML;
- CGI/FastCGI;
- полноценный reverse proxy;
- динамический модульный ABI как у nginx;
- универсальный web framework;
- собственный OpenAPI-движок;
- полная HTTP caching proxy semantics.

---

## 5. Нормативная база и внешние документы

### 5.1 HTTP и TLS

- RFC 9110 — HTTP Semantics  
  <https://www.rfc-editor.org/rfc/rfc9110.html>
- RFC 9112 — HTTP/1.1  
  <https://www.rfc-editor.org/rfc/rfc9112>
- RFC 9113 — HTTP/2  
  <https://www.rfc-editor.org/rfc/rfc9113>
- RFC 9114 — HTTP/3  
  <https://www.rfc-editor.org/rfc/rfc9114>
- RFC 8446 — TLS 1.3  
  <https://www.rfc-editor.org/rfc/rfc8446>
- wolfSSL docs  
  <https://www.wolfssl.com/docs/>
- wolfSSL manual  
  <https://www.wolfssl.com/documentation/manuals/wolfssl/>

### 5.2 Linux `io_uring`

- `io_uring(7)`  
  <https://man7.org/linux/man-pages/man7/io_uring.7.html>
- `io_uring_setup(2)`  
  <https://man7.org/linux/man-pages/man2/io_uring_setup.2.html>
- `io_uring_prep_recv_multishot(3)`  
  <https://man7.org/linux/man-pages/man3/io_uring_prep_recv_multishot.3.html>
- `io_uring_prep_read_multishot(3)`  
  <https://man7.org/linux/man-pages/man3/io_uring_prep_read_multishot.3.html>
- `io_uring_prep_send_zc(3)`  
  <https://man7.org/linux/man-pages/man3/io_uring_prep_send_zc.3.html>
- `io_uring_register_buf_ring(3)`  
  <https://man7.org/linux/man-pages/man3/io_uring_register_buf_ring.3.html>

### 5.3 Дополнительные спецификации

- HAProxy PROXY protocol specification  
  <https://www.haproxy.org/download/3.4/doc/proxy-protocol.txt>
- RFC 6455 — WebSocket Protocol  
  <https://www.rfc-editor.org/rfc/rfc6455>
- WHATWG Server-Sent Events  
  <https://html.spec.whatwg.org/multipage/server-sent-events.html>

---

## 6. Архитектурная позиция относительно `io_uring`

### 6.1 `io_uring` — не транспортный backend, а core runtime

Архитектура должна строиться так, чтобы:

- connection lifecycle опирался на SQE/CQE;
- таймауты были частью ring-driven модели;
- buffering strategy была совместима с provided buffers и zero-copy путями;
- все critical-path операции были выражены через `io_uring` primitive или минимальную обвязку над ними.

### 6.2 Минимальная версия среды

Рекомендуемый production baseline:

- Linux kernel **6.7+**;
- современная версия `liburing`, поддерживающая используемые операции;
- wolfSSL с поддержкой TLS 1.3 и необходимых callback/API режимов.

### 6.3 Что нельзя проектировать «поверх» `io_uring`

Нельзя делать архитектуру, где:

- логика сервера остается epoll-centric, а `io_uring` только подменяет `recv/send`;
- владение соединением постоянно прыгает между потоками без явного ownership protocol;
- blocking file I/O или DNS внезапно оказываются в hot path;
- TLS и HTTP parser проектируются как синхронные циклы без явной обработки WANT_READ/WANT_WRITE.

---

## 7. Рекомендуемая top-level архитектура

### 7.1 Слои

Рекомендуется следующая декомпозиция:

1. **Core runtime / reactor**
2. **Connection and timer management**
3. **Transport adapters**
   - TCP
   - TLS (wolfSSL)
   - UDP/QUIC
4. **HTTP protocol layer**
   - HTTP/1.1
   - HTTP/2
   - HTTP/3
5. **Unified request/response model**
6. **Router + middleware**
7. **Static files / SPA / WS / SSE**
8. **Integration adapters**
   - `liboas`
   - metrics
   - logging

### 7.2 Рекомендуемая стратегия многопоточности

Для production стоит закладывать **multi-reactor architecture**:

- **один `io_uring` ring на worker thread**;
- каждый worker владеет своим набором соединений;
- listener strategy выбирается явно:
  - либо общий accept + handoff,
  - либо `SO_REUSEPORT` и listener-per-worker;
- соединение после назначения worker-у **не должно мигрировать между ring-ами в обычном режиме**.

### 7.3 Почему не только single-thread loop

Single-thread reactor хорош для P0/P1 и отладки, но для production с TLS, HTTP/2, статикой и будущим HTTP/3 лучше заранее закладывать:

- горизонтальное масштабирование по CPU;
- локальность буферов и allocator-ов;
- отсутствие кросс-thread contention на hot path.

Итоговая рекомендация:

- поддерживать **single-reactor** как development/profile mode;
- считать **multi-reactor ring-per-thread** основным production-профилем.

---

## 8. Event loop и ownership model

### 8.1 Правило владения

Каждое соединение должно иметь **ровно одного владельца** — конкретный reactor thread.

Этот владелец отвечает за:

- submit recv/send SQE;
- управление timeout SQE;
- lifecycle `io_conn_t`;
- взаимодействие с TLS object;
- protocol parser state;
- переходы state machine.

### 8.2 Передача между потоками

Передача соединения между потоками должна считаться исключительным случаем.

Если handoff нужен, он должен быть:

- явным;
- редким;
- безопасным с точки зрения lifetime буферов и TLS/session state.

### 8.3 CQE dispatch

Нужно проектировать таблицу соответствия `user_data -> object/state` так, чтобы:

- не было неоднозначности типов CQE;
- можно было быстро различать accept/recv/send/timeout/file/tls helper operations;
- обработка CQE не требовала heap allocations.

Рекомендуется использовать типизированный packing `user_data` или slab-идентификаторы.

---

## 9. Connection state machine

Рекомендуемая базовая state machine:

```text
ACCEPTING
  -> PROXY_HEADER
  -> TLS_HANDSHAKE
  -> PROTOCOL_NEGOTIATION
  -> HTTP_ACTIVE
      -> WS_ACTIVE
      -> SSE_ACTIVE
  -> DRAINING
  -> CLOSING
  -> CLOSED
```

Для HTTP/3/QUIC state machine может отличаться, но unified connection/session abstraction должна сохраняться.

### 9.1 Обязательные таймауты

Нужны отдельные таймауты на:

- accept backlog / initial read;
- PROXY header read;
- TLS handshake;
- request headers;
- request body;
- keep-alive idle;
- websocket ping/pong liveness;
- graceful drain/shutdown.

### 9.2 Таймауты как часть ring model

Таймауты должны использоваться через механизмы `io_uring`, а не через внешние blocking/sleep конструкции.

---

## 10. Использование возможностей `io_uring`

### 10.1 Обязательные или strongly recommended primitive

Рекомендуется опереться на следующие возможности:

- multishot accept;
- provided buffer rings;
- multishot recv там, где это уместно;
- linked timeouts;
- registered files;
- registered buffers;
- zero-copy send для крупных ответов;
- операции, упрощающие zero-copy/static file path.

### 10.2 Multishot accept

Должен использоваться как основной механизм приема новых TCP-соединений.

Требования:

- корректная обработка серии CQE;
- учет завершения multishot request;
- немедленный re-arm при прекращении `IORING_CQE_F_MORE`;
- отдельная стратегия backpressure при исчерпании connection pool.

### 10.3 Provided buffers

Рекомендуется использовать buffer rings для входящих socket reads.

Требования:

- фиксированные классы размеров буферов;
- четкая ownership-модель;
- возможность частичного потребления;
- отсутствие бесконтрольного роста памяти.

### 10.4 Zero-copy send

Нужно закладывать отдельную политику:

- мелкие ответы отправляются обычным send path;
- крупные ответы переводятся на `SEND_ZC` или file-splice/sendfile-подобный путь;
- порог zero-copy должен быть настраиваемым;
- код должен корректно обрабатывать notification CQE.

### 10.5 Registered files и buffers

Использование зарегистрированных файлов и буферов рекомендуется для:

- статических файлов;
- frequently-used descriptors;
- предсказуемого latency profile.

Но нельзя делать архитектуру, где регистрация становится обязательной для корректности.

---

## 11. Буферы и память

### 11.1 Цели

Модель памяти должна обеспечивать:

- bounded memory;
- отсутствие malloc/free в каждом CQE path;
- локальность данных по worker-ам;
- совместимость с TLS и HTTP parser lifetime.

### 11.2 Рекомендуемая структура памяти

- fixed connection pool на worker;
- fixed or slab-based request arena;
- buffer ring для ingress;
- отдельные output buffer/slab классы;
- pre-sized header parse buffers;
- отдельно управляемые file send contexts.

### 11.3 Что важно для TLS

С wolfSSL буферная модель должна учитывать два уровня данных:

- ciphertext buffers;
- plaintext/request buffers.

Нельзя смешивать ownership этих буферов неявно.

### 11.4 Ограничения

Должны быть лимиты на:

- headers size;
- header count;
- request body size;
- websocket frame size;
- SSE queue depth;
- static file open handles;
- число одновременных connections/streams.

---

## 12. wolfSSL как встроенный TLS backend

### 12.1 Обязательная архитектурная позиция

wolfSSL должен быть **первичным и встроенным TLS backend**.

`iohttp` не должен строиться через OpenSSL compatibility layer, если от этого можно отказаться.

### 12.2 Обязанности TLS слоя

TLS слой `iohttp` должен обеспечивать:

- TLS 1.3 как основной профиль;
- TLS 1.2 как опциональный compatibility режим;
- ALPN;
- SNI;
- mTLS;
- certificate reload;
- session resumption / tickets при необходимости;
- извлечение peer certificate metadata;
- подготовку security context для верхних слоев.

### 12.3 Non-blocking integration pattern

Интеграция должна быть проектирована как неблокирующая:

- `wolfSSL_accept/read/write` вызываются в controlled non-blocking cycle;
- `WANT_READ/WANT_WRITE` транслируются в соответствующие дальнейшие SQE;
- handshake и app-data path не должны скрывать повторные попытки внутри долгих blocking loops.

### 12.4 Нормализация TLS-метаданных

TLS слой должен уметь нормализовать для верхнего уровня:

- наличие TLS;
- negotiated version/cipher/ALPN;
- наличие client cert;
- verify status;
- subject/issuer/fingerprint/SAN;
- признак trusted/untrusted peer cert.

Это критично для интеграции с `liboas`.

---

## 13. PROXY protocol

### 13.1 Статус

Поддержка **PROXY protocol v1/v2** должна быть встроенной возможностью listener-а.

### 13.2 Порядок обработки

Рекомендуемый порядок:

1. accept connection;
2. если listener expects PROXY protocol — прочитать и разобрать PROXY header;
3. только после этого переходить к TLS handshake, если listener TLS-enabled;
4. далее перейти к HTTP protocol parsing.

### 13.3 Security policy

PROXY protocol должен приниматься только на доверенных listener-ах.

Нужны настройки:

- accept/require/disable mode;
- allowlist trusted upstream addresses;
- лимит длины header;
- strict parser mode;
- сохранение original peer address в connection metadata.

### 13.4 Верхнеуровневый API

Приложение, middleware и `liboas` должны иметь доступ к:

- socket peer address;
- proxied/original client address;
- флагу trusted proxy.

---

## 14. HTTP протоколы

### 14.1 HTTP/1.1

HTTP/1.1 обязателен и должен быть production-ready первым.

Нужно обеспечить:

- корректный parser;
- keep-alive;
- chunked transfer;
- header limits;
- pipelining policy либо явное ограничение/отказ от него;
- `Expect: 100-continue` policy;
- корректный `HEAD` и `OPTIONS`.

### 14.2 HTTP/2

HTTP/2 должен быть предусмотрен как следующий обязательный этап.

Архитектурно нужно заранее предусмотреть:

- stream multiplexing;
- flow control;
- HPACK/QPACK-related abstractions на своем уровне не дублировать;
- mapping stream -> unified request context.

### 14.3 HTTP/3

HTTP/3 должен быть предусмотрен как отдельный transport/runtime слой над QUIC.

Важно:

- не смешивать TCP/TLS lifecycle и QUIC lifecycle в одну низкоуровневую state machine;
- unified request abstraction должна появляться только над transport/protocol-слоем.

---

## 15. Unified request/response abstraction

Это один из ключевых architectural seams.

Верхние уровни должны работать с единым набором сущностей:

- `io_request_t`;
- `io_response_t`;
- `io_conn_info_t`;
- `io_tls_peer_info_t`;
- `io_route_match_t`.

Эти сущности должны быть независимы от:

- HTTP/1.1 parser internals;
- HTTP/2 stream internals;
- QUIC packet internals;
- wolfSSL internals.

Именно на этом уровне должны работать:

- router result;
- middleware;
- auth;
- rate limiting;
- logging;
- `liboas`.

---

## 16. Router и middleware

### 16.1 Router

Router должен обеспечивать:

- match по method + path;
- path params;
- wildcard/static prefixes;
- route groups;
- attach user metadata к маршруту;
- attach pointer на `oas_operation_t` к маршруту.

### 16.2 Middleware

Middleware chain должна быть:

- protocol-agnostic;
- non-blocking friendly;
- совместимой с streaming responses;
- пригодной для short-circuit ответов.

### 16.3 Критично для `liboas`

Router должен уметь сохранить binding между route и operation metadata, чтобы `liboas` не выполняла второй route lookup.

---

## 17. Static files и SPA

### 17.1 Базовый профиль

Должны поддерживаться:

- безопасная раздача файлов;
- MIME types;
- `Content-Length`;
- `ETag` / `Last-Modified`;
- `Range` по возможности;
- zero-copy путь для крупных файлов.

### 17.2 SPA support

Обязательна возможность раздачи SPA для:

- React;
- Vue;
- Svelte.

Это означает, что сервер должен поддерживать:

- mount статического каталога;
- fallback на `index.html` для неизвестных frontend-route;
- исключения для API prefix (`/api/`, `/openapi.json`, `/metrics`, и др.);
- корректные `Cache-Control` политики для hashed assets и HTML shell;
- precompressed `.gz` / `.br` при наличии.

### 17.3 Безопасность статической раздачи

Нужны:

- path normalization;
- защита от traversal;
- запрет выхода из root;
- контроль symbolic links политикой конфигурации.

---

## 18. WebSocket и SSE

### 18.1 WebSocket

Поддержка WebSocket должна быть отдельным режимом соединения после HTTP upgrade.

Нужно обеспечить:

- frame parsing;
- mask handling;
- fragmentation;
- ping/pong;
- close handshake;
- backpressure policy.

### 18.2 SSE

SSE должен поддерживаться как lightweight streaming response mode.

Нужны:

- heartbeat;
- reconnect-friendly semantics;
- `Last-Event-ID`;
- bounded per-connection queue.

### 18.3 Учет в `io_uring` архитектуре

И WebSocket, и SSE должны быть вписаны в единый ownership model reactor-а без фоновых blocking writers.

---

## 19. Интеграция с `liboas`

### 19.1 Граница ответственности

`iohttp` должен предоставлять `liboas`:

- runtime request abstraction;
- route match result;
- security context;
- response abstraction;
- hooks/middleware точки интеграции.

`iohttp` не должен перекладывать в `liboas`:

- TLS handshake;
- cert verification;
- PROXY protocol parsing;
- routing.

### 19.2 Обязательные API-точки

Нужно предусмотреть:

- pre-handler request validation hook;
- post-handler response validation hook;
- publish helper для `/openapi.json`;
- route metadata attachment.

### 19.3 Что особенно важно

`iohttp` должен уметь отдать наверх уже нормализованный контекст:

- real client IP;
- TLS/mTLS status;
- auth extraction results;
- route params;
- selected content type.

---

## 20. Наблюдаемость и эксплуатация

### 20.1 Логирование

Нужны:

- structured access log;
- error log;
- connection-level diagnostics;
- rate-limited logging для шумных ошибок.

### 20.2 Метрики

Нужны как минимум:

- active connections;
- request latency;
- bytes in/out;
- handshake failures;
- parser errors;
- router misses;
- response status counters;
- websocket active count;
- backpressure / dropped queue counters.

### 20.3 Debug/trace facilities

Желательны:

- debug counters по ring operations;
- CQE error decoding;
- buffer ring statistics;
- zero-copy usage counters;
- timeout counters.

---

## 21. Безопасность по умолчанию

Сервер должен по умолчанию иметь:

- безопасные лимиты;
- строгий парсинг request line / headers;
- protection against oversized inputs;
- slow client protection;
- path traversal protection;
- trusted proxy policy;
- safe TLS defaults;
- возможность включать HSTS/CSP/nosniff/X-Frame-Options.

---

## 22. Этапы реализации

### P0 — Core runtime

- single-reactor baseline;
- multishot accept;
- provided buffers;
- HTTP/1.1;
- basic router;
- static responses;
- basic wolfSSL integration.

### P1 — Production HTTP/1.1

- middleware;
- static files;
- SPA fallback;
- PROXY protocol;
- metrics/logging;
- response builder;
- request limits and timeouts.

### P2 — Scale-up runtime

- multi-reactor ring-per-thread;
- zero-copy send policy;
- registered files/buffers optimization;
- richer TLS/mTLS metadata;
- `liboas` adapter.

### P3 — HTTP/2 and advanced features

- HTTP/2;
- WebSocket;
- SSE;
- advanced observability;
- graceful drain/shutdown polish.

### P4 — HTTP/3 / QUIC

- QUIC transport runtime;
- HTTP/3;
- production hardening and performance tuning.

---

## 23. Рекомендуемая структура каталогов

```text
iohttp/
  include/iohttp/
    io_server.h
    io_request.h
    io_response.h
    io_router.h
    io_middleware.h
    io_tls.h
    io_conn.h
    io_metrics.h

  src/core/
    io_loop.c
    io_worker.c
    io_conn.c
    io_timeout.c
    io_buffer.c
    io_fdreg.c

  src/net/
    io_listener.c
    io_accept.c
    io_socket.c
    io_proxy_proto.c

  src/tls/
    io_tls_wolfssl.c
    io_tls_peer.c
    io_tls_alpn.c

  src/http/
    io_http1.c
    io_http2.c
    io_http3.c
    io_request.c
    io_response.c

  src/router/
    io_router.c
    io_route_group.c

  src/middleware/
    io_logging.c
    io_metrics.c
    io_auth.c
    io_cors.c
    io_security.c
    io_oas.c

  src/static/
    io_static.c
    io_spa.c
    io_compress.c

  src/ws/
    io_websocket.c
    io_sse.c

  tests/
```

---

## 24. Ключевые обновленные рекомендации

С учетом уточнения про `io_uring` должны быть зафиксированы следующие решения:

1. `io_uring` является **ядром runtime**, а не сменным backend-слоем.
2. Основной production-профиль — **multi-reactor, ring-per-thread**.
3. Connection ownership должен быть строгим и не мигрировать между worker-ами без явного handoff.
4. wolfSSL остается встроенным TLS backend `iohttp`.
5. PROXY protocol должен разбираться **до** TLS handshake на доверенных listener-ах.
6. `iohttp` должен предоставлять верхним слоям нормализованный connection/security context.
7. `liboas` интегрируется через adapter/middleware к `iohttp`, а не через wolfSSL.
8. SPA support должен быть частью core web-serving профиля, а не внешним дополнением.
9. Архитектура должна быть заранее готова к HTTP/2 и HTTP/3, но без перегрузки P0/P1.

---

## 25. Критерии приемки

Архитектурный профиль `iohttp` считается соответствующим данному документу, если:

1. сервер строится вокруг `io_uring` и не зависит от epoll-centric модели;
2. имеет четкую ownership-модель соединений и worker-ов;
3. использует bounded memory и управляемые буферы;
4. имеет встроенную поддержку wolfSSL и PROXY protocol;
5. поддерживает production-ready HTTP/1.1 и архитектурно готов к HTTP/2/HTTP/3;
6. умеет безопасно раздавать SPA и статические ресурсы;
7. предоставляет middleware-friendly unified request/response abstraction;
8. предоставляет корректную интеграционную поверхность для `liboas`.

