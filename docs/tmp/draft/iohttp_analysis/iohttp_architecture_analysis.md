# Архитектурный анализ iohttp — встроенного HTTP-сервера на C23

## Резюме

iohttp представляет собой современную, хорошо продуманную архитектуру встроенного HTTP-сервера, 
которая использует передовые подходы системного программирования. Основные сильные стороны: 
использование io_uring, делегирование протокольной логики проверенным библиотекам, 
модульная структура и фокус на zero-copy путях.

**Общая оценка: 8.5/10**

---

## 1. ОЦЕНКА АРХИТЕКТУРНЫХ ПАТТЕРНОВ

### ✅ Что сделано правильно

#### 1.1 Паттерн "Delegate, Don't Implement"
**Решение:** Использование battle-tested библиотек для протокольной логики
- HTTP/1.1: picohttpparser
- HTTP/2: nghttp2  
- HTTP/3: ngtcp2 + nghttp3
- TLS: wolfSSL

**Почему это правильно:**
- Снижение объёма собственного кода с ~80K до ~12-20K LOC
- Использование проверенных реализаций протоколов
- Фокус на интеграционном слое — где реальная ценность
- Быстрый time-to-market и снижение рисков безопасности

#### 1.2 Event-Driven I/O с io_uring
**Решение:** io_uring как единый I/O engine

**Преимущества:**
- Единый интерфейс для всех I/O операций
- Эффективное batching системных вызовов
- Поддержка advanced features: MULTISHOT_ACCEPT, SEND_ZC, SPLICE
- Потенциал для SQPOLL mode (polling submit queue)

#### 1.3 Layered Architecture (Слоистая архитектура)
```
User Application
    ↓
Public C API
    ↓
Router/Middleware/Static/WebSocket/SSE
    ↓
HTTP Protocol Layer (HTTP/1.1/2/3)
    ↓
TLS Layer (wolfSSL)
    ↓
I/O Engine (io_uring)
    ↓
Linux Kernel
```

**Плюсы:**
- Чёткое разделение ответственности
- Возможность тестирования слоёв изолированно
- Лёгкость замены реализаций на уровне слоя

#### 1.4 State Machine для соединений
**Состояния:** ACCEPTING → PROXY_HEADER → TLS_HANDSHAKE → PROTOCOL_NEGOTIATION → HTTP_ACTIVE → DRAINING → CLOSING → CLOSED

**Преимущества:**
- Предсказуемое поведение
- Легко отслеживать и логировать
- Упрощает обработку ошибок
- Поддержка альтернативных путей (WebSocket, SSE)

#### 1.5 Typed User Data Packing
**Решение:** `(conn_id << 8) | op_type` для CQE dispatch

**Плюсы:**
- Эффективное использование 64-битного user_data
- Быстрая диспетчеризация без хеш-таблиц
- Cache-friendly

#### 1.6 Memory Pool Pattern
- Fixed-size connection pool (default 256)
- Arena allocators per-request
- Provided buffer rings (kernel-managed)

**Преимущества:**
- Предсказуемое потребление памяти
- Минимизация malloc/free в hot path
- Лучшая cache locality

#### 1.7 Zero-Copy Paths
- splice() для файлов
- SEND_ZC для сетевого ввода-вывода
- Registered buffers/files для DMA

**Влияние на производительность:** Критически важно для высоконагруженных систем

---

### ⚠️ Области для улучшения паттернов

#### 1.8 Отсутствие явного Backpressure механизма
**Проблема:** Нет описания механизма обратного давления при перегрузке

**Рекомендация:**
```c
// Добавить в конфигурацию
struct ioh_backpressure_config {
    size_t max_pending_requests;      // Макс. запросов в очереди
    size_t max_request_queue_bytes;   // Макс. байт в очереди
    uint32_t overload_threshold_ms;   // Порог для срабатывания
    void (*on_overload)(ioh_server_t*); // Callback при перегрузке
};
```

#### 1.9 Недостаточно деталей про Multi-Reactor Pattern
**Проблема:** "Single-reactor (dev) или multi-reactor ring-per-thread (production)" — 
но нет деталей балансировки нагрузки между реакторами

**Рекомендация:**
- SO_REUSEPORT для распределения соединений
- Work stealing между реакторами
- CPU affinity для потоков

#### 1.10 Отсутствие Circuit Breaker паттерна
**Проблема:** Нет защиты от каскадных отказов при проблемах с upstream

**Рекомендация:** Добавить middleware с circuit breaker для проксирования

---

## 2. ПОТЕНЦИАЛЬНЫЕ РИСКИ И ПРОБЛЕМЫ МАСШТАБИРОВАНИЯ

### 🔴 Высокий риск

#### 2.1 Connection Pool Size — Fixed Size
**Проблема:** "Fixed-size connection pool (default 256)"

**Риски:**
- 256 соединений может быть недостаточно для high-traffic сценариев
- Нет механизма динамического расширения
- Переполнение пула = отказ в обслуживании

**Рекомендация:**
```c
// Динамический пул с лимитами
typedef struct {
    size_t min_connections;      // Минимальный размер
    size_t max_connections;      // Максимальный размер (hard limit)
    size_t growth_factor;        // На сколько расширяться
    float scale_up_threshold;    // При какой загрузке расширяться
    float scale_down_threshold;  // При какой загрузке сжиматься
} ioh_pool_scaling_config_t;
```

#### 2.2 Single Point of Contention в CQE Dispatch
**Проблема:** Typed user_data packing хорош для single-reactor, 
но в multi-reactor может потребоваться per-reactor connection ID space

**Риск:** Конфликты ID при масштабировании

**Рекомендация:**
```c
// Структурированный user_data
typedef union {
    struct {
        uint16_t reactor_id;    // 0-65535 reactors
        uint16_t conn_id;       // Per-reactor connection ID
        uint8_t  op_type;       // Operation type
        uint8_t  reserved;      // Padding/flags
    } fields;
    uint64_t raw;
} ioh_cqe_user_data_t;
```

#### 2.3 Buffer Management при высокой нагрузке
**Проблема:** "Provided buffer rings — kernel-managed" — но нет деталей:
- Какой размер буферов?
- Сколько буферов в кольце?
- Что при исчерпании?

**Риск:** Исчерпание буферов = dropped connections

**Рекомендация:**
```c
typedef struct {
    size_t buffer_size;           // Размер каждого буфера
    size_t num_buffers;           // Количество буферов
    size_t low_watermark;         // Порог для аллокации доп. буферов
    size_t high_watermark;        // Порог для освобождения
    bool enable_dynamic_growth;   // Динамическое расширение
} ioh_buffer_ring_config_t;
```

### 🟡 Средний риск

#### 2.4 TLS Handshake Blocking
**Проблема:** TLS handshake может быть CPU-intensive и блокировать реактор

**Риск:** При большом количестве TLS handshake'ов — деградация производительности

**Рекомендация:**
- Offload TLS handshake в отдельный thread pool
- Async TLS с состоянием HANDSHAKING
- Session resumption (уже есть, но проверить coverage)

#### 2.5 HTTP/2 и HTTP/3 — сложность интеграции
**Проблема:** Три разных библиотеки с разными API

**Риск:**
- Сложность unified abstraction
- Потенциальные утечки ресурсов
- Разные подходы к flow control

**Рекомендация:**
- Тщательное тестирование граничных случаев
- Фаззинг для всех трёх протоколов
- Единый интерфейс flow control

#### 2.6 Router — Radix Trie Scalability
**Проблема:** Radix trie хорош для чтения, но:
- Перестроение при динамической регистрации маршрутов
- Per-method trees = умножение памяти на число методов

**Риск:** При тысячах маршрутов — высокое потребление памяти

**Рекомендация:**
- Ленивая инициализация per-method trees
- Компрессия префиксов
- Ограничение на глубину дерева

### 🟢 Низкий риск

#### 2.7 C23 #embed для статических файлов
**Проблема:** #embed компилирует файлы в бинарник

**Риск:** Размер бинарника при большом количестве статических файлов

**Рекомендация:** Использовать #embed только для критичных файлов, остальное — runtime loading

---

## 3. КОНКРЕТНЫЕ УЛУЧШЕНИЯ АРХИТЕКТУРЫ

### 3.1 Добавить Observability Layer

```c
// Структурированная телеметрия
typedef struct {
    // Connection metrics
    atomic_size_t active_connections;
    atomic_size_t total_connections;
    atomic_size_t rejected_connections;
    
    // Request metrics
    atomic_size_t requests_total;
    atomic_size_t requests_by_status[6]; // 1xx, 2xx, 3xx, 4xx, 5xx, error
    
    // Latency histograms
    ioh_histogram_t request_latency_us;
    ioh_histogram_t tls_handshake_ms;
    
    // I/O metrics
    atomic_size_t bytes_read;
    atomic_size_t bytes_written;
    atomic_size_t zero_copy_transfers;
    
    // Error metrics
    atomic_size_t ioh_errors;
    atomic_size_t protocol_errors;
    atomic_size_t timeout_errors;
} ioh_server_metrics_t;
```

### 3.2 Graceful Shutdown

```c
typedef enum {
    IOH_SHUTDOWN_GRACEFUL,    // Дождаться завершения запросов
    IOH_SHUTDOWN_IMMEDIATE,   // Закрыть все соединения
    IOH_SHUTDOWN_DRAIN        // Не принимать новые, дождаться текущих
} ioh_shutdown_mode_t;

typedef struct {
    ioh_shutdown_mode_t mode;
    uint32_t graceful_timeout_ms;
    void (*on_shutdown_complete)(void* user_data);
} ioh_shutdown_config_t;
```

### 3.3 Health Check Endpoint

```c
// Встроенный health check
typedef struct {
    const char* path;                    // "/health"
    ioh_health_status_t (*check)(void*);  // Custom check function
    bool include_metrics;                // Добавить метрики в ответ
} ioh_health_config_t;

// Ответ: {"status":"healthy","checks":{"memory":true,"disk":true},"uptime":3600}
```

### 3.4 Request Context с Propagation

```c
// Контекст запроса для tracing и metadata
typedef struct ioh_request_context {
    uuid_t request_id;              // Уникальный ID запроса
    uuid_t trace_id;                // Distributed trace ID
    uuid_t parent_span_id;          // Parent span для tracing
    struct timespec start_time;     // Начало обработки
    
    // Baggage для propagation
    ioh_baggage_t* baggage;
    
    // Custom user data
    void* user_data;
    void (*user_data_destructor)(void*);
} ioh_request_context_t;

// Middleware получает контекст
typedef int (*ioh_middleware_fn_t)(
    ioh_request_t* req, 
    ioh_response_t* resp,
    ioh_request_context_t* ctx,
    ioh_next_fn_t next
);
```

### 3.5 Plugin/Extension System

```c
// Динамическая загрузка расширений
typedef struct {
    const char* name;
    uint32_t version;
    
    // Lifecycle hooks
    int (*init)(ioh_server_t* server, void* config);
    void (*shutdown)(ioh_server_t* server);
    
    // Request hooks
    int (*pre_request)(ioh_request_t* req, ioh_request_context_t* ctx);
    int (*post_request)(ioh_request_t* req, ioh_response_t* resp, ioh_request_context_t* ctx);
    
    // Connection hooks
    void (*on_connect)(ioh_connection_t* conn);
    void (*on_disconnect)(ioh_connection_t* conn, ioh_disconnect_reason_t reason);
} ioh_plugin_t;

int ioh_server_register_plugin(ioh_server_t* server, const ioh_plugin_t* plugin);
```

### 3.6 Improved Rate Limiting

```c
// Distributed rate limiting с поддержкой Redis
typedef struct {
    // Local token bucket
    double tokens_per_second;
    size_t bucket_capacity;
    
    // Distributed backend (optional)
    ioh_rate_limit_backend_t* backend;
    
    // Key extraction
    ioh_rate_limit_key_fn_t key_fn;  // IP, user, API key, etc.
    
    // Response
    uint16_t status_code;           // 429 by default
    const char* retry_after_header; // Добавить Retry-After
} ioh_rate_limit_config_t;
```

### 3.7 Configuration Hot Reload

```c
// Перезагрузка конфигурации без рестарта
typedef struct {
    const char* config_file_path;
    uint32_t reload_interval_ms;
    bool watch_file_changes;
    
    // Callbacks
    void (*on_reload_start)(void);
    void (*on_reload_success)(void);
    void (*on_reload_error)(const char* error);
} ioh_config_reload_t;

int ioh_server_reload_config(ioh_server_t* server);
```

---

## 4. ОЦЕНКА SEPARATION OF CONCERNS И МОДУЛЬНОСТИ

### 4.1 Оценка по слоям

| Слой | Separation | Cohesion | Coupling | Оценка |
|------|------------|----------|----------|--------|
| I/O Engine | ✅ Отлично | ✅ Высокая | ✅ Слабое | 9/10 |
| TLS Layer | ✅ Отлично | ✅ Высокая | ✅ Слабое | 9/10 |
| HTTP Protocol | ✅ Хорошо | ✅ Высокая | ⚠️ Среднее | 8/10 |
| Router | ✅ Отлично | ✅ Высокая | ✅ Слабое | 9/10 |
| Middleware | ✅ Хорошо | ✅ Высокая | ✅ Слабое | 8/10 |
| Static Files | ✅ Хорошо | ✅ Высокая | ✅ Слабое | 8/10 |
| WebSocket/SSE | ⚠️ Средне | ⚠️ Средняя | ⚠️ Среднее | 6/10 |

### 4.2 Сильные стороны модульности

#### ✅ Чёткие границы модулей
```
src/
├── core/      # I/O Engine — только I/O
├── tls/       # TLS Layer — только криптография
├── http/      # Protocol — только парсинг HTTP
├── router/    # Routing — только маршрутизация
├── middleware/# Cross-cutting concerns
├── static/    # Static files — только файлы
└── ws/        # WebSocket/SSE — realtime
```

#### ✅ Единый интерфейс между слоями
Каждый слой предоставляет чёткий C API — нет прямого доступа к внутренностям

#### ✅ Dependency Injection через config
```c
ioh_server_config_t config = {
    .tls_config = &tls_cfg,      // Inject TLS
    .router = &my_router,        // Inject router
    .middleware_chain = chain,   // Inject middleware
};
```

### 4.3 Области для улучшения модульности

#### ⚠️ WebSocket/SSE — смешение ответственности
**Проблема:** Один модуль для двух разных протоколов

**Рекомендация:**
```
src/
├── websocket/     # RFC 6455 WebSocket
│   ├── frame.c    # Frame parsing
│   ├── handshake.c # Upgrade handling
│   └── handler.c  # Message handling
└── sse/           # Server-Sent Events
    ├── event.c    # Event formatting
    ├── session.c  # Session management
    └── retry.c    # Reconnection logic
```

#### ⚠️ Middleware — жёсткая цепочка
**Проблема:** Middleware организованы в линейную цепочку

**Рекомендация:** Поддержка условного выполнения:
```c
typedef struct {
    ioh_middleware_fn_t fn;
    ioh_middleware_condition_fn_t condition;  // Условие выполнения
    uint32_t priority;                        // Порядок выполнения
    const char* name;                         // Для debugging
} ioh_middleware_entry_t;

// Пример: CORS только для OPTIONS и cross-origin
bool cors_condition(ioh_request_t* req) {
    return strcmp(req->method, "OPTIONS") == 0 || 
           is_cross_origin(req);
}
```

#### ⚠️ HTTP Protocol Layer — три реализации
**Проблема:** HTTP/1.1, HTTP/2, HTTP/3 в одном слое

**Рекомендация:** Явное разделение:
```
src/http/
├── common/        # Общие типы и утилиты
├── http1/         # HTTP/1.1 implementation
├── http2/         # HTTP/2 implementation
└── http3/         # HTTP/3 implementation
```

### 4.4 Рекомендуемая структура модулей

```
iohttp/
├── include/iohttp/          # Public API
│   ├── server.h
│   ├── router.h
│   ├── middleware.h
│   └── websocket.h
├── src/
│   ├── core/               # I/O Engine (internal)
│   │   ├── ring.c          # io_uring operations
│   │   ├── buffer.c        # Buffer management
│   │   └── connection.c    # Connection state machine
│   ├── tls/                # TLS abstraction
│   │   ├── wolfssl.c       # wolfSSL implementation
│   │   └── common.h        # TLS interface
│   ├── protocol/           # Protocol implementations
│   │   ├── http1/
│   │   ├── http2/
│   │   └── http3/
│   ├── routing/            # Request routing
│   │   ├── radix.c         # Radix tree
│   │   └── router.c        # Router logic
│   ├── middleware/         # Built-in middleware
│   │   ├── cors.c
│   │   ├── ratelimit.c
│   │   └── auth.c
│   ├── static/             # Static file serving
│   ├── realtime/           # WebSocket & SSE
│   │   ├── websocket/
│   │   └── sse/
│   └── utils/              # Utilities
│       ├── arena.c         # Arena allocator
│       ├── pool.c          # Memory pools
│       └── metrics.c       # Metrics collection
├── tests/                  # Test suites
└── examples/               # Example applications
```

---

## 5. ИТОГОВАЯ ОЦЕНКА

### Общий балл: 8.5/10

| Критерий | Балл | Комментарий |
|----------|------|-------------|
| Модульность | 9/10 | Отличное разделение, чёткие границы |
| Масштабируемость | 7/10 | Нужна работа над multi-reactor и backpressure |
| Производительность | 9/10 | io_uring, zero-copy — отлично |
| Безопасность | 8/10 | wolfSSL, bounds checking — хорошо |
| Сопровождаемость | 9/10 | ~12-20K LOC, делегирование — отлично |
| Наблюдаемость | 6/10 | Нужен metrics layer, tracing |
| Гибкость | 8/10 | Middleware, конфигурация — хорошо |

### Приоритеты для улучшения

1. **🔴 Высокий:** Добавить observability (metrics, tracing, structured logging)
2. **🔴 Высокий:** Graceful shutdown и backpressure
3. **🟡 Средний:** Plugin system для расширяемости
4. **🟡 Средний:** Hot reload конфигурации
5. **🟢 Низкий:** Разделение WebSocket/SSE модулей

### Сравнение с конкурентами

| Фича | iohttp | H2O | nginx | Caddy |
|------|--------|-----|-------|-------|
| HTTP/3 | ✅ | ✅ | ⚠️ | ✅ |
| io_uring | ✅ | ❌ | ❌ | ❌ |
| Embedded | ✅ | ⚠️ | ❌ | ❌ |
| LOC | ~15K | ~80K | ~160K | ~50K |
| C23 | ✅ | ❌ | ❌ | ❌ |
| wolfSSL native | ✅ | ❌ | ❌ | ❌ |

**Вывод:** iohttp занимает уникальную нишу — современный embedded server 
с минимальным footprint и максимальной производительностью.
