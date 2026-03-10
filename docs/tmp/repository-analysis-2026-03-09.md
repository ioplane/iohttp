# Анализ репозитория `iohttp` (2026-03-09)

## 1) Резюме

- Проект: embedded HTTP server на C23 (`io_uring`, wolfSSL, HTTP/1.1/2/3, router, middleware, static, ws/sse).
- Проверки запускались в Podman-контейнере: `iohttp-dev` (`localhost/iohttp-dev:latest`).
- Базовый debug-контур стабилен: `46/46` тестов проходят.
- Найдены критичные ошибки памяти под ASan:
  - `heap-use-after-free` в HTTP/2 response data provider.
  - `stack-use-after-return` в роутере (lifetime path params).
- `quality.sh` падает из-за PVS-предупреждений в тестах (не production-код, но CI шумит/краснеет).

## 2) Что проверено

Выполнено внутри Podman:

1. `./scripts/quality.sh`
2. `cmake --preset clang-asan`
3. `cmake --build --preset clang-asan`
4. `ctest --preset clang-asan --output-on-failure`
5. Повторный запуск только упавших ASan-тестов:
   `ctest --test-dir build/clang-asan -R "test_ioh_router|test_ioh_websocket|test_ioh_http2|test_http2_server" --output-on-failure`

Результаты:

- `quality.sh`: `PASS: 5, FAIL: 1` (падение на PVS в тестах).
- `clang-debug` (через quality): `46/46` passed.
- `clang-asan`: `42/46` passed (падают `test_ioh_router`, `test_ioh_http2`, `test_http2_server`; `test_ioh_websocket` при повторном запуске проходит).

## 3) Карта проекта

### Основные директории

- `src/core/` — loop/server/conn/buffer/ctx/log
- `src/http/` — request/response, HTTP/1.1, HTTP/2, HTTP/3, QUIC, proxy protocol, multipart
- `src/tls/` — TLS интеграция с wolfSSL
- `src/router/` — radix tree, dispatch, groups, inspect/meta
- `src/middleware/` — middleware chain + CORS/rate-limit/security/auth
- `src/static/` — static/spa/compress
- `src/ws/` — websocket + SSE
- `tests/unit/` — модульные тесты
- `tests/integration/` — интеграционные pipeline/server тесты
- `deploy/podman/` — container build environment

### Ключевые CMake-таргеты (по `CMakeLists.txt`)

- Core: `ioh_loop`, `ioh_conn`, `ioh_log`, `ioh_server`, `ioh_ctx`, `ioh_buffer`
- TLS: `ioh_tls`
- HTTP: `ioh_request`, `ioh_response`, `ioh_http1`, `ioh_http2`, `ioh_http3`, `ioh_quic`, `ioh_proxy_proto`, `ioh_multipart`, `ioh_alt_svc`
- Router: `ioh_radix`, `ioh_router`, `ioh_route_group`, `ioh_route_inspect`, `ioh_route_meta`
- Middleware: `ioh_middleware`, `ioh_cors`, `ioh_ratelimit`, `ioh_security`, `ioh_auth`
- Static/streaming: `ioh_static`, `ioh_spa`, `ioh_compress`, `ioh_sse`, `ioh_websocket`

## 4) Потенциальные ошибки и баги

## [CRITICAL] `heap-use-after-free` в HTTP/2 отправке response body

### Симптом

ASan стабильно падает в:

- `test_ioh_http2`
- `test_http2_server`

Чтение освобожденной памяти в `resp_data_read_cb`.

### Техническая причина

1. В `submit_response()` data provider сохраняет указатель на `resp->body`:
   - `src/http/ioh_http2.c:253` (`rd->body = resp->body`)
2. После `submit_response(...)` response уничтожается:
   - `src/http/ioh_http2.c:475` (`ioh_response_destroy(&resp)`)
3. `ioh_response_destroy()` освобождает `resp->body`:
   - `src/http/ioh_response.c:72`
4. Позже nghttp2 вызывает data callback, который читает уже освобожденный буфер:
   - `src/http/ioh_http2.c:171` (`memcpy(buf, rd->body + rd->offset, n)`)

### Риск

- UB в runtime при HTTP/2 ответах с body.
- Потенциальные краши/коррупция памяти под нагрузкой.

## [HIGH] `stack-use-after-return` в роутере (lifetime path params)

### Симптом

ASan стабильно падает в `test_ioh_router`:

- stack-use-after-return, чтение данных после возврата из `ioh_router_dispatch`.

### Техническая причина

1. `ioh_router_dispatch()` нормализует path во временный stack-буфер:
   - `src/router/ioh_router.c:439` (`char normalized[IOH_MAX_URI_SIZE]`)
2. `ioh_radix_lookup()` сохраняет `param.value` как указатель внутрь переданного path:
   - `src/router/ioh_radix.c:552`, `:582`
3. Эти указатели копируются в `ioh_route_match_t` и возвращаются наружу:
   - `src/router/ioh_router.c:482`, `:497`
4. Далее копируются в `req->params` в runtime:
   - `src/core/ioh_server.c:383-386`

После возврата из `ioh_router_dispatch` stack-буфер `normalized` недействителен, а указатели на него остаются в match/request параметрах.

### Риск

- Некорректные значения path params, падения, UB в middleware/handlers.

## [MEDIUM] Quality pipeline красный из-за PVS сообщений в тестах

`scripts/quality.sh` падает по двум сообщениям PVS:

- `tests/integration/test_limits.c:95` (`V590`)
- `tests/unit/test_ioh_log.c:153` (`V618`)

Это не production runtime-баги, но:

- ломают сигнал качества и CI-гейтинг;
- усложняют детект реальных регрессий.

## 5) Дополнительные наблюдения

- Основная документация (`docs/en`, `docs/ru`) заполнена и в рабочем состоянии (не TODO-заглушки).
- Архитектура хорошо модульная, тестовое покрытие широкое (unit + integration), но sanitizer-контур сейчас не зеленый.

## 6) Рекомендованный порядок исправлений

1. Исправить UAF в HTTP/2:
   - гарантировать lifetime response body до завершения stream/data callback
   - например, копировать body в stream-owned буфер (`h2_stream_data`) или передавать ownership `rd`.
2. Исправить lifetime path params в router:
   - не возвращать указатели в stack `normalized`;
   - хранить стабильные строки параметров (arena/request-owned copy).
3. Добавить/обновить ASan-регресс тесты:
   - сценарии для HTTP/2 body sending и path param dispatch.
4. Нормализовать PVS-предупреждения в тестах:
   - либо исправить код тестов, либо корректно подавить через baseline/suppress, чтобы quality не был ложнокрасным.
