# Спецификация встраиваемого HTTP-сервера (C23)

## 1. Общие положения

Сервер предназначен для встраивания в embedded-устройства с ограниченными ресурсами (MCU с 512KB+ Flash, 128KB+ RAM). Реализован на C23 с фокусом на zero-copy операции, статическую аллокацию и детерминированное поведение.

**Целевые платформы:** Linux embedded, FreeRTOS, Zephyr RTOS, bare-metal (с lwIP).

---

## 2. Поддержка протоколов

### 2.1 HTTP/1.1

- **RFC 7230** - HTTP/1.1 Message Syntax and Routing
- **RFC 7231** - HTTP/1.1 Semantics and Content
- **RFC 7232** - Conditional Requests (ETag, If-None-Match)
- **RFC 7234** - Caching
- **RFC 7235** - Authentication (Basic/Digest)

**Требования:**

- Persistent connections (Keep-Alive) с configurable таймаутами
- Pipeline обработка (опционально, с ограничением глубины очереди)
- Chunked transfer encoding для upstream/downstream
- Range requests (RFC 7233) для докачки и media streaming
- 100-continue expectation (RFC 7231 §5.1.1)

### 2.2 HTTP/2 (опционально, рекомендуется)

- **RFC 7540** - HTTP/2 Specification
- **RFC 7541** - HPACK Header Compression

**Требования:**

- Только upgrade через TLS ALPN (h2)
- Server push (опционально)
- Stream multiplexing с приоритизацией
- Flow control с configurable window sizes

### 2.3 WebSocket

- **RFC 6455** - The WebSocket Protocol

**Требования:**

- Переключение с HTTP/1.1 через Upgrade handshake
- Фреймы: text, binary, ping/pong, close
- Маскирование клиентских фреймов (проверка)
- Fragmented messages support
- Автоматический pong на ping с configurable таймаутом

### 2.4 Server-Sent Events (SSE)

- **HTML Living Standard - Server-Sent Events**: https://html.spec.whatwg.org/multipage/server-sent-events.html
- **RFC 6202** - Known Issues and Best Practices for Server-Sent Events
- **RFC 2616 §3.6.1** - Chunked Transfer Coding (required for streaming)

**Требования:**

- **Формат сообщений**: `field: value\n` с обязательным `\n\n` в конце
  - `data: <message>\n\n` — базовое сообщение
  - `id: <id>\n` — ID события для восстановления соединения
  - `event: <type>\n` — тип события (custom event name)
  - `retry: <ms>\n` — рекомендуемая задержка переподключения
- **Content-Type**: `text/event-stream; charset=utf-8`
- **Cache-Control**: `no-cache` (обязательно для предотвращения буферизации прокси)
- **Connection**: `keep-alive` с chunked transfer encoding
- **CORS**: `Access-Control-Allow-Origin` для cross-origin EventSource
- **Heartbeat**: отправка комментариев (`:ping\n\n`) каждые 30 секунд для поддержания соединения через NAT/файрволы
- **Backpressure handling**: буферизация не более N сообщений (configurable, default 100), дроп старых при переполнении
- **Multiplexing**: поддержка множественных SSE-клиентов через единый event loop (non-blocking write)
- **Graceful degradation**: закрытие потока при `close_notify` от клиента (RST или FIN)

**Embedded-специфика:**

- Поддержка "Last-Event-ID" header для восстановления потока после разрыва
- Ограничение на размер одного event (default 4KB, configurable)
- Опциональная буферизация на Flash для persistent events (кольцевой буфер)

### 2.5 PROXY Protocol

- **PROXY Protocol Specification v1/v2**: https://www.haproxy.org/download/3.4/doc/proxy-protocol.txt

**Требования:**

- **PPv1** (текстовый формат): `PROXY TCP4 192.168.1.1 192.168.1.2 12345 80\r\n`
- **PPv2** (бинарный формат): сигнатура `\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A`
- Поддержка команд: PROXY (передача адреса), LOCAL (health checks)
- Семейства адресов: AF_INET, AF_INET6, AF_UNIX
- TLV extensions (PPv2): SSL session info, connection ID, netns
- Валидация: отбрасывание соединений с невалидным PROXY header (опция `strict_mode`)
- Сохранение оригинальных адресов в структуре соединения для логирования и ACL

---

## 3. Транспортный уровень и безопасность

### 3.1 wolfSSL Integration (обязательно)

- **wolfSSL Documentation**: https://www.wolfssl.com/documentation/
- **TLS 1.3**: RFC 8446
- **TLS 1.2**: RFC 5246

**Требования:**

- Статическая линковка с wolfSSL (не динамическая загрузка)
- Конфигурация cipher suites через wolfSSL API (например, `TLS13_AES_GCM_SHA256`)
- Поддержка PSK (Pre-Shared Keys) для IoT-сценариев
- SNI (Server Name Indication) callback для мультитенантности
- Session tickets и session ID cache с configurable TTL
- OCSP stapling (опционально)

### 3.2 io_uring (Linux 5.1+, опционально но рекомендуется)

- **io_uring Documentation**: https://kernel.dk/ioh_uring.pdf (Jens Axboe)
- **liburing**: https://github.com/axboe/liburing

**Требования:**

- **Runtime detection**: проверка наличия io_uring через `io_uring_queue_init_params` с `IORING_FEAT_SINGLE_MMAP`
- **Fallback**: автоматическое переключение на epoll если io_uring недоступен (старые ядра, seccomp)

**Операции через io_uring:**

- **Multishot accept**: `IORING_OP_ACCEPT` с `IOSQE_CQE_SKIP_SUCCESS` (Linux 6.0+) — один запрос, множественные соединения
- **Async recv/send**: `IORING_OP_RECV`/`IORING_OP_SEND` без системных вызовов
- **Zero-copy file send**: `IORING_OP_SPLICE` (pipe → socket) или `IORING_OP_SEND_ZC` (Linux 6.0+, true zero-copy)
- **File I/O**: `IORING_OP_READ`/`IORING_OP_WRITE` для async чтения статики с диска
- **Timeout**: `IORING_OP_LINK_TIMEOUT` для keepalive таймаутов

**Оптимизации:**

- **Buffer ring**: `io_uring_setup_buf_ring` для автоматического выделения буферов под recv (убирает alloc в hot path)
- **Registered buffers**: `IORING_REGISTER_BUFFERS` для pinned memory (ускоряет DMA)
- **Registered files**: `IORING_REGISTER_FILES` для пропуска fd table lookup
- **Polling**: `IORING_SETUP_IOPOLL` для polled I/O (только для direct I/O, не для sockets)
- **Kernel thread polling**: `IORING_SETUP_SQPOLL` — ядерный тред сбора запросов (снижает latency, но требует CAP_SYS_ADMIN)

**Ограничения:**

- TLS через wolfSSL требует `SSL_MODE_ENABLE_PARTIAL_WRITE` + ручную обработку WANT_READ/WANT_WRITE (io_uring не совместим с blocking TLS)
- Buffer ring требует фиксированного размера буферов (подходит для embedded)
- Не доступен на RTOS (FreeRTOS/Zephyr) — только Linux

**Конфигурация:**

```c
#define HTTP_USE_IOURING 1           // Включить io_uring если доступен
#define HTTP_IOURING_QUEUE_DEPTH 256 // Размер submission/completion rings
#define HTTP_IOURING_BUF_RING_SIZE 64 // Количество буферов в buffer ring
#define HTTP_IOURING_FALLBACK_EPOLL 1 // Fallback на epoll
```

### 3.3 Сетевой I/O (Fallback для non-Linux)

- **Non-blocking I/O** обязательно (edge-triggered epoll на Linux, kqueue на BSD, poll/pollset на RTOS)
- Zero-copy sendfile для статических файлов (если ОС поддерживает)
- Scatter-gather I/O (writev) для header + body отправки
- TCP_NODELAY для WebSocket и API endpoint'ов (configurable)
- TCP_FASTOPEN (опционально)

**Memory constraints:**

- Input buffer: 16KB (TLS record size max)
- Output buffer: 16KB
- Handshake heap: configurable (default 8KB)

---

## 4. Раздача Single Page Applications (SPA)

### 4.1 Требования к статике

- **MIME-type detection** по расширению и magic bytes:
  - `.js` → `application/javascript` (RFC 4329)
  - `.mjs` → `application/javascript`
  - `.wasm` → `application/wasm` (WebAssembly)
  - `.css` → `text/css`
  - Статические ассеты: `.png`, `.jpg`, `.svg`, `.woff2` и т.д.

### 4.2 Client-side Routing Support

- **History API Fallback**: все запросы к несуществующим файлам (кроме API префиксов) → `index.html`
- **Конфигурация исключений**: список path-префиксов, которые не должны fallback'иться (например, `/api/`, `/ws/`)
- **Content-Type для fallback**: `text/html; charset=utf-8`
- **Status code**: 200 OK (не 404), так как это валидный ответ для SPA роутера

### 4.3 Кэширование и производительность

- **ETag generation**: на основе hash содержимого или mtime + size
- **Cache-Control**: 
  - `index.html`: `no-cache` (всегда свежий)
  - Hashed assets (`app.abc123.js`): `immutable, max-age=31536000`
- **Gzip/Brotli precompression**: отдача `.gz` или `.br` файлов если они существуют и клиент поддерживает (Accept-Encoding)
- **Service Worker support**: корректная обработка `scope` и `navigator.serviceWorker.register`

### 4.4 Security Headers для SPA

- `X-Frame-Options: DENY` или `SAMEORIGIN`
- `X-Content-Type-Options: nosniff`
- `Referrer-Policy: strict-origin-when-cross-origin`
- CSP (Content Security Policy) через конфигурацию

---

## 5. Архитектура и API

### 5.1 Маршрутизация (Routing)

```c
// Пример API
http_route_t routes[] = {
    { .method = HTTP_GET, .path = "/api/status", .handler = handle_status },
    { .method = HTTP_GET, .path = "/api/device/:id/config", .handler = handle_config },
    { .method = HTTP_POST, .path = "/api/device/:id/command", .handler = handle_command },
    { .method = HTTP_GET, .path = "/ws", .handler = ws_upgrade_handler, .is_websocket = true },
    { .method = HTTP_GET, .path = "/*", .handler = spa_static_handler, .is_spa_fallback = true },
    {0}
};
```

**Особенности:**

- Path parameters (`:id`) с валидацией (regex pattern в конфигурации)
- Wildcard routes с приоритетом (специфичные > общие)
- Route groups с middleware (prefix `/api/v1`)

### 5.2 Middleware Chain

- Логирование (structured JSON logs)
- CORS handling (RFC 6454)
- Rate limiting (token bucket per IP)
- Basic/Digest auth
- JWT verification (HS256/RS256)
- Request ID propagation (X-Request-ID)

### 5.3 Memory Management (C23)

- **Fixed-size memory pools** для соединений (compile-time configurable)

- **Arena allocators** для request/response lifecycle

- `[[nodiscard]]` на всех alloc-функциях

- `static_assert` для проверки размеров структур:

  ```c
  static_assert(sizeof(http_connection_t) <= 512, "Connection struct too large");
  ```

---

## 6. Конфигурация (Compile-time)

```c
// config.h
#define HTTP_MAX_CONNECTIONS 64
#define HTTP_REQUEST_BUFFER_SIZE 4096
#define HTTP_RESPONSE_BUFFER_SIZE 8192
#define HTTP_KEEPALIVE_TIMEOUT_MS 30000
#define HTTP_MAX_HEADERS 32
#define HTTP_MAX_URI_LENGTH 2048
#define HTTP_MAX_BODY_SIZE (1024 * 1024)  // 1MB

// wolfSSL specific
#define WOLFSSL_TLS13_ENABLED 1
#define WOLFSSL_PSK_ENABLED 0
#define WOLFSSL_SESSION_CACHE_SIZE 20

// SPA specific
#define SPA_INDEX_FILE "/www/index.html"
#define SPA_API_PREFIX "/api"
#define SPA_ENABLE_GZIP 1
#define SPA_ENABLE_BROTLI 0
```

---

## 7. Зависимости

**Обязательные:**

- wolfSSL (>= 5.5.0) - TLS layer
- C23 compatible compiler (GCC 13+, Clang 16+, MSVC 2022+)

**Опциональные:**

- lwIP (для bare-metal) - networking stack
- zlib (или miniz) - gzip compression
- brotli (dec) - brotli compression
- jsmn или similar - JSON parsing для API

**Недопустимые (zero-dependency philosophy):**

- glib, libevent, libuv (слишком тяжелые для embedded)
- OpenSSL (использовать wolfSSL)
- Динамическая аллокация в hot path

---

## 8. Мониторинг и отладка

### 8.1 Метрики (Prometheus format)

Эндпоинт `/metrics` (опционально, защищен auth):

```
http_requests_total{method="GET",status="200",path="/api/status"} 1024
http_request_duration_seconds_bucket{le="0.1"} 950
http_connections_active 12
http_tls_handshake_duration_seconds 0.045
wolfssl_memory_used_bytes 32768
```

### 8.2 Health Checks

- `/health` - Liveness (HTTP 200 если сервер принимает соединения)
- `/ready` - Readiness (HTTP 200 если зависимости готовы, например, БД)

### 8.3 Логирование

- Structured JSON logging:

  ```json
  {"timestamp":"2024-01-15T10:30:00Z","level":"INFO","method":"GET","path":"/api/status","status":200,"duration_ms":5,"client_ip":"192.168.1.100","proxy_protocol":true,"original_ip":"10.0.0.5"}
  ```

---

## 9. Пример использования

```c
#include "http_server.h"
#include "wolfssl/ssl.h"

static http_status_t api_handler(http_request_t *req, http_response_t *resp) {
    // PROXY protocol info доступна через req->proxy_src_addr
    if (req->proxy_protocol_used) {
        log_info("Original client: %s", req->proxy_src_addr);
    }
    
    http_json_response(resp, 200, "{\"status\":\"ok\"}");
    return HTTP_OK;
}

int main() {
    http_server_t server = {0};
    
    // wolfSSL контекст
    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    wolfSSL_CTX_use_certificate_file(ctx, "server.crt", SSL_FILETYPE_PEM);
    wolfSSL_CTX_use_PrivateKey_file(ctx, "server.key", SSL_FILETYPE_PEM);
    
    http_server_init(&server, &(http_config_t){
        .bind_addr = "0.0.0.0",
        .port = 443,
        .tls_ctx = ctx,
        .proxy_protocol = true,  // Включаем PROXY protocol
        .spa_config = {
            .index_path = "/var/www/index.html",
            .static_root = "/var/www/",
            .api_prefix = "/api/",
            .enable_gzip = true
        }
    });
    
    http_route(&server, HTTP_GET, "/api/data", api_handler);
    http_spa_enable(&server);  // Включает fallback для SPA
    
    http_server_run(&server);  // Блокирующий event loop
    return 0;
}
```

### Пример с io_uring (Linux)

```c
#include "http_server.h"
#include <liburing.h>

int main() {
    http_server_t server = {0};
    
    // io_uring конфигурация (опционально, по умолчанию epoll)
    http_io_uring_config_t uring_cfg = {
        .queue_depth = 256,
        .buf_ring_size = 64,        // Количество буферов для recv
        .buf_size = 4096,           // Размер каждого буфера
        .flags = HTTP_IOURING_USE_BUF_RING | HTTP_IOURING_MULTISHOT_ACCEPT,
        .fallback_to_epoll = true   // Fallback если ядро < 5.1
    };
    
    http_server_init(&server, &(http_config_t){
        .bind_addr = "0.0.0.0",
        .port = 8080,
        .ioh_backend = HTTP_BACKEND_IOURING,  // или HTTP_BACKEND_AUTO для автоопределения
        .io_uring = &uring_cfg,
        .worker_threads = 0  // io_uring = single-threaded + async I/O
    });
    
    // Zero-copy file send через io_uring splice
    http_route_file(&server, "/static/*", "/var/www/static/", 
                    HTTP_SENDFILE_IOURING_SPLICE);
    
    http_server_run(&server);
    return 0;
}

// Внутреннее устройство io_uring event loop (псевдокод):
// 1. io_uring_submit() - отправка batch запросов в ядро
// 2. io_uring_wait_cqe() - ожидание completion
// 3. Обработка CQE: accept -> setup recv -> parse -> send response
// 4. Для файлов: open -> read -> splice to socket (zero-copy)
```

### Пример SSE (Server-Sent Events)

```c
// Handler для /events
static http_status_t sse_handler(http_request_t *req, http_response_t *resp) {
    // Проверяем Accept header
    if (!http_header_contains(req, "Accept", "text/event-stream")) {
        return HTTP_406_NOT_ACCEPTABLE;
    }
    
    // Устанавливаем SSE headers
    http_set_header(resp, "Content-Type", "text/event-stream; charset=utf-8");
    http_set_header(resp, "Cache-Control", "no-cache");
    http_set_header(resp, "Connection", "keep-alive");
    http_set_header(resp, "Access-Control-Allow-Origin", "*");  // CORS если нужно
    
    // Отправляем HTTP 200 и сразу flush headers (chunked encoding)
    http_start_streaming(resp);
    
    // Отправляем начальное событие
    http_sse_send(resp, &(sse_event_t){
        .event = "connected",
        .data = "{\"status\":\"ok\"}",
        .id = "1"
    });
    
    // Основной цикл отправки (в реальности - из другого треда или timer callback)
    int counter = 0;
    while (http_connection_alive(req->conn) && counter < 100) {
        char data[256];
        snprintf(data, sizeof(data), "{\"time\":%lu,\"value\":%d}", time(NULL), counter++);
        
        http_sse_send(resp, &(sse_event_t){
            .event = "update",      // client: eventSource.addEventListener('update', ...)
            .data = data,
            .id = http_itoa(counter)
        });
        
        // Heartbeat каждые 30 секунд
        if (counter % 30 == 0) {
            http_sse_send_comment(resp, "ping");  // Комментарий (игнорируется клиентом)
        }
        
        sleep(1);  // В реальности - async через event loop
    }
    
    http_sse_close(resp);  // Отправляет "event: close\n\n" и закрывает соединение
    return HTTP_OK;
}

// JavaScript клиент:
// const es = new EventSource('/events');
// es.addEventListener('update', (e) => console.log(JSON.parse(e.data)));
// es.onerror = (e) => console.log('SSE error, reconnecting...');
```

---

## 10. Ссылки и стандарты

### RFCs

- [RFC 7230-7235](https://tools.ietf.org/html/rfc7230) - HTTP/1.1
- [RFC 7540](https://tools.ietf.org/html/rfc7540) - HTTP/2
- [RFC 6455](https://tools.ietf.org/html/rfc6455) - WebSocket
- [RFC 6202](https://tools.ietf.org/html/rfc6202) - Server-Sent Events Best Practices
- [RFC 2616 §3.6.1](https://tools.ietf.org/html/rfc2616#section-3.6.1) - Chunked Transfer Coding
- [RFC 8446](https://tools.ietf.org/html/rfc8446) - TLS 1.3
- [RFC 5246](https://tools.ietf.org/html/rfc5246) - TLS 1.2
- [RFC 6454](https://tools.ietf.org/html/rfc6454) - Origin definition (CORS)
- [RFC 7617](https://tools.ietf.org/html/rfc7617) - Basic HTTP Authentication
- [RFC 7616](https://tools.ietf.org/html/rfc7616) - Digest HTTP Authentication
- [RFC 7233](https://tools.ietf.org/html/rfc7233) - Range Requests

### Спецификации

- [PROXY Protocol v1/v2](https://www.haproxy.org/download/3.4/doc/proxy-protocol.txt) - HAProxy
- [wolfSSL Manual](https://www.wolfssl.com/documentation/manuals/wolfssl/index.html)
- [WHATWG HTML Living Standard - Server-Sent Events](https://html.spec.whatwg.org/multipage/server-sent-events.html)
- [WebAssembly MIME type](https://webassembly.github.io/spec/core/binary/conventions.html)
- [io_uring whitepaper](https://kernel.dk/ioh_uring.pdf) - Jens Axboe (PDF)
- [liburing GitHub](https://github.com/axboe/liburing) - Official userspace library

### C23 Standard

- [ISO/IEC 9899:2024](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3096.pdf) - Working Draft

---
