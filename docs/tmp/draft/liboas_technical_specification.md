# ТЕХНИЧЕСКОЕ ЗАДАНИЕ

## liboas — Библиотека OpenAPI 3.2.0 для C23

---

**Версия документа:** 1.0  
**Дата:** 2025  
**Статус:** Утверждено  
**Целевой проект:** WolfGuard VPN (C++17/Qt 6, WolfSSL)

---

## СОДЕРЖАНИЕ

1. [Введение](#1-введение)
2. [Общее описание](#2-общее-описание)
3. [Требования к библиотеке](#3-требования-к-библиотеке)
4. [Архитектура системы](#4-архитектура-системы)
5. [Объектная модель](#5-объектная-модель)
6. [API интерфейс](#6-api-интерфейс)
7. [Управление памятью](#7-управление-памятью)
8. [Валидация](#8-валидация)
9. [Зависимости](#9-зависимости)
10. [План разработки](#10-план-разработки)
11. [Тестирование](#11-тестирование)
12. [Приложения](#12-приложения)

---

## 1. ВВЕДЕНИЕ

### 1.1 Цель документа

Настоящее техническое задание определяет требования к разработке библиотеки **liboas** — высокопроизводительной C-библиотеки для работы со спецификациями OpenAPI 3.2.0. Документ предназначен для разработчиков, архитекторов и технических руководителей проекта WolfGuard VPN.

### 1.2 Область применения

Библиотека liboas предназначена для:
- Парсинга OpenAPI 3.2.0 спецификаций в форматах JSON и YAML
- Валидации HTTP-запросов и ответов согласно спецификации
- Раздачи OpenAPI спецификации через Swagger UI
- Интеграции с management API WolfGuard VPN

### 1.3 Определения и сокращения

| Термин | Определение |
|--------|-------------|
| **OAS** | OpenAPI Specification — спецификация описания REST API |
| **liboas** | Название разрабатываемой библиотеки |
| **WolfGuard** | Целевой проект VPN-системы |
| **$ref** | Механизм ссылок в OpenAPI для повторного использования компонентов |
| **JSON Schema** | Система описания структуры JSON-документов |
| **Arena allocator** | Аллокатор памяти с линейным выделением и массовым освобождением |
| **String view** | Не владеющая ссылка на строку (данные + длина) |
| **Zero-copy** | Подход к обработке данных без копирования |

### 1.4 Ссылки на нормативные документы

| Документ | Версия | Ссылка |
|----------|--------|--------|
| OpenAPI Specification | 3.2.0 | https://spec.openapis.org/oas/v3.2.0.html |
| JSON Schema | 2020-12 | https://json-schema.org/draft/2020-12/schema |
| JSON Schema Validation | 2020-12 | https://json-schema.org/draft/2020-12/json-schema-validation |
| YAML Specification | 1.2 | https://yaml.org/spec/1.2.2/ |
| RFC 8259 | — | The JavaScript Object Notation (JSON) Data Interchange Format |
| RFC 8628 | — | OAuth 2.0 Device Authorization Grant |

---

## 2. ОБЩЕЕ ОПИСАНИЕ

### 2.1 Общая характеристика решаемых задач

Библиотека liboas решает следующие задачи:

1. **Парсинг спецификаций** — загрузка и разбор OpenAPI 3.2.0 документов из JSON и YAML
2. **Моделирование** — представление всех 30 объектов OAS в виде C-структур
3. **Разрешение ссылок** — обработка `$ref` ссылок между компонентами
4. **Валидация запросов** — проверка входящих HTTP-запросов на соответствие спецификации
5. **Валидация ответов** — проверка исходящих HTTP-ответов
6. **Swagger UI** — раздача спецификации и Swagger UI для документирования API

### 2.2 Описание целевого проекта (WolfGuard)

**WolfGuard VPN** — высокопроизводительная VPN-система со следующими характеристиками:

| Параметр | Значение |
|----------|----------|
| **Язык разработки** | C++17 |
| **GUI фреймворк** | Qt 6 |
| **TLS библиотека** | WolfSSL |
| **HTTP сервер** | Встроенный (Qt Network) |
| **Management API** | REST API с OpenAPI спецификацией |

**Требования к интеграции:**
- Потокобезопасность для многопоточной обработки запросов
- Минимальное потребление памяти (embedded-friendly)
- Высокая производительность валидации (< 1ms на запрос)
- Отсутствие зависимостей от C++ STL для core библиотеки

### 2.3 Ограничения и допущения

**Ограничения:**
- Поддержка только OpenAPI 3.2.0 (без обратной совместимости с 2.0/3.0/3.1)
- Минимальная версия компилятора: GCC 13+, Clang 16+, MSVC 2022+
- Целевая архитектура: x86_64, ARM64

**Допущения:**
- Спецификация загружается один раз при старте (read-heavy workload)
- Размер спецификации не превышает 10MB
- Входящие запросы валидируются в одном потоке (thread-local валидаторы)

---

## 3. ТРЕБОВАНИЯ К БИБЛИОТЕКЕ

### 3.1 Функциональные требования

#### 3.1.1 Парсинг спецификаций

| ID | Требование | Приоритет |
|----|------------|-----------|
| F-001 | Парсинг JSON формата | Обязательно |
| F-002 | Парсинг YAML формата | Обязательно |
| F-003 | Поддержка всех 30 объектов OAS 3.2.0 | Обязательно |
| F-004 | Разрешение внешних `$ref` ссылок | Обязательно |
| F-005 | Разрешение внутренних `$ref` ссылок | Обязательно |
| F-006 | Поддержка `$dynamicRef` и `$dynamicAnchor` | Обязательно |
| F-007 | Детальные сообщения об ошибках парсинга | Обязательно |
| F-008 | Поддержка JSON5 (опционально) | Желательно |

#### 3.1.2 Валидация запросов

| ID | Требование | Приоритет |
|----|------------|-----------|
| F-101 | Валидация HTTP метода | Обязательно |
| F-102 | Валидация пути (path) | Обязательно |
| F-103 | Валидация path-параметров | Обязательно |
| F-104 | Валидация query-параметров | Обязательно |
| F-105 | Валидация header-параметров | Обязательно |
| F-106 | Валидация cookie-параметров | Обязательно |
| F-107 | Валидация тела запроса (JSON Schema) | Обязательно |
| F-108 | Валидация content-type | Обязательно |
| F-109 | Поддержка всех стилей параметров | Обязательно |

#### 3.1.3 Валидация ответов

| ID | Требование | Приоритет |
|----|------------|-----------|
| F-201 | Валидация статус-кода | Обязательно |
| F-202 | Валидация заголовков ответа | Обязательно |
| F-203 | Валидация тела ответа | Обязательно |
| F-204 | Соответствие content-type | Обязательно |

#### 3.1.4 Swagger UI

| ID | Требование | Приоритет |
|----|------------|-----------|
| F-301 | Раздача OpenAPI спецификации по HTTP | Обязательно |
| F-302 | Встраивание Swagger UI статики | Обязательно |
| F-303 | Конфигурация путей спецификации и UI | Обязательно |

### 3.2 Нефункциональные требования

#### 3.2.1 Производительность

| Метрика | Требование |
|---------|------------|
| Время парсинга спецификации (10MB) | < 100 мс |
| Время валидации запроса | < 1 мс |
| Пропускная способность валидации | > 10,000 RPS |
| Потребление памяти (спецификация 1MB) | < 5 MB |
| Потребление памяти (спецификация 10MB) | < 50 MB |

#### 3.2.2 Потокобезопасность

| Требование | Описание |
|------------|----------|
| Парсер | Потокобезопасен для независимых документов |
| Валидатор | Требует thread-local экземпляры |
| Реестр объектов | Потокобезопасен после инициализации |
| Arena allocator | Не потокобезопасен (один arena на поток) |

#### 3.2.3 Надёжность

| Требование | Значение |
|------------|----------|
| Покрытие кода тестами | > 90% |
| Обработка некорректного входа | Graceful degradation |
| Отсутствие утечек памяти | Проверка valgrind/ASAN |
| Стабильность API | Semantic versioning |

### 3.3 Требования к совместимости

| Платформа | Статус |
|-----------|--------|
| Linux x86_64 | Обязательно |
| Linux ARM64 | Обязательно |
| macOS x86_64 | Желательно |
| macOS ARM64 | Желательно |
| Windows x86_64 | Желательно |

---

## 4. АРХИТЕКТУРА СИСТЕМЫ

### 4.1 Компонентная модель

```
┌─────────────────────────────────────────────────────────────────┐
│                         liboas Library                          │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │
│  │  swagger │  │ validate │  │   model  │  │   ref    │        │
│  │    UI    │  │          │  │          │  │ resolver │        │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘        │
│       │             │             │             │               │
│       └─────────────┴─────────────┴─────────────┘               │
│                         │                                       │
│       ┌─────────────────┼─────────────────┐                    │
│       │                 │                 │                    │
│  ┌────┴────┐      ┌────┴────┐      ┌────┴────┐                │
│  │  parser │      │  core   │      │  utils  │                │
│  │(yyjson) │      │(arena,  │      │(hash,   │                │
│  │         │      │ string) │      │ vector) │                │
│  └─────────┘      └─────────┘      └─────────┘                │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 Диаграмма зависимостей модулей

```
                    ┌─────────────┐
                    │   swagger   │
                    └──────┬──────┘
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
        ▼                  ▼                  ▼
   ┌─────────┐      ┌──────────┐       ┌──────────┐
   │ validate│◄────►│  model   │◄─────►│   ref    │
   └────┬────┘      └────┬─────┘       └────┬─────┘
        │                │                  │
        │         ┌──────┴──────┐           │
        │         │             │           │
        ▼         ▼             ▼           ▼
   ┌─────────┐ ┌──────┐   ┌─────────┐  ┌─────────┐
   │  utils  │ │parser│   │  core   │  │  utils  │
   └─────────┘ └──┬───┘   └─────────┘  └─────────┘
                  │
                  ▼
             ┌─────────┐
             │ yyjson  │
             └─────────┘
```

### 4.3 Описание модулей

#### 4.3.1 Core Module (`core/`)

| Файл | Назначение |
|------|------------|
| `arena.h` | Arena allocator для эффективного управления памятью |
| `pool.h` | Object pools для частых аллокаций |
| `string.h` | String views (zero-copy строки) |
| `error.h` | Система обработки ошибок с контекстом |
| `types.h` | Общие типы и определения |

#### 4.3.2 Parser Module (`parser/`)

| Файл | Назначение |
|------|------------|
| `json.h` | Обертка над yyjson с arena-интеграцией |
| `yaml.h` | Обертка над libyaml (опционально) |
| `reader.h` | Унифицированный reader для JSON/YAML |

#### 4.3.3 Model Module (`model/`)

| Файл | Назначение |
|------|------------|
| `oas.h` | Корневой OASDocument |
| `info.h` | Info объект |
| `server.h` | Server объекты |
| `paths.h` | Paths и PathItem |
| `operation.h` | Operation объекты |
| `parameter.h` | Parameter объекты |
| `schema.h` | Schema объекты (JSON Schema 2020-12) |
| `response.h` | Response объекты |
| `request.h` | RequestBody объекты |
| `components.h` | Components (reusable объекты) |
| `security.h` | Security schemes |
| `callback.h` | Callback объекты |

#### 4.3.4 Reference Module (`ref/`)

| Файл | Назначение |
|------|------------|
| `resolver.h` | Разрешение `$ref` ссылок |
| `registry.h` | Реестр объектов для `$ref` target |
| `cache.h` | Кэш разрешенных ссылок |

#### 4.3.5 Validation Module (`validate/`)

| Файл | Назначение |
|------|------------|
| `context.h` | Контекст валидации |
| `schema.h` | JSON Schema валидация |
| `request.h` | Валидация запросов |
| `response.h` | Валидация ответов |

#### 4.3.6 Swagger Module (`swagger/`)

| Файл | Назначение |
|------|------------|
| `ui.h` | Раздача Swagger UI |
| `spec.h` | Утилиты для раздачи спецификации |

---

## 5. ОБЪЕКТНАЯ МОДЕЛЬ

### 5.1 Полный список объектов OAS 3.2.0

| № | Объект | Раздел OAS | C-структура |
|---|--------|------------|-------------|
| 1 | OpenAPI Object | 4.1 | `OASDocument` |
| 2 | Info Object | 4.2 | `OASInfo` |
| 3 | Contact Object | 4.3 | `OASContact` |
| 4 | License Object | 4.4 | `OASLicense` |
| 5 | Server Object | 4.5 | `OASServer` |
| 6 | Server Variable Object | 4.6 | `OASServerVariable` |
| 7 | Components Object | 4.7 | `OASComponents` |
| 8 | Paths Object | 4.8 | `OASPaths` |
| 9 | Path Item Object | 4.9 | `OASPathItem` |
| 10 | Operation Object | 4.10 | `OASOperation` |
| 11 | External Documentation Object | 4.11 | `OASExternalDocs` |
| 12 | Parameter Object | 4.12 | `OASParameter` |
| 13 | Request Body Object | 4.13 | `OASRequestBody` |
| 14 | Media Type Object | 4.14 | `OASMediaType` |
| 15 | Encoding Object | 4.15 | `OASEncoding` |
| 16 | Responses Object | 4.16 | `OASResponses` |
| 17 | Response Object | 4.17 | `OASResponse` |
| 18 | Callback Object | 4.18 | `OASCallback` |
| 19 | Example Object | 4.19 | `OASExample` |
| 20 | Link Object | 4.20 | `OASLink` |
| 21 | Header Object | 4.21 | `OASHeader` |
| 22 | Tag Object | 4.22 | `OASTag` |
| 23 | Reference Object | 4.23 | `OASRef` |
| 24 | Schema Object | 4.24 | `OASSchema` |
| 25 | Discriminator Object | 4.25 | `OASDiscriminator` |
| 26 | XML Object | 4.26 | `OASXML` |
| 27 | Security Scheme Object | 4.27 | `OASSecurityScheme` |
| 28 | OAuth Flows Object | 4.28 | `OASOAuthFlows` |
| 29 | OAuth Flow Object | 4.29 | `OASOAuthFlow` |
| 30 | Security Requirement Object | 4.30 | `OASSecurityRequirement` |

### 5.2 Новые возможности OpenAPI 3.2.0

#### 5.2.1 Streaming и Sequential Media Types

```c
// Новые поля в OASMediaType
typedef struct OASMediaType {
    // ... существующие поля ...
    
    // NEW in 3.2.0: Streaming support
    OASSchema *itemSchema;        // Schema for individual items in stream
    OASEncoding *itemEncoding;    // Encoding for item boundaries
    OASEncoding *prefixEncoding;  // Prefix encoding for streaming
    
    // Supported streaming types:
    // - text/event-stream (SSE)
    // - application/jsonl (JSON Lines)
    // - application/json-seq (JSON Sequences)
    // - multipart/mixed
} OASMediaType;
```

#### 5.2.2 Nested Tags (Вложенные теги)

```c
// Новые поля в OASTag
typedef struct OASTag {
    OASStringView name;
    OASStringView description;
    OASExternalDocs *externalDocs;
    OASHashTable *extensions;
    
    // NEW in 3.2.0:
    OASStringView summary;        // Short description
    OASStringView parent;         // Parent tag name for nesting
    OASStringView kind;           // Tag classification ("nav", "badge", "internal")
} OASTag;
```

#### 5.2.3 Произвольные HTTP методы

```c
// Новые поля в OASPathItem
typedef struct OASPathItem {
    // ... стандартные методы ...
    OASOperation *get;
    OASOperation *put;
    OASOperation *post;
    OASOperation *delete;
    OASOperation *options;
    OASOperation *head;
    OASOperation *patch;
    OASOperation *trace;
    
    // NEW in 3.2.0:
    OASOperation *query;          // New QUERY method (RFC 7231 extension)
    
    // Additional arbitrary methods
    OASVector additionalOperations;  // OASAdditionalOperation[]
    
    OASStringView summary;
    OASStringView description;
    OASVector servers;
    OASVector parameters;
} OASPathItem;

// Structure for additional operations
typedef struct OASAdditionalOperation {
    OASStringView method;         // HTTP method name (e.g., "CUSTOM")
    OASOperation *operation;
} OASAdditionalOperation;
```

#### 5.2.4 OAuth 2.0 Device Authorization Flow

```c
// Новые поля в OASOAuthFlows
typedef struct OASOAuthFlows {
    OASOAuthFlow *implicit;
    OASOAuthFlow *password;
    OASOAuthFlow *clientCredentials;
    OASOAuthFlow *authorizationCode;
    
    // NEW in 3.2.0:
    OASOAuthFlow *deviceAuthorization;  // RFC 8628
} OASOAuthFlows;

// Новые поля в OASOAuthFlow
typedef struct OASOAuthFlow {
    OASStringView authorizationUrl;
    OASStringView tokenUrl;
    OASStringView refreshUrl;
    OASHashTable *scopes;         // name -> description
    
    // NEW in 3.2.0:
    OASStringView deviceAuthorizationUrl;  // RFC 8628
} OASOAuthFlow;

// Новые поля в OASSecurityScheme
typedef struct OASSecurityScheme {
    OASStringView type;           // "apiKey", "http", "oauth2", "openIdConnect", "mutualTLS"
    OASStringView description;
    OASStringView name;           // For apiKey
    OASStringView in;             // For apiKey: "query", "header", "cookie"
    OASStringView scheme;         // For http
    OASStringView bearerFormat;   // For http (bearer)
    OASOAuthFlows *flows;         // For oauth2
    OASStringView openIdConnectUrl; // For openIdConnect
    
    // NEW in 3.2.0:
    OASStringView oauth2MetadataUrl;  // OAuth 2.0 Authorization Server Metadata
    bool deprecated;
} OASSecurityScheme;
```

### 5.3 JSON Schema 2020-12 интеграция

```c
// Полная поддержка JSON Schema 2020-12
typedef struct OASSchema {
    // === Metadata ===
    OASStringView title;
    OASStringView description;
    OASStringView comment;
    OASStringView default_value;
    OASStringView example;
    bool deprecated;
    bool readOnly;
    bool writeOnly;
    
    // === Type ===
    OASSchemaType type;
    OASVector enum_values;        // OASStringView[]
    OASVector type_union;         // OASSchemaType[] for multi-type
    
    // === Composition Keywords ===
    OASVector allOf;              // OASSchemaRef[]
    OASVector anyOf;              // OASSchemaRef[]
    OASVector oneOf;              // OASSchemaRef[]
    OASSchema *notSchema;
    
    // === Conditional Keywords ===
    OASSchema *ifSchema;
    OASSchema *thenSchema;
    OASSchema *elseSchema;
    OASHashTable *dependentSchemas;
    OASVector dependentRequired;
    
    // === Dynamic References ===
    OASStringView dynamicRef;     // $dynamicRef
    OASStringView dynamicAnchor;  // $dynamicAnchor
    
    // === Unevaluated Keywords ===
    OASSchema *unevaluatedProperties;
    OASSchema *unevaluatedItems;
    
    // === Object Validation ===
    OASVector required;           // OASStringView[]
    OASHashTable *properties;     // name -> OASSchemaRef
    OASSchema *additionalProperties;
    OASStringView patternProperties;
    OASSchema *propertyNames;
    size_t minProperties;
    size_t maxProperties;
    bool additionalPropertiesBool;
    
    // === Array Validation ===
    OASSchema *items;
    OASVector prefixItems;        // OASSchemaRef[] for tuple validation
    OASSchema *contains;
    size_t minContains;
    size_t maxContains;
    size_t minItems;
    size_t maxItems;
    bool uniqueItems;
    
    // === String Validation ===
    size_t minLength;
    size_t maxLength;
    OASStringView pattern;
    OASStringView contentEncoding;
    OASStringView contentMediaType;
    OASSchema *contentSchema;
    
    // === Number Validation ===
    double minimum;
    double maximum;
    double exclusiveMinimum;
    double exclusiveMaximum;
    double multipleOf;
    bool hasMinimum;
    bool hasMaximum;
    bool hasExclusiveMinimum;
    bool hasExclusiveMaximum;
    bool hasMultipleOf;
    
    // === References ===
    OASRef *ref;
    OASSchema *resolved;
    
    // === Extensions ===
    OASHashTable *extensions;
} OASSchema;
```

---

## 6. API ИНТЕРФЕЙС

### 6.1 Публичный API

#### 6.1.1 Документ API

```c
// model/oas.h
#pragma once
#include "core/arena.h"
#include "core/string.h"
#include "core/error.h"

// Forward declarations
typedef struct OASInfo OASInfo;
typedef struct OASServer OASServer;
typedef struct OASPaths OASPaths;
typedef struct OASComponents OASComponents;
typedef struct OASSecurityRequirement OASSecurityRequirement;
typedef struct OASTag OASTag;
typedef struct OASExternalDocs OASExternalDocs;
typedef struct OASObjectRegistry OASObjectRegistry;
typedef struct OASOperation OASOperation;

// OpenAPI Document (root object)
typedef struct OASDocument {
    OASStringView openapi;           // "3.2.0"
    OASInfo *info;                   // Required
    OASVector servers;               // OASServer[]
    OASPaths *paths;                 // Required
    OASComponents *components;       // Nullable
    OASVector security;              // OASSecurityRequirement[]
    OASVector tags;                  // OASTag[]
    OASExternalDocs *externalDocs;   // Nullable
    
    // Internal
    OASArena *arena;                 // Document's arena
    OASObjectRegistry *registry;     // For $ref resolution
} OASDocument;

// HTTP methods enum (C23 with underlying type)
typedef enum OASHttpMethod : uint8_t {
    OAS_HTTP_GET = 0,
    OAS_HTTP_PUT,
    OAS_HTTP_POST,
    OAS_HTTP_DELETE,
    OAS_HTTP_OPTIONS,
    OAS_HTTP_HEAD,
    OAS_HTTP_PATCH,
    OAS_HTTP_TRACE,
    OAS_HTTP_QUERY,      // NEW in 3.2.0
    OAS_HTTP_METHOD_COUNT
} OASHttpMethod;

// Document lifecycle
[[nodiscard]] OASDocument *oas_document_create(void);
void oas_document_destroy(OASDocument *doc);

// Parsing
[[nodiscard]] OASDocument *oas_document_parse_json(
    OASArena *arena, 
    const char *json, 
    size_t len,
    OASError **error
);

[[nodiscard]] OASDocument *oas_document_parse_yaml(
    OASArena *arena,
    const char *yaml,
    size_t len,
    OASError **error
);

[[nodiscard]] OASDocument *oas_document_parse_file(
    OASArena *arena,
    const char *path,
    OASError **error
);

// Accessors
OASServer *oas_document_get_server(OASDocument *doc, size_t index);
OASOperation *oas_document_get_operation(
    OASDocument *doc,
    OASStringView path,
    OASHttpMethod method
);

// HTTP method conversion
OASHttpMethod oas_http_method_from_string(OASStringView sv);
OASStringView oas_http_method_to_string(OASHttpMethod method);
```

#### 6.1.2 Arena Allocator API

```c
// core/arena.h
#pragma once
#include <stddef.h>
#include <stdint.h>

#define OAS_ARENA_DEFAULT_CAPACITY (64 * 1024)  // 64KB chunks

typedef struct OASArenaChunk {
    uint8_t *data;
    size_t used;
    size_t capacity;
    struct OASArenaChunk *next;
} OASArenaChunk;

typedef struct OASArena {
    OASArenaChunk *current;
    OASArenaChunk *chunks;
    size_t total_allocated;
    size_t chunk_count;
} OASArena;

// Lifecycle
[[nodiscard]] OASArena *oas_arena_create(size_t initial_capacity);
void oas_arena_destroy(OASArena *arena);
void oas_arena_reset(OASArena *arena);

// Allocation
[[nodiscard]] void *oas_arena_alloc(OASArena *arena, size_t size);
[[nodiscard]] void *oas_arena_alloc_aligned(
    OASArena *arena, 
    size_t size, 
    size_t align
);

// String allocation
[[nodiscard]] char *oas_arena_strdup(OASArena *arena, const char *str);
[[nodiscard]] char *oas_arena_strndup(
    OASArena *arena, 
    const char *str, 
    size_t len
);

// Convenience macros
#define OAS_ARENA_ALLOC(arena, T) \
    ((T *)oas_arena_alloc_aligned((arena), sizeof(T), alignof(T)))

#define OAS_ARENA_ALLOC_N(arena, T, n) \
    ((T *)oas_arena_alloc_aligned((arena), sizeof(T) * (n), alignof(T)))
```

#### 6.1.3 String Views API

```c
// core/string.h
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

typedef struct OASStringView {
    const char *data;
    size_t len;
} OASStringView;

static_assert(sizeof(OASStringView) == sizeof(void *) + sizeof(size_t));

// Constructors
#define OAS_SV_LITERAL(s) ((OASStringView){.data = (s), .len = sizeof(s) - 1})
#define OAS_SV_FROM_CSTR(s) ((OASStringView){.data = (s), .len = strlen(s)})
#define OAS_SV_FROM_PTR_LEN(p, l) ((OASStringView){.data = (p), .len = (l)})

// Operations
bool oas_sv_eq(OASStringView a, OASStringView b);
bool oas_sv_eq_cstr(OASStringView sv, const char *cstr);
int oas_sv_cmp(OASStringView a, OASStringView b);
OASStringView oas_sv_slice(OASStringView sv, size_t start, size_t len);
bool oas_sv_starts_with(OASStringView sv, OASStringView prefix);
bool oas_sv_ends_with(OASStringView sv, OASStringView suffix);
uint32_t oas_sv_hash(OASStringView sv);

// Conversion
[[nodiscard]] char *oas_sv_to_cstr(OASStringView sv, OASArena *arena);
```

#### 6.1.4 Validation API

```c
// validate/validate.h
#pragma once
#include "model/oas.h"
#include "core/error.h"

typedef struct OASValidationContext {
    bool strict_mode;
    size_t max_errors;
    bool validate_formats;
    bool coerce_types;
} OASValidationContext;

typedef struct OASValidationResult {
    bool valid;
    OASError *errors;
    size_t error_count;
} OASValidationResult;

// Request validation
bool oas_validate_request(
    OASDocument *doc,
    OASStringView path,
    OASHttpMethod method,
    OASStringView query_string,
    OASStringView headers,
    OASStringView body,
    const OASValidationContext *ctx,
    OASValidationResult *result
);

// Response validation
bool oas_validate_response(
    OASDocument *doc,
    OASStringView path,
    OASHttpMethod method,
    int status_code,
    OASStringView headers,
    OASStringView body,
    const OASValidationContext *ctx,
    OASValidationResult *result
);

// JSON Schema validation
bool oas_validate_json(
    OASSchema *schema,
    const char *json,
    size_t json_len,
    const OASValidationContext *ctx,
    OASValidationResult *result
);

// Parameter validation
bool oas_validate_parameter(
    OASParameter *param,
    OASStringView value,
    OASValidationResult *result
);
```

#### 6.1.5 Swagger UI API

```c
// swagger/ui.h
#pragma once
#include "model/oas.h"

// Callback for serving content
typedef int (*oas_http_handler_t)(
    const char *path,
    const char *method,
    const char *content_type,
    const void *data,
    size_t data_len,
    void *user_data
);

// Serve OpenAPI spec
int oas_swagger_serve_spec(
    OASDocument *doc,
    oas_http_handler_t handler,
    const char *path,
    void *user_data
);

// Serve Swagger UI
int oas_swagger_serve_ui(
    oas_http_handler_t handler,
    const char *ui_path,
    const char *spec_path,
    void *user_data
);

// Combined setup
int oas_swagger_setup(
    OASDocument *doc,
    oas_http_handler_t handler,
    const char *ui_path,
    const char *spec_path,
    void *user_data
);
```

### 6.2 Примеры использования

#### 6.2.1 Базовый парсинг

```c
#include <oas/oas.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <openapi.json>\n", argv[0]);
        return 1;
    }
    
    // Create arena for the document
    OASArena *arena = oas_arena_create(0);
    
    // Parse document
    OASError *error = nullptr;
    OASDocument *doc = oas_document_parse_file(arena, argv[1], &error);
    
    if (!doc) {
        oas_error_print(error, stderr);
        oas_arena_destroy(arena);
        return 1;
    }
    
    // Access info
    printf("API: %.*s\n", (int)doc->info->title.len, doc->info->title.data);
    printf("Version: %.*s\n", (int)doc->info->version.len, doc->info->version.data);
    
    // Cleanup
    oas_arena_destroy(arena);
    return 0;
}
```

#### 6.2.2 Итерация по операциям

```c
void list_operations(OASDocument *doc) {
    OASPaths *paths = doc->paths;
    
    for (size_t i = 0; i < paths->count; i++) {
        OASPathItem *item = paths->items[i];
        
        printf("Path: %.*s\n", (int)item->path.len, item->path.data);
        
        // Check each HTTP method
        static const OASHttpMethod methods[] = {
            OAS_HTTP_GET, OAS_HTTP_POST, OAS_HTTP_PUT, 
            OAS_HTTP_DELETE, OAS_HTTP_PATCH
        };
        
        for (size_t m = 0; m < 5; m++) {
            OASOperation *op = oas_pathitem_get_operation(item, methods[m]);
            if (op) {
                OASStringView method_str = oas_http_method_to_string(methods[m]);
                printf("  %.*s: %.*s\n", 
                       (int)method_str.len, method_str.data,
                       (int)op->operationId.len, op->operationId.data);
            }
        }
    }
}
```

#### 6.2.3 Валидация запроса

```c
#include <oas/validate.h>

bool validate_request(OASDocument *doc,
                      OASStringView path,
                      OASHttpMethod method,
                      const char *request_body_json) {
    
    OASValidationContext ctx = {
        .strict_mode = true,
        .max_errors = 10,
        .validate_formats = true,
        .coerce_types = false
    };
    
    OASValidationResult result = {0};
    
    bool valid = oas_validate_request(
        doc,
        path,
        method,
        OAS_SV_LITERAL("page=1&limit=10"),
        OAS_SV_LITERAL("Content-Type: application/json"),
        OAS_SV_FROM_CSTR(request_body_json),
        &ctx,
        &result
    );
    
    if (!valid) {
        OASError *err = result.errors;
        while (err) {
            fprintf(stderr, "Validation error: %.*s\n",
                    (int)err->message.len, err->message.data);
            err = err->next;
        }
    }
    
    return valid;
}
```

#### 6.2.4 Swagger UI интеграция

```c
#include <oas/swagger.h>

// HTTP handler callback
int http_handler(const char *path, const char *method,
                 const char *content_type,
                 const void *data, size_t data_len,
                 void *user_data) {
    HTTPServer *server = (HTTPServer *)user_data;
    return server->serve(path, method, content_type, data, data_len);
}

// Setup Swagger UI
void setup_swagger_ui(OASDocument *doc, HTTPServer *server) {
    oas_swagger_setup(
        doc,
        http_handler,
        "/docs",           // UI path
        "/openapi.json",   // Spec path
        server
    );
}
```

### 6.3 Соглашения об именовании

| Тип | Префикс | Пример |
|-----|---------|--------|
| Публичные типы | `OAS` | `OASDocument`, `OASSchema` |
| Публичные функции | `oas_` | `oas_document_parse` |
| Внутренние типы | `oas_` | `oas_arena_chunk` |
| Внутренние функции | `oas_i_` | `oas_i_resolve_ref` |
| Макросы | `OAS_` | `OAS_SV_LITERAL` |
| Константы | `OAS_` | `OAS_MAX_PATH_DEPTH` |

**Структура имён функций:**
```
oas_<module>_<action>[_<modifier>]

Examples:
- oas_document_parse_json()
- oas_schema_get_property()
- oas_arena_alloc_aligned()
- oas_validate_request_body()
```

---

## 7. УПРАВЛЕНИЕ ПАМЯТЬЮ

### 7.1 Arena Allocator

**Принцип работы:**
- Линейное выделение памяти в чанках фиксированного размера
- Массовое освобождение путём сброса или уничтожения арены
- Отсутствие фрагментации и индивидуального освобождения

**Преимущества для liboas:**
- Все объекты документа имеют одинаковый жизненный цикл
- Нет необходимости в индивидуальном освобождении объектов
- Высокая производительность аллокации
- Предсказуемое потребление памяти

```c
// Создание арены
OASArena *arena = oas_arena_create(64 * 1024);  // 64KB initial chunk

// Выделение памяти
OASDocument *doc = OAS_ARENA_ALLOC(arena, OASDocument);
OASSchema *schemas = OAS_ARENA_ALLOC_N(arena, OASSchema, 100);
char *copy = oas_arena_strdup(arena, "hello");

// Сброс для повторного использования
oas_arena_reset(arena);

// Полное освобождение
oas_arena_destroy(arena);
```

### 7.2 String Views (Zero-Copy)

**Принцип работы:**
- Строки хранятся как указатель + длина
- Нет копирования данных при создании view
- Данные остаются в исходном буфере (JSON/YAML)

```c
// Создание string view из литерала
OASStringView sv1 = OAS_SV_LITERAL("hello");

// Создание из C-строки
OASStringView sv2 = OAS_SV_FROM_CSTR("world");

// Создание из указателя и длины
OASStringView sv3 = OAS_SV_FROM_PTR_LEN(ptr, len);

// Операции без копирования
if (oas_sv_eq(sv1, sv2)) { ... }
if (oas_sv_starts_with(sv3, OAS_SV_LITERAL("prefix"))) { ... }

// Конвертация в C-строку (требует arena)
char *cstr = oas_sv_to_cstr(sv1, arena);
```

### 7.3 Shared Objects Registry

**Назначение:**
- Реестр для разрешения `$ref` ссылок
- O(1) lookup по имени и контейнеру
- Потокобезопасен после инициализации

```c
// Type tag for runtime type identification
typedef enum OASObjectType : uint8_t {
    OAS_OBJ_SCHEMA = 0,
    OAS_OBJ_RESPONSE,
    OAS_OBJ_PARAMETER,
    OAS_OBJ_EXAMPLE,
    OAS_OBJ_REQUEST_BODY,
    OAS_OBJ_HEADER,
    OAS_OBJ_SECURITY_SCHEME,
    OAS_OBJ_LINK,
    OAS_OBJ_CALLBACK,
    OAS_OBJ_PATH_ITEM,
    OAS_OBJ_COUNT
} OASObjectType;

// Generic reference to any OAS object
typedef struct OASObjectRef {
    void *ptr;
    OASObjectType type;
} OASObjectRef;

// Registry
typedef struct OASObjectRegistry {
    OASHashTable *buckets;
    OASArena *arena;
    size_t entry_count;
} OASObjectRegistry;

// Usage
OASObjectRegistry *reg = oas_registry_create(arena);

// Register schema
oas_registry_register(reg, 
                      OAS_SV_LITERAL("schemas"),
                      OAS_SV_LITERAL("Pet"),
                      (OASObjectRef){pet_schema, OAS_OBJ_SCHEMA});

// Lookup
OASSchema *pet = OAS_REGISTRY_LOOKUP_SCHEMA(reg, OAS_SV_LITERAL("Pet"));
```

---

## 8. ВАЛИДАЦИЯ

### 8.1 JSON Schema валидация

**Поддерживаемые ключевые слова JSON Schema 2020-12:**

| Категория | Ключевые слова |
|-----------|----------------|
| **Type** | type, enum, const |
| **Numeric** | multipleOf, minimum, maximum, exclusiveMinimum, exclusiveMaximum |
| **String** | minLength, maxLength, pattern, contentEncoding, contentMediaType, contentSchema, format |
| **Array** | minItems, maxItems, uniqueItems, items, prefixItems, contains, minContains, maxContains |
| **Object** | minProperties, maxProperties, required, properties, patternProperties, additionalProperties, propertyNames |
| **Composition** | allOf, anyOf, oneOf, not |
| **Conditional** | if, then, else, dependentSchemas, dependentRequired |
| **Dynamic** | $dynamicRef, $dynamicAnchor |
| **Unevaluated** | unevaluatedProperties, unevaluatedItems |

### 8.2 Валидация запросов/ответов

**Процесс валидации запроса:**

```
┌─────────────────┐
│  HTTP Request   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐     ┌─────────────────┐
│ Validate Method │────►│ Invalid Method  │
└────────┬────────┘     └─────────────────┘
         │ Valid
         ▼
┌─────────────────┐     ┌─────────────────┐
│  Match Path     │────►│  Invalid Path   │
└────────┬────────┘     └─────────────────┘
         │ Found
         ▼
┌─────────────────┐     ┌─────────────────┐
│ Validate Path   │────►│ Invalid Path    │
│   Parameters    │     │   Parameters    │
└────────┬────────┘     └─────────────────┘
         │ Valid
         ▼
┌─────────────────┐     ┌─────────────────┐
│ Validate Query  │────►│ Invalid Query   │
│   Parameters    │     │   Parameters    │
└────────┬────────┘     └─────────────────┘
         │ Valid
         ▼
┌─────────────────┐     ┌─────────────────┐
│ Validate Header │────►│ Invalid Header  │
│   Parameters    │     │   Parameters    │
└────────┬────────┘     └─────────────────┘
         │ Valid
         ▼
┌─────────────────┐     ┌─────────────────┐
│  Validate Body  │────►│  Invalid Body   │
│  (JSON Schema)  │     │                 │
└────────┬────────┘     └─────────────────┘
         │ Valid
         ▼
┌─────────────────┐
│ Request Valid   │
└─────────────────┘
```

### 8.3 Parameter Styles

**Поддерживаемые стили параметров:**

| Style | Location | Пример | Описание |
|-------|----------|--------|----------|
| `matrix` | path | `;color=blue` | Semicolon-prefixed |
| `label` | path | `.blue` | Dot-prefixed |
| `simple` | path, header | `blue,black` | Comma-separated |
| `form` | query, cookie | `color=blue` | Ampersand-separated |
| `spaceDelimited` | query | `blue black` | Space-separated |
| `pipeDelimited` | query | `blue|black` | Pipe-separated |
| `deepObject` | query | `color[primary]=blue` | Nested object |
| `cookie` | cookie | `color=blue` | Semicolon-separated |

```c
// Parameter style enum
typedef enum OASParamStyle : uint8_t {
    OAS_STYLE_MATRIX = 0,
    OAS_STYLE_LABEL,
    OAS_STYLE_SIMPLE,
    OAS_STYLE_FORM,
    OAS_STYLE_SPACE_DELIMITED,
    OAS_STYLE_PIPE_DELIMITED,
    OAS_STYLE_DEEP_OBJECT,
    OAS_STYLE_COOKIE
} OASParamStyle;

// Serialization
OASStringView oas_serialize_parameter(
    OASParameter *param,
    const void *value,
    OASArena *arena
);

// Deserialization
bool oas_deserialize_parameter(
    OASParameter *param,
    OASStringView input,
    void **value,
    OASValidationResult *result
);
```

---

## 9. ЗАВИСИМОСТИ

### 9.1 Обязательные зависимости

| Библиотека | Назначение | Версия | Vendored | Размер |
|------------|------------|--------|----------|--------|
| **yyjson** | JSON парсинг | 0.10+ | ✅ Yes | ~100KB |

### 9.2 Опциональные зависимости

| Библиотека | Назначение | Версия | Vendored | Размер |
|------------|------------|--------|----------|--------|
| **libyaml** | YAML парсинг | 0.2.5+ | ✅ Yes | ~200KB |
| **pcre2** | Pattern matching | 10.42+ | ❌ No | ~500KB |

### 9.3 Vendored библиотеки

```
vendor/
├── yyjson/
│   ├── yyjson.h      # Single-header version
│   └── yyjson.c
├── libyaml/          # Optional, for YAML support
│   ├── include/
│   └── src/
└── README.md
```

### 9.4 Интеграция с yyjson

```c
// yyjson allocator that uses our arena
static inline void *yyjson_arena_malloc(void *ctx, size_t size) {
    return oas_arena_alloc((OASArena *)ctx, size);
}

static inline void *yyjson_arena_realloc(void *ctx, void *ptr, 
                                          size_t old_size, size_t size) {
    void *new_ptr = oas_arena_alloc((OASArena *)ctx, size);
    if (ptr && old_size > 0) {
        memcpy(new_ptr, ptr, old_size < size ? old_size : size);
    }
    return new_ptr;
}

static inline void yyjson_arena_free(void *ctx, void *ptr) {
    (void)ctx;
    (void)ptr;
    // No-op for arena
}

// Create yyjson alc using our arena
static inline yyjson_alc oas_yyjson_alc(OASArena *arena) {
    return (yyjson_alc){
        .malloc = yyjson_arena_malloc,
        .realloc = yyjson_arena_realloc,
        .free = yyjson_arena_free,
        .ctx = arena
    };
}
```

---

## 10. ПЛАН РАЗРАБОТКИ

### 10.1 Фазы и этапы

#### Фаза 1: Фундамент (4 недели)

| Этап | Длительность | Результат |
|------|--------------|-----------|
| 1.1 Core модули | 1 неделя | Arena, String views, Error handling |
| 1.2 Utils модули | 1 неделя | Hash tables, Vectors, Buffers |
| 1.3 JSON парсер | 1 неделя | yyjson интеграция |
| 1.4 Базовые тесты | 1 неделя | Unit tests для core |

#### Фаза 2: Модель данных (4 недели)

| Этап | Длительность | Результат |
|------|--------------|-----------|
| 2.1 Базовые объекты | 1 неделя | Document, Info, Server, Contact, License |
| 2.2 Path объекты | 1 неделя | Paths, PathItem, Operation |
| 2.3 Parameter объекты | 1 неделя | Parameter, RequestBody, MediaType, Encoding |
| 2.4 Response объекты | 1 неделя | Responses, Response, Header, Link |

#### Фаза 3: Schema и References (4 недели)

| Этап | Длительность | Результат |
|------|--------------|-----------|
| 3.1 Schema объект | 2 недели | Полная поддержка JSON Schema 2020-12 |
| 3.2 Reference resolver | 1 неделя | $ref, $dynamicRef resolution |
| 3.3 Components | 1 неделя | Components, Object registry |

#### Фаза 4: Валидация (4 недели)

| Этап | Длительность | Результат |
|------|--------------|-----------|
| 4.1 JSON Schema validation | 2 недели | Все ключевые слова |
| 4.2 Request validation | 1 неделя | Parameters, Body |
| 4.3 Response validation | 1 неделя | Status, Headers, Body |

#### Фаза 5: Интеграция (2 недели)

| Этап | Длительность | Результат |
|------|--------------|-----------|
| 5.1 Swagger UI | 1 неделя | UI serving, Spec serving |
| 5.2 YAML support | 0.5 недели | libyaml интеграция |
| 5.3 Performance tuning | 0.5 недели | Оптимизации |

#### Фаза 6: Тестирование (2 недели)

| Этап | Длительность | Результат |
|------|--------------|-----------|
| 6.1 Unit tests | 1 неделя | >90% coverage |
| 6.2 Integration tests | 0.5 недели | Petstore, WolfGuard |
| 6.3 Performance tests | 0.5 недели | Benchmarks |

### 10.2 Оценка трудоёмкости

| Фаза | Недели | Человеко-часы* |
|------|--------|----------------|
| Фаза 1: Фундамент | 4 | 160 |
| Фаза 2: Модель данных | 4 | 160 |
| Фаза 3: Schema и References | 4 | 160 |
| Фаза 4: Валидация | 4 | 160 |
| Фаза 5: Интеграция | 2 | 80 |
| Фаза 6: Тестирование | 2 | 80 |
| **Итого** | **20** | **800** |

*При 1 разработчике, 40 часов/неделю

### 10.3 Риски

| Риск | Вероятность | Влияние | Митигация |
|------|-------------|---------|-----------|
| Сложность JSON Schema 2020-12 | Средняя | Высокое | Использовать существующие тестовые наборы |
| Производительность валидации | Средняя | Высокое | Предварительная компиляция схем |
| Потокобезопасность | Низкая | Высокое | Thread-local валидаторы |
| Интеграция с WolfGuard | Низкая | Среднее | Раннее тестирование |
| Поддержка YAML | Низкая | Низкое | Опциональная зависимость |

---

## 11. ТЕСТИРОВАНИЕ

### 11.1 Стратегия тестирования

```
┌─────────────────────────────────────────────────────────────┐
│                    Тестовая пирамида                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│                    ┌─────────────┐                          │
│                    │  E2E Tests  │  < 5%                   │
│                    │ (WolfGuard) │                          │
│                    └──────┬──────┘                          │
│                   ┌───────┴───────┐                         │
│              ┌────┴────┐     ┌────┴────┐                    │
│              │Integration│    │Integration│  ~15%            │
│              │ (Petstore)│    │(Validation)│                  │
│              └────┬────┘     └────┬────┘                    │
│         ┌─────────┴─────────┐     │                         │
│    ┌────┴────┐         ┌────┴────┐                          │
│    │  Unit   │         │  Unit   │    ~80%                  │
│    │ (Core)  │         │(Schema) │                          │
│    └─────────┘         └─────────┘                          │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 11.2 Структура тестов

```
tests/
├── unit/
│   ├── test_arena.c
│   ├── test_string.c
│   ├── test_parser.c
│   ├── test_schema.c
│   ├── test_ref.c
│   └── test_validate.c
├── integration/
│   ├── test_petstore.c
│   ├── test_validation.c
│   └── test_swagger.c
├── fixtures/
│   ├── petstore.json
│   ├── petstore.yaml
│   └── complex_spec.json
├── benchmarks/
│   ├── bench_parser.c
│   └── bench_validate.c
└── test_main.c
```

### 11.3 Покрытие

| Модуль | Целевое покрытие |
|--------|------------------|
| core/ | 95% |
| parser/ | 90% |
| model/ | 85% |
| ref/ | 90% |
| validate/ | 90% |
| swagger/ | 80% |
| **Итого** | **> 90%** |

### 11.4 Пример unit-теста

```c
// tests/unit/test_arena.c
#include <oas/core/arena.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

void test_arena_basic(void) {
    OASArena *arena = oas_arena_create(1024);
    assert(arena != nullptr);
    
    // Allocate some memory
    void *p1 = oas_arena_alloc(arena, 100);
    assert(p1 != nullptr);
    
    void *p2 = oas_arena_alloc(arena, 200);
    assert(p2 != nullptr);
    assert(p2 > p1);  // Linear allocation
    
    // Test aligned allocation
    void *p3 = oas_arena_alloc_aligned(arena, 64, 64);
    assert(p3 != nullptr);
    assert(((uintptr_t)p3 % 64) == 0);
    
    // Test strdup
    char *copy = oas_arena_strdup(arena, "hello world");
    assert(strcmp(copy, "hello world") == 0);
    
    // Reset and reuse
    oas_arena_reset(arena);
    
    // Should be able to allocate again
    void *p4 = oas_arena_alloc(arena, 50);
    assert(p4 != nullptr);
    
    oas_arena_destroy(arena);
    printf("test_arena_basic: PASSED\n");
}

void test_arena_large_allocation(void) {
    OASArena *arena = oas_arena_create(1024);
    
    // Allocate more than chunk size
    void *large = oas_arena_alloc(arena, 10 * 1024);
    assert(large != nullptr);
    
    oas_arena_destroy(arena);
    printf("test_arena_large_allocation: PASSED\n");
}

int main(void) {
    test_arena_basic();
    test_arena_large_allocation();
    printf("All arena tests PASSED\n");
    return 0;
}
```

---

## 12. ПРИЛОЖЕНИЯ

### Приложение A: Полная таблица объектов OAS 3.2.0

| № | Объект | Спецификация | C-структура | Основные поля |
|---|--------|--------------|-------------|---------------|
| 1 | OpenAPI | 4.1 | `OASDocument` | openapi, info, servers, paths, components, security, tags, externalDocs |
| 2 | Info | 4.2 | `OASInfo` | title, summary, description, termsOfService, contact, license, version |
| 3 | Contact | 4.3 | `OASContact` | name, url, email |
| 4 | License | 4.4 | `OASLicense` | name, identifier, url |
| 5 | Server | 4.5 | `OASServer` | url, description, variables |
| 6 | ServerVariable | 4.6 | `OASServerVariable` | enum, default, description |
| 7 | Components | 4.7 | `OASComponents` | schemas, responses, parameters, examples, requestBodies, headers, securitySchemes, links, callbacks, pathItems |
| 8 | Paths | 4.8 | `OASPaths` | [path -> PathItem] |
| 9 | PathItem | 4.9 | `OASPathItem` | $ref, summary, description, get, put, post, delete, options, head, patch, trace, query, additionalOperations, servers, parameters |
| 10 | Operation | 4.10 | `OASOperation` | tags, summary, description, externalDocs, operationId, parameters, requestBody, responses, callbacks, deprecated, security, servers |
| 11 | ExternalDocumentation | 4.11 | `OASExternalDocs` | description, url |
| 12 | Parameter | 4.12 | `OASParameter` | name, in, description, required, deprecated, allowEmptyValue, style, explode, allowReserved, schema, example, examples, content |
| 13 | RequestBody | 4.13 | `OASRequestBody` | description, content, required |
| 14 | MediaType | 4.14 | `OASMediaType` | schema, example, examples, encoding, itemSchema, itemEncoding, prefixEncoding |
| 15 | Encoding | 4.15 | `OASEncoding` | contentType, headers, style, explode, allowReserved |
| 16 | Responses | 4.16 | `OASResponses` | default, [statusCode -> Response] |
| 17 | Response | 4.17 | `OASResponse` | description, headers, content, links |
| 18 | Callback | 4.18 | `OASCallback` | [expression -> PathItem] |
| 19 | Example | 4.19 | `OASExample` | summary, description, value, externalValue |
| 20 | Link | 4.20 | `OASLink` | operationRef, operationId, parameters, requestBody, description, server |
| 21 | Header | 4.21 | `OASHeader` | description, required, deprecated, allowEmptyValue, style, explode, allowReserved, schema, example, examples, content |
| 22 | Tag | 4.22 | `OASTag` | name, summary, description, externalDocs, parent, kind |
| 23 | Reference | 4.23 | `OASRef` | $ref, summary, description |
| 24 | Schema | 4.24 | `OASSchema` | Полная поддержка JSON Schema 2020-12 |
| 25 | Discriminator | 4.25 | `OASDiscriminator` | propertyName, mapping |
| 26 | XML | 4.26 | `OASXML` | name, namespace, prefix, attribute, wrapped |
| 27 | SecurityScheme | 4.27 | `OASSecurityScheme` | type, description, name, in, scheme, bearerFormat, flows, openIdConnectUrl, oauth2MetadataUrl, deprecated |
| 28 | OAuthFlows | 4.28 | `OASOAuthFlows` | implicit, password, clientCredentials, authorizationCode, deviceAuthorization |
| 29 | OAuthFlow | 4.29 | `OASOAuthFlow` | authorizationUrl, tokenUrl, refreshUrl, scopes, deviceAuthorizationUrl |
| 30 | SecurityRequirement | 4.30 | `OASSecurityRequirement` | [name -> scopes[]] |

### Приложение B: Примеры кода

#### B.1 Создание документа программно

```c
#include <oas/oas.h>

OASDocument *create_sample_api(OASArena *arena) {
    OASDocument *doc = OAS_ARENA_ALLOC(arena, OASDocument);
    *doc = (OASDocument){0};
    
    // OpenAPI version
    doc->openapi = OAS_SV_LITERAL("3.2.0");
    
    // Info
    doc->info = OAS_ARENA_ALLOC(arena, OASInfo);
    *doc->info = (OASInfo){
        .title = OAS_SV_LITERAL("Sample API"),
        .summary = OAS_SV_LITERAL("A sample API for testing"),
        .version = OAS_SV_LITERAL("1.0.0")
    };
    
    // Server
    OASServer *server = OAS_ARENA_ALLOC(arena, OASServer);
    *server = (OASServer){
        .url = OAS_SV_LITERAL("https://api.example.com/v1"),
        .description = OAS_SV_LITERAL("Production server")
    };
    oas_vector_push(&doc->servers, server);
    
    // Schema for Pet
    OASSchema *petSchema = OAS_ARENA_ALLOC(arena, OASSchema);
    *petSchema = (OASSchema){
        .type = OAS_SCHEMA_OBJECT,
        .title = OAS_SV_LITERAL("Pet")
    };
    
    // Register in components
    doc->components = OAS_ARENA_ALLOC(arena, OASComponents);
    doc->components->schemas = oas_hashtable_create(arena);
    oas_hashtable_put(doc->components->schemas, 
                      OAS_SV_LITERAL("Pet"), 
                      petSchema);
    
    return doc;
}
```

#### B.2 Валидация с детальными ошибками

```c
void validate_with_details(OASDocument *doc, const char *json) {
    OASValidationContext ctx = {
        .strict_mode = true,
        .max_errors = 100,
        .validate_formats = true
    };
    
    OASValidationResult result = {0};
    
    // Get schema from components
    OASSchema *schema = oas_registry_lookup_schema(
        doc->registry, 
        OAS_SV_LITERAL("Pet")
    );
    
    if (!schema) {
        fprintf(stderr, "Schema 'Pet' not found\n");
        return;
    }
    
    bool valid = oas_validate_json(schema, json, strlen(json), &ctx, &result);
    
    if (!valid) {
        printf("Validation failed with %zu errors:\n", result.error_count);
        
        OASError *err = result.errors;
        size_t i = 1;
        while (err) {
            printf("  %zu. [%s] %.*s\n",
                   i++,
                   oas_error_category_to_string(err->category),
                   (int)err->message.len, 
                   err->message.data);
            
            if (err->line > 0) {
                printf("     at line %zu, column %zu\n",
                       err->line, err->column);
            }
            
            err = err->next;
        }
    } else {
        printf("Validation passed!\n");
    }
}
```

#### B.3 CMake интеграция

```cmake
# CMakeLists.txt для проекта, использующего liboas

cmake_minimum_required(VERSION 3.20)
project(MyProject VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find liboas
find_package(liboas REQUIRED)

# Your executable
add_executable(my_app main.c)

# Link with liboas
target_link_libraries(my_app PRIVATE liboas::oas)

# Optional: Enable YAML support
target_compile_definitions(my_app PRIVATE OAS_HAS_YAML=1)
```

### Приложение C: Сравнение с существующими библиотеками

| Характеристика | liboas | cpp-oasvalidator | libopenapi |
|----------------|--------|------------------|------------|
| **Язык** | C23 | C++11 | Go |
| **OpenAPI 3.2** | ✅ | ❌ | ✅ |
| **JSON Schema 2020-12** | ✅ | ❌ | ✅ |
| **Zero-copy парсинг** | ✅ | ⚠️ | ❌ |
| **Arena allocator** | ✅ | ❌ | ❌ |
| **Потокобезопасность** | ✅ | ✅ | N/A |
| **Swagger UI** | ✅ | ❌ | ✅* |
| **YAML support** | ✅ (opt) | ❌ | ✅ |
| **Размер библиотеки** | ~200KB | ~500KB | N/A |
| **Зависимости** | yyjson | RapidJSON | yaml.v3 |

*Через отдельный пакет

---

## ИСТОРИЯ ИЗМЕНЕНИЙ

| Версия | Дата | Автор | Изменения |
|--------|------|-------|-----------|
| 1.0 | 2025 | — | Первоначальная версия |

---

## СОГЛАСОВАНИЕ

| Роль | ФИО | Подпись | Дата |
|------|-----|---------|------|
| Технический руководитель | | | |
| Архитектор | | | |
| Руководитель проекта WolfGuard | | | |

---

*Документ подготовлен в соответствии с требованиями к техническому заданию для проекта liboas — библиотеки OpenAPI 3.2.0 для C23.*
