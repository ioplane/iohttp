# Методология тестирования производительности iohttp

## Сравнение с топовыми HTTP серверами на разных языках

**Версия:** 1.1  
**Дата:** 2026-03-08  
**Статус:** Draft (актуализировано с последними LTS версиями)

---

## Содержание

1. [Цели и задачи](#1-цели-и-задачи)
2. [Тестируемые серверы](#2-тестируемые-серверы)
3. [Методология тестирования](#3-методология-тестирования)
4. [Примеры кода](#4-примеры-кода)
5. [Инфраструктура](#5-инфраструктура)
6. [План тестирования](#6-план-тестирования)
7. [Метрики и анализ](#7-метрики-и-анализ)

---

## 1. Цели и задачи

### 1.1. Цели

- Определить производительность iohttp относительно лучших HTTP серверов на разных языках
- Выявить сильные и слабые стороны iohttp
- Получить объективные данные для позиционирования iohttp

### 1.2. Задачи

1. Протестировать iohttp против 8+ серверов на разных языках
2. Использовать одинаковые тестовые сценарии
3. Обеспечить воспроизводимость результатов
4. Сравнить по ключевым метрикам: RPS, latency, memory, CPU

---

## 2. Тестируемые серверы

### 2.1. Список серверов

| Язык | Сервер | Версия | Тип | Ссылка | Примечания |
|------|--------|--------|-----|--------|------------|
| **C** | iohttp | 1.0 | Custom | [GitHub](https://github.com/yourusername/iohttp) | io_uring + wolfSSL, C23 |
| **C** | nginx | 1.28.1 | Reverse Proxy | [nginx.org](https://nginx.org/) | Event-driven, C |
| **C** | H2O | 2.2.6 | Library/Server | [GitHub](https://github.com/h2o/h2o) | HTTP/1/2/3, picotls, C11 |
| **C** | Lwan | 0.5 | Library | [GitHub](https://github.com/lpereira/lwan) | Coroutines, C11 |
| **C++** | Drogon | 1.9.10 | Framework | [GitHub](https://github.com/drogonframework/drogon) | Async, ORM, C++20 |
| **C++** | oat++ | 1.3.0 | Framework | [GitHub](https://github.com/oatpp/oatpp) | Zero-deps, C++17 |
| **C++** | Crow | 1.2.0 | Microframework | [GitHub](https://github.com/crowcpp/crow) | Header-only, C++14/17/20 |
| **Go** | FastHTTP | 1.59.0 | Library | [GitHub](https://github.com/valyala/fasthttp) | Zero-allocation, Go 1.24 |
| **Go** | Gin | 1.10.0 | Framework | [GitHub](https://github.com/gin-gonic/gin) | Radix tree routing, Go 1.24 |
| **Go** | Fiber | 2.52.0 | Framework | [GitHub](https://github.com/gofiber/fiber) | Express-like, Go 1.24 |
| **Go** | Hertz | 0.9.0 | Framework | [GitHub](https://github.com/cloudwego/hertz) | CloudWeGo, high-performance, Go 1.24 |
| **Go** | BunRouter | 1.0.22 | Router | [GitHub](https://github.com/uptrace/bunrouter) | Fast, zero-allocation, Go 1.24 |
| **Go** | net/http | stdlib | Standard | [go.dev](https://pkg.go.dev/net/http) | Go 1.24.2 (latest) |
| **Rust** | Actix-web | 4.10.2 | Framework | [GitHub](https://github.com/actix/actix-web) | Actor model, Rust 1.85+ |
| **Rust** | Axum | 0.8.3 | Framework | [GitHub](https://github.com/tokio-rs/axum) | Tower ecosystem, Rust 1.85+ |
| **Rust** | Hyper | 1.6.0 | Library | [GitHub](https://github.com/hyperium/hyper) | Low-level HTTP, Rust 1.85+ |
| **Java** | Vert.x | 4.5.14 | Framework | [GitHub](https://github.com/eclipse-vertx/vert.x) | Event-driven, Java 21 LTS |
| **Java** | Netty | 4.2.0 | Library | [GitHub](https://github.com/netty/netty) | Async NIO, Java 21 LTS |
| **Java** | Spring Boot | 3.4.3 | Framework | [spring.io](https://spring.io/projects/spring-boot) | WebFlux reactive, Java 21 LTS |
| **C#** | ASP.NET Core (Kestrel) | 10.0 | Framework | [microsoft.com](https://dotnet.microsoft.com/) | Built-in Kestrel server, .NET 10 LTS |
| **Python** | FastAPI | 0.115.12 | Framework | [GitHub](https://github.com/tiangolo/fastapi) | ASGI, Starlette, Python 3.13 |
| **Python** | Uvicorn | 0.34.0 | Server | [GitHub](https://github.com/encode/uvicorn) | ASGI server, Python 3.13 |
| **Node.js** | Express | 4.21.2 | Framework | [GitHub](https://github.com/expressjs/express) | Most popular Node.js framework, Node.js 22 LTS |
| **Node.js** | Fastify | 5.2.1 | Framework | [GitHub](https://github.com/fastify/fastify) | High-performance Node.js, Node.js 22 LTS |
| **Bun** | Bun.serve | 1.2.4 | Runtime | [bun.sh](https://bun.sh/) | Built-in HTTP server, Bun 1.2+ |
| **Zig** | httpz | 0.10.0 | Framework | [GitHub](https://github.com/karlseguin/http.zig) | Zig-native, Zig 0.14 |
| **Zig** | zzz | 0.3.0 | Framework | [GitHub](https://github.com/tardy-org/zzz) | tardy runtime, Zig 0.14 |

### 2.2. Критерии отбора

- Популярность (>1000 stars на GitHub)
- Активная разработка (коммиты в 2024-2025)
- Production-ready
- Поддержка Linux

---

## 3. Методология тестирования

### 3.1. Тестовые сценарии

#### Scenario 1: Hello World (Plaintext)

**Описание:** Базовый endpoint, возвращающий "Hello, World!"

**Цель:** Измерить максимальную пропускную способность фреймворка

**Запрос:**
```
GET /plaintext HTTP/1.1
Host: localhost
```

**Ответ:**
```
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 13

Hello, World!
```

#### Scenario 2: JSON Serialization

**Описание:** Endpoint, возвращающий JSON объект

**Цель:** Измерить производительность сериализации JSON

**Запрос:**
```
GET /json HTTP/1.1
Host: localhost
```

**Ответ:**
```json
{
  "message": "Hello, World!",
  "timestamp": 1705327822
}
```

#### Scenario 3: Database Query (Single)

**Описание:** Запрос к PostgreSQL, возврат одной записи

**Цель:** Измерить производительность с реальной нагрузкой

**Запрос:**
```
GET /db HTTP/1.1
Host: localhost
```

**Ответ:**
```json
{
  "id": 1,
  "name": "Test User",
  "email": "user@example.com"
}
```

#### Scenario 4: Database Query (Multiple)

**Описание:** 20 параллельных запросов к БД

**Цель:** Измерить производительность connection pool

**Запрос:**
```
GET /queries?n=20 HTTP/1.1
Host: localhost
```

#### Scenario 5: Static File Serving

**Описание:** Отдача статического файла (1KB, 10KB, 100KB)

**Цель:** Измерить производительность I/O

**Запрос:**
```
GET /static/test_1k.txt HTTP/1.1
Host: localhost
```

### 3.2. Load Testing Tools

| Tool | Назначение | Параметры |
|------|------------|-----------|
| **wrk** | HTTP benchmarking | `-t12 -c400 -d30s` |
| **wrk2** | Constant throughput | `-t12 -c400 -d30s -R100000` |
| **Bombardier** | High-load testing | `-c 1000 -n 1000000` |
| **Vegeta** | Rate-controlled | `rate=50000/s duration=30s` |
| **hey** | Simple load testing | `-n 1000000 -c 1000` |
| **ApacheBench (ab)** | Legacy testing | `-n 100000 -c 1000` |
| **JMeter** | GUI/CLI load testing | `-t test.jmx -n -l results.jtl` |

**Рекомендуемая конфигурация:**
```bash
# wrk - основной инструмент
wrk -t12 -c400 -d30s --latency http://localhost:8080/plaintext

# wrk2 - для latency-под нагрузкой
wrk2 -t12 -c400 -d30s -R100000 --latency http://localhost:8080/plaintext

# Bombardier - для максимальной нагрузки
bombardier -c 1000 -n 1000000 -l http://localhost:8080/plaintext

# JMeter - для сложных сценариев (GUI для разработки, CLI для запуска)
jmeter -t test_plan.jmx -n -l results.jtl -e -o report/

# JMeter с плагинами для графиков в реальном времени
jmeter -t test_plan.jmx -n -l results.jtl -Djmeter.save.saveservice.output_format=csv
```

**Apache JMeter** — мощный инструмент для нагрузочного тестирования с GUI.

**Особенности:**
- GUI для разработки тест-планов
- Поддержка HTTP/2, WebSocket, JDBC, и др.
- Распределенное тестирование (master-slave)
- Богатые отчеты и визуализации
- Плагины для расширения функциональности

**Когда использовать:**
- Сложные сценарии с параметризацией
- Тестирование не-HTTP протоколов
- Интеграция с CI/CD (Jenkins, GitLab CI)
- Необходимость GUI для отладки

### 3.3. Конфигурация тестов

| Параметр | Значение | Описание |
|----------|----------|----------|
| **Duration** | 30 секунд | Длительность теста |
| **Warmup** | 10 секунд | Разогрев перед измерением |
| **Connections** | 100, 400, 1000 | Количество соединений |
| **Threads** | 12 | Количество потоков wrk |
| **Pipelining** | 1, 16 | HTTP pipelining depth |

### 3.4. Системные настройки

```bash
# Увеличение лимитов
ulimit -n 65535
sysctl -w net.ipv4.tcp_tw_reuse=1
sysctl -w net.ipv4.ip_local_port_range="1024 65535"
sysctl -w net.core.somaxconn=65535

# Отключение swap
swapoff -a

# CPU governor
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > $cpu
done
```

---

## 4. Примеры кода

### 4.1. C - iohttp

```c
// iohttp_hello.c
#include <iohttp.h>
#include <stdio.h>

void plaintext_handler(ioh_request_t *req, ioh_response_t *resp) {
    ioh_response_set_status(resp, 200);
    ioh_response_set_header(resp, "Content-Type", "text/plain");
    ioh_response_set_body(resp, "Hello, World!", 13);
}

void json_handler(ioh_request_t *req, ioh_response_t *resp) {
    ioh_response_set_status(resp, 200);
    ioh_response_set_header(resp, "Content-Type", "application/json");
    ioh_response_set_body(resp, "{\"message\":\"Hello, World!\"}", 30);
}

int main() {
    ioh_server_t *server = ioh_server_create(&(ioh_server_config_t){
        .listen_addr = "0.0.0.0",
        .listen_port = 8080,
        .max_connections = 10000,
        .queue_depth = 4096,
    });
    
    ioh_route_get(server, "/plaintext", plaintext_handler);
    ioh_route_get(server, "/json", json_handler);
    
    printf("iohttp server listening on :8080\n");
    ioh_server_run(server);
    
    return 0;
}
```

**Сборка (C23, GCC 13+):**
```bash
gcc -O3 -march=native -std=c23 -DNDEBUG -o iohttp_hello iohttp_hello.c \
    -liohttp -lwolfssl -lpthread
```

### 4.2. C - nginx

```nginx
# nginx.conf
worker_processes auto;
worker_cpu_affinity auto;

events {
    worker_connections 65535;
    use epoll;
    multi_accept on;
}

http {
    access_log off;
    server_tokens off;
    
    server {
        listen 8080 reuseport;
        
        location /plaintext {
            return 200 "Hello, World!";
            add_header Content-Type text/plain;
        }
        
        location /json {
            return 200 '{"message":"Hello, World!"}';
            add_header Content-Type application/json;
        }
    }
}
```

**Запуск:**
```bash
nginx -c nginx.conf -p /tmp/nginx
```

### 4.3. C - H2O

```c
// h2o_hello.c
#include <h2o.h>
#include <stdio.h>

static h2o_globalconf_t config;
static h2o_context_t ctx;
static h2o_accept_ctx_t accept_ctx;

static int plaintext_handler(h2o_handler_t *self, h2o_req_t *req) {
    static h2o_generator_t generator = {NULL, NULL};
    
    if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")))
        return -1;
    
    req->res.status = 200;
    req->res.reason = "OK";
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, 
                   NULL, H2O_STRLIT("text/plain"));
    h2o_start_response(req, &generator);
    h2o_send(req, &(h2o_iovec_t){H2O_STRLIT("Hello, World!")}, 1, 1);
    
    return 0;
}

int main() {
    h2o_config_init(&config);
    h2o_hostconf_t *host = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    h2o_pathconf_t *path = h2o_config_register_path(host, "/plaintext", 0);
    h2o_handler_t *handler = h2o_create_handler(path, sizeof(*handler));
    handler->on_req = plaintext_handler;
    
    h2o_context_init(&ctx, h2o_evloop_create(), &config);
    
    // ... setup listeners ...
    
    while (h2o_evloop_run(ctx.loop, INT32_MAX) == 0);
    
    return 0;
}
```

### 4.4. C++ - Drogon

```cpp
// drogon_hello.cc
#include <drogon/drogon.h>

using namespace drogon;

int main() {
    app().registerHandler("/plaintext", 
        [](const HttpRequestPtr& req, 
           std::function<void(const HttpResponsePtr&)>&& callback) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setBody("Hello, World!");
            resp->setContentTypeCode(CT_TEXT_PLAIN);
            callback(resp);
        });
    
    app().registerHandler("/json",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback) {
            Json::Value json;
            json["message"] = "Hello, World!";
            auto resp = HttpResponse::newHttpJsonResponse(json);
            callback(resp);
        });
    
    app().addListener("0.0.0.0", 8080);
    app().setThreadNum(std::thread::hardware_concurrency());
    app().run();
    
    return 0;
}
```

**Сборка (C++20, GCC 13+):**
```bash
g++ -O3 -march=native -std=c++20 -DNDEBUG -o drogon_hello drogon_hello.cc \
    -ldrogon -ljsoncpp -lpthread -lssl -lcrypto
```

### 4.5. C++ - oat++

```cpp
// oatpp_hello.cpp
#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/web/server/HttpRouter.hpp"
#include "oatpp/json/ObjectMapper.hpp"

class Handler : public oatpp::web::server::HttpRequestHandler {
public:
    std::shared_ptr<OutgoingResponse> handle(
        const std::shared_ptr<IncomingRequest>& request) override {
        return ResponseFactory::createResponse(Status::CODE_200, "Hello, World!");
    }
};

class JsonHandler : public oatpp::web::server::HttpRequestHandler {
private:
    std::shared_ptr<oatpp::json::ObjectMapper> m_objectMapper;
public:
    JsonHandler() : m_objectMapper(oatpp::json::ObjectMapper::createShared()) {}
    
    std::shared_ptr<OutgoingResponse> handle(
        const std::shared_ptr<IncomingRequest>& request) override {
        auto json = oatpp::Fields<oatpp::String>({
            {"message", oatpp::String("Hello, World!")}
        });
        return ResponseFactory::createResponse(Status::CODE_200, 
            m_objectMapper->writeToString(json));
    }
};

void run() {
    auto router = oatpp::web::server::HttpRouter::createShared();
    router->route("GET", "/plaintext", std::make_shared<Handler>());
    router->route("GET", "/json", std::make_shared<JsonHandler>());
    
    auto connectionHandler = oatpp::web::server::HttpConnectionHandler::createShared(router);
    auto connectionProvider = oatpp::network::tcp::server::ConnectionProvider::createShared(
        {"localhost", 8080, oatpp::network::Address::IP_4});
    
    oatpp::network::Server server(connectionProvider, connectionHandler);
    server.run();
}

int main() {
    oatpp::base::Environment::init();
    run();
    oatpp::base::Environment::destroy();
    return 0;
}
```

### 4.6. Go - FastHTTP

```go
// fasthttp_hello.go
package main

import (
    "encoding/json"
    "fmt"
    "github.com/valyala/fasthttp"
)

func plaintextHandler(ctx *fasthttp.RequestCtx) {
    ctx.SetContentType("text/plain")
    ctx.WriteString("Hello, World!")
}

func jsonHandler(ctx *fasthttp.RequestCtx) {
    ctx.SetContentType("application/json")
    response := map[string]string{"message": "Hello, World!"}
    json.NewEncoder(ctx).Encode(response)
}

func main() {
    server := &fasthttp.Server{
        Handler: func(ctx *fasthttp.RequestCtx) {
            switch string(ctx.Path()) {
            case "/plaintext":
                plaintextHandler(ctx)
            case "/json":
                jsonHandler(ctx)
            default:
                ctx.Error("Not Found", fasthttp.StatusNotFound)
            }
        },
        Concurrency: fasthttp.DefaultConcurrency,
    }
    
    fmt.Println("FastHTTP server listening on :8080")
    server.ListenAndServe(":8080")
}
```

**Сборка (Go 1.24.2):**
```bash
go mod init fasthttp_hello
go get github.com/valyala/fasthttp@latest
go build -ldflags="-s -w" -o fasthttp_hello fasthttp_hello.go
```

### 4.7. Go - Gin

```go
// gin_hello.go
package main

import (
    "net/http"
    "github.com/gin-gonic/gin"
)

func main() {
    gin.SetMode(gin.ReleaseMode)
    r := gin.New()
    
    r.GET("/plaintext", func(c *gin.Context) {
        c.String(http.StatusOK, "Hello, World!")
    })
    
    r.GET("/json", func(c *gin.Context) {
        c.JSON(http.StatusOK, gin.H{
            "message": "Hello, World!",
        })
    })
    
    r.Run(":8080")
}
```

**Сборка (Go 1.24.2):**
```bash
go mod init gin_hello
go get github.com/gin-gonic/gin@latest
go build -ldflags="-s -w" -o gin_hello gin_hello.go
```

### 4.8. Go - Fiber

```go
// fiber_hello.go
package main

import (
    "github.com/gofiber/fiber/v2"
)

func main() {
    app := fiber.New(fiber.Config{
        Prefork: true,
    })
    
    app.Get("/plaintext", func(c *fiber.Ctx) error {
        return c.SendString("Hello, World!")
    })
    
    app.Get("/json", func(c *fiber.Ctx) error {
        return c.JSON(fiber.Map{
            "message": "Hello, World!",
        })
    })
    
    app.Listen(":8080")
}
```

**Сборка (Go 1.24.2):**
```bash
go mod init fiber_hello
go get github.com/gofiber/fiber/v2@latest
go build -ldflags="-s -w" -o fiber_hello fiber_hello.go
```

### 4.9. Rust - Actix-web

```rust
// actix_hello.rs
use actix_web::{get, App, HttpResponse, HttpServer, Responder};

#[get("/plaintext")]
async fn plaintext() -> impl Responder {
    HttpResponse::Ok()
        .content_type("text/plain")
        .body("Hello, World!")
}

#[get("/json")]
async fn json() -> impl Responder {
    HttpResponse::Ok().json(serde_json::json!({
        "message": "Hello, World!"
    }))
}

#[actix_web::main]
async fn main() -> std::io::Result<()> {
    HttpServer::new(|| {
        App::new()
            .service(plaintext)
            .service(json)
    })
    .bind("0.0.0.0:8080")?
    .workers(num_cpus::get())
    .run()
    .await
}
```

**Сборка (Rust 1.85+):**
```toml
# Cargo.toml
[package]
name = "actix_hello"
version = "0.1.0"
edition = "2024"

[dependencies]
actix-web = "4.10"
serde_json = "1.0"
num_cpus = "1.16"
```

```bash
RUSTFLAGS="-C target-cpu=native" cargo build --release
```

### 4.10. Rust - Axum

```rust
// axum_hello.rs
use axum::{
    routing::get,
    response::{Html, Json},
    Router,
};
use serde_json::Value;

async fn plaintext() -> Html<&'static str> {
    Html("Hello, World!")
}

async fn json() -> Json<Value> {
    Json(serde_json::json!({
        "message": "Hello, World!"
    }))
}

#[tokio::main]
async fn main() {
    let app = Router::new()
        .route("/plaintext", get(plaintext))
        .route("/json", get(json));
    
    let listener = tokio::net::TcpListener::bind("0.0.0.0:8080").await.unwrap();
    axum::serve(listener, app).await.unwrap();
}
```

**Сборка (Rust 1.85+):**
```toml
# Cargo.toml
[package]
name = "axum_hello"
version = "0.1.0"
edition = "2024"

[dependencies]
axum = "0.8"
tokio = { version = "1", features = ["full"] }
serde_json = "1.0"
```

```bash
RUSTFLAGS="-C target-cpu=native" cargo build --release
```

### 4.11. Java - Vert.x

```java
// VertxHello.java
import io.vertx.core.AbstractVerticle;
import io.vertx.core.Vertx;
import io.vertx.core.http.HttpServer;
import io.vertx.core.json.JsonObject;

public class VertxHello extends AbstractVerticle {
    @Override
    public void start() {
        HttpServer server = vertx.createHttpServer();
        
        server.requestHandler(req -> {
            String path = req.path();
            
            if ("/plaintext".equals(path)) {
                req.response()
                    .putHeader("Content-Type", "text/plain")
                    .end("Hello, World!");
            } else if ("/json".equals(path)) {
                req.response()
                    .putHeader("Content-Type", "application/json")
                    .end(new JsonObject()
                        .put("message", "Hello, World!")
                        .encode());
            } else {
                req.response().setStatusCode(404).end();
            }
        });
        
        server.listen(8080);
        System.out.println("Vert.x server listening on :8080");
    }
    
    public static void main(String[] args) {
        Vertx vertx = Vertx.vertx();
        vertx.deployVerticle(new VertxHello());
    }
}
```

**Сборка (Java 21 LTS):**
```xml
<!-- pom.xml -->
<properties>
    <maven.compiler.source>21</maven.compiler.source>
    <maven.compiler.target>21</maven.compiler.target>
</properties>
<dependencies>
    <dependency>
        <groupId>io.vertx</groupId>
        <artifactId>vertx-core</artifactId>
        <version>4.5.14</version>
    </dependency>
    <dependency>
        <groupId>io.vertx</groupId>
        <artifactId>vertx-web</artifactId>
        <version>4.5.14</version>
    </dependency>
</dependencies>
```

```bash
mvn package -DskipTests
java -XX:+UseZGC -XX:+ZGenerational -jar target/vertx-hello.jar
```

### 4.12. Java - Netty

```java
// NettyHello.java
import io.netty.bootstrap.ServerBootstrap;
import io.netty.buffer.Unpooled;
import io.netty.channel.*;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import io.netty.handler.codec.http.*;

public class NettyHello {
    public static void main(String[] args) throws Exception {
        EventLoopGroup bossGroup = new NioEventLoopGroup(1);
        EventLoopGroup workerGroup = new NioEventLoopGroup();
        
        try {
            ServerBootstrap b = new ServerBootstrap();
            b.group(bossGroup, workerGroup)
                .channel(NioServerSocketChannel.class)
                .childHandler(new ChannelInitializer<SocketChannel>() {
                    @Override
                    protected void initChannel(SocketChannel ch) {
                        ch.pipeline()
                            .addLast(new HttpServerCodec())
                            .addLast(new HttpObjectAggregator(65536))
                            .addLast(new SimpleChannelInboundHandler<FullHttpRequest>() {
                                @Override
                                protected void channelRead0(ChannelHandlerContext ctx, FullHttpRequest req) {
                                    String path = req.uri();
                                    FullHttpResponse response;
                                    
                                    if ("/plaintext".equals(path)) {
                                        response = new DefaultFullHttpResponse(
                                            HttpVersion.HTTP_1_1, HttpResponseStatus.OK,
                                            Unpooled.wrappedBuffer("Hello, World!".getBytes()));
                                        response.headers().set(HttpHeaderNames.CONTENT_TYPE, "text/plain");
                                    } else if ("/json".equals(path)) {
                                        String json = "{\"message\":\"Hello, World!\"}";
                                        response = new DefaultFullHttpResponse(
                                            HttpVersion.HTTP_1_1, HttpResponseStatus.OK,
                                            Unpooled.wrappedBuffer(json.getBytes()));
                                        response.headers().set(HttpHeaderNames.CONTENT_TYPE, "application/json");
                                    } else {
                                        response = new DefaultFullHttpResponse(
                                            HttpVersion.HTTP_1_1, HttpResponseStatus.NOT_FOUND);
                                    }
                                    
                                    response.headers().set(HttpHeaderNames.CONTENT_LENGTH, response.content().readableBytes());
                                    ctx.writeAndFlush(response);
                                }
                            });
                    }
                });
            
            ChannelFuture f = b.bind(8080).sync();
            System.out.println("Netty server listening on :8080");
            f.channel().closeFuture().sync();
        } finally {
            bossGroup.shutdownGracefully();
            workerGroup.shutdownGracefully();
        }
    }
}
```

### 4.13. C# - ASP.NET Core

```csharp
// Program.cs
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using System.Text.Json;

var builder = WebApplication.CreateBuilder(args);
var app = builder.Build();

app.MapGet("/plaintext", () => Results.Text("Hello, World!"));

app.MapGet("/json", () => Results.Json(new { message = "Hello, World!" }));

app.Run("http://0.0.0.0:8080");
```

**Сборка (.NET 10):**
```bash
dotnet new web -o aspnet_hello --framework net10.0
cd aspnet_hello
# Заменить Program.cs
dotnet publish -c Release -r linux-x64 --self-contained -p:PublishAot=true -o publish
./publish/aspnet_hello
```

### 4.14. Python - FastAPI

```python
# fastapi_hello.py
from fastapi import FastAPI
from fastapi.responses import PlainTextResponse, JSONResponse
import uvicorn

app = FastAPI()

@app.get("/plaintext", response_class=PlainTextResponse)
async def plaintext():
    return "Hello, World!"

@app.get("/json")
async def json_endpoint():
    return {"message": "Hello, World!"}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8080, workers=4)
```

**Запуск (Python 3.13):**
```bash
pip install fastapi uvicorn
uvicorn fastapi_hello:app --host 0.0.0.0 --port 8080 --workers 4 --loop uvloop
```

### 4.15. Python - Uvicorn (ASGI)

```python
# uvicorn_hello.py
async def app(scope, receive, send):
    assert scope['type'] == 'http'
    
    path = scope['path']
    
    if path == '/plaintext':
        await send({
            'type': 'http.response.start',
            'status': 200,
            'headers': [[b'content-type', b'text/plain']],
        })
        await send({
            'type': 'http.response.body',
            'body': b'Hello, World!',
        })
    elif path == '/json':
        await send({
            'type': 'http.response.start',
            'status': 200,
            'headers': [[b'content-type', b'application/json']],
        })
        await send({
            'type': 'http.response.body',
            'body': b'{"message":"Hello, World!"}',
        })
    else:
        await send({
            'type': 'http.response.start',
            'status': 404,
        })
        await send({
            'type': 'http.response.body',
            'body': b'Not Found',
        })
```

**Запуск:**
```bash
uvicorn uvicorn_hello:app --host 0.0.0.0 --port 8080 --workers 4
```

### 4.16. Zig - httpz

```zig
// httpz_hello.zig
const std = @import("std");
const httpz = @import("httpz");

const PlaintextHandler = struct {
    pub fn get(_: PlaintextHandler, _: *httpz.Request, res: *httpz.Response) !void {
        res.content_type = .TEXT;
        res.body = "Hello, World!";
    }
};

const JsonHandler = struct {
    pub fn get(_: JsonHandler, _: *httpz.Request, res: *httpz.Response) !void {
        res.content_type = .JSON;
        res.body = "{\"message\":\"Hello, World!\"}";
    }
};

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    const allocator = gpa.allocator();
    
    var server = try httpz.Server().init(allocator, .{
        .port = 8080,
        .address = "0.0.0.0",
    });
    defer server.deinit();
    
    var router = try server.router(.{});
    router.get("/plaintext", PlaintextHandler{});
    router.get("/json", JsonHandler{});
    
    std.log.info("httpz server listening on :8080", .{});
    try server.listen();
}
```

**Сборка (Zig 0.14):**
```bash
zig fetch --save git+https://github.com/karlseguin/httpz
cd httpz_hello
zig build -Doptimize=ReleaseFast -Dcpu=native
./zig-out/bin/httpz_hello
```

### 4.17. Zig - zzz

```zig
// zzz_hello.zig
const std = @import("std");
const zzz = @import("zzz");
const http = zzz.HTTP;

const PlaintextHandler = struct {
    pub fn handle(_: PlaintextHandler, _: http.Request, res: *http.Response) !void {
        try res.setBody("Hello, World!");
        try res.setHeader("Content-Type", "text/plain");
        res.status = .ok;
    }
};

const JsonHandler = struct {
    pub fn handle(_: JsonHandler, _: http.Request, res: *http.Response) !void {
        try res.setBody("{\"message\":\"Hello, World!\"}");
        try res.setHeader("Content-Type", "application/json");
        res.status = .ok;
    }
};

pub fn main() !void {
    const allocator = std.heap.page_allocator;
    
    var router = http.Router.init(allocator);
    defer router.deinit();
    
    try router.route("/plaintext", PlaintextHandler{});
    try router.route("/json", JsonHandler{});
    
    var server = http.Server.init(allocator, &router);
    defer server.deinit();
    
    std.log.info("zzz server listening on :8080", .{});
    try server.listen(.{ .port = 8080 });
}
```

**Сборка (Zig 0.14):**
```bash
zig fetch --save git+https://github.com/tardy-org/zzz#v0.3.0
zig build -Doptimize=ReleaseFast -Dcpu=native
```

### 4.18. Go - Hertz

```go
// hertz_hello.go
package main

import (
    "context"
    "github.com/cloudwego/hertz/pkg/app"
    "github.com/cloudwego/hertz/pkg/app/server"
    "github.com/cloudwego/hertz/pkg/common/utils"
)

func main() {
    h := server.Default(server.WithHostPorts("0.0.0.0:8080"))
    
    h.GET("/plaintext", func(ctx context.Context, c *app.RequestContext) {
        c.String(200, "Hello, World!")
    })
    
    h.GET("/json", func(ctx context.Context, c *app.RequestContext) {
        c.JSON(200, utils.H{
            "message": "Hello, World!",
        })
    })
    
    h.Spin()
}
```

**Сборка (Go 1.24.2):**
```bash
go mod init hertz_hello
go get github.com/cloudwego/hertz@latest
go build -ldflags="-s -w" -o hertz_hello hertz_hello.go
```

### 4.19. Go - BunRouter

```go
// bunrouter_hello.go
package main

import (
    "github.com/uptrace/bunrouter"
    "net/http"
)

func main() {
    router := bunrouter.New()
    
    router.GET("/plaintext", func(w http.ResponseWriter, req bunrouter.Request) error {
        w.Header().Set("Content-Type", "text/plain")
        w.Write([]byte("Hello, World!"))
        return nil
    })
    
    router.GET("/json", func(w http.ResponseWriter, req bunrouter.Request) error {
        w.Header().Set("Content-Type", "application/json")
        w.Write([]byte(`{"message":"Hello, World!"}`))
        return nil
    })
    
    http.ListenAndServe(":8080", router)
}
```

**Сборка (Go 1.24.2):**
```bash
go mod init bunrouter_hello
go get github.com/uptrace/bunrouter@latest
go build -ldflags="-s -w" -o bunrouter_hello bunrouter_hello.go
```

### 4.20. Node.js - Express

```javascript
// express_hello.js
const express = require('express');
const app = express();

app.get('/plaintext', (req, res) => {
    res.set('Content-Type', 'text/plain');
    res.send('Hello, World!');
});

app.get('/json', (req, res) => {
    res.json({ message: 'Hello, World!' });
});

app.listen(8080, () => {
    console.log('Express server listening on :8080');
});
```

**Запуск (Node.js 22 LTS):**
```bash
npm init -y
npm install express
node --max-old-space-size=4096 --optimize-for-size express_hello.js
```

### 4.21. Node.js - Fastify

```javascript
// fastify_hello.js
const fastify = require('fastify')({ logger: false });

fastify.get('/plaintext', async (request, reply) => {
    reply.type('text/plain');
    return 'Hello, World!';
});

fastify.get('/json', async (request, reply) => {
    return { message: 'Hello, World!' };
});

fastify.listen({ port: 8080, host: '0.0.0.0' }, (err) => {
    if (err) {
        console.error(err);
        process.exit(1);
    }
    console.log('Fastify server listening on :8080');
});
```

**Запуск (Node.js 22 LTS):**
```bash
npm init -y
npm install fastify
node --max-old-space-size=4096 --optimize-for-size fastify_hello.js
```

### 4.22. Bun - Bun.serve

```typescript
// bun_hello.ts
Bun.serve({
    port: 8080,
    hostname: "0.0.0.0",
    fetch(req: Request): Response {
        const url = new URL(req.url);
        
        if (url.pathname === '/plaintext') {
            return new Response('Hello, World!', {
                headers: { 'Content-Type': 'text/plain' }
            });
        }
        
        if (url.pathname === '/json') {
            return Response.json({ message: 'Hello, World!' });
        }
        
        return new Response('Not Found', { status: 404 });
    }
});

console.log('Bun server listening on :8080');
```

**Запуск (Bun 1.2+):**
```bash
bun run --smol bun_hello.ts
```

---

## 5. Инфраструктура

### 5.1. Рекомендуемая конфигурация

#### Bare Metal (Рекомендуется для точных результатов)

| Компонент | Спецификация |
|-----------|--------------|
| **CPU** | Intel Xeon Gold 6330 @ 2.00GHz (56 cores) или AMD EPYC 7763 (64 cores) |
| **RAM** | 64GB DDR4-3200 ECC |
| **Network** | Mellanox ConnectX-6 40Gbps или Intel X710 10Gbps |
| **Disk** | NVMe SSD 1TB (для логов) |
| **OS** | Ubuntu 24.04 LTS (Linux 6.8+) |

**Преимущества:**
- Максимальная производительность
- Без overhead виртуализации
- Точные результаты

**Недостатки:**
- Высокая стоимость
- Сложность масштабирования

#### Docker (Для разработки и CI/CD)

```dockerfile
# Dockerfile.benchmark
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    g++ \
    golang \
    rustc \
    cargo \
    openjdk-21-jdk \
    dotnet-sdk-8.0 \
    python3 \
    python3-pip \
    wrk \
    bombardier \
    jmeter \
    && rm -rf /var/lib/apt/lists/*

# Настройка системы
RUN echo 'net.ipv4.tcp_tw_reuse=1' >> /etc/sysctl.conf && \
    echo 'net.core.somaxconn=65535' >> /etc/sysctl.conf

WORKDIR /benchmark
COPY . .

CMD ["/benchmark/run_all.sh"]
```

**Запуск:**
```bash
docker build -t iohttp-benchmark -f Dockerfile.benchmark .
docker run --rm --privileged --network host \
    -v $(pwd)/results:/benchmark/results \
    iohttp-benchmark
```

**Преимущества:**
- Воспроизводимость
- Легкость развертывания
- Изоляция

**Недостатки:**
- Overhead 0-5% (сеть)
- Overhead 5-15% (диск)

#### Virtual Machine (Для тестирования в облаке)

| Провайдер | Тип | Спецификация |
|-----------|-----|--------------|
| **AWS** | c7i.8xlarge | 32 vCPU, 64GB RAM |
| **GCP** | c2-standard-32 | 32 vCPU, 128GB RAM |
| **Azure** | F64s_v2 | 64 vCPU, 128GB RAM |
| **Hetzner** | CCX63 | 64 vCPU, 256GB RAM |

**Преимущества:**
- Масштабируемость
- Гибкость
- Доступность

**Недостатки:**
- Overhead виртуализации 0-10%
- Noisy neighbors

### 5.2. Сравнение инфраструктур

| Метрика | Bare Metal | Docker | VM |
|---------|------------|--------|-----|
| **CPU overhead** | 0% | 0-3% | 0-10% |
| **Memory overhead** | 0% | 0-5% | 0-10% |
| **Network overhead** | 0% | 0-5% | 5-15% |
| **Disk overhead** | 0% | 5-15% | 10-35% |
| **Стоимость** | Высокая | Низкая | Средняя |
| **Воспроизводимость** | Средняя | Высокая | Высокая |
| **Сложность** | Низкая | Средняя | Средняя |

**Рекомендация:**
- Для финальных результатов: **Bare Metal**
- Для разработки и CI: **Docker**
- Для масштабирования: **VM в облаке**

### 5.3. Системные настройки

```bash
#!/bin/bash
# setup_benchmark.sh

# Отключение swap
swapoff -a

# Увеличение лимитов
ulimit -n 1048576
cat >> /etc/security/limits.conf << EOF
* soft nofile 1048576
* hard nofile 1048576
* soft nproc 1048576
* hard nproc 1048576
EOF

# Настройка TCP
cat >> /etc/sysctl.conf << EOF
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 10
net.ipv4.tcp_keepalive_time = 30
net.ipv4.tcp_max_syn_backlog = 65535
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.ip_local_port_range = 1024 65535
net.ipv4.tcp_max_tw_buckets = 2000000
net.ipv4.tcp_fastopen = 3
EOF

sysctl -p

# CPU governor
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > $cpu 2>/dev/null || true
done

# Отключение hyperthreading (опционально)
# echo off > /sys/devices/system/cpu/smt/control

echo "Benchmark environment configured"
```

---

## 6. План тестирования

### 6.1. Этапы

| Этап | Длительность | Описание |
|------|--------------|----------|
| **1. Подготовка** | 1 день | Установка ПО, настройка системы |
| **2. Hello World** | 1 день | Тестирование всех серверов |
| **3. JSON** | 1 день | Тестирование JSON сериализации |
| **4. Database** | 2 дня | Тестирование с PostgreSQL |
| **5. Static Files** | 1 день | Тестирование статики |
| **6. Анализ** | 1 день | Обработка результатов |

### 6.2. Автоматизация

```bash
#!/bin/bash
# run_benchmark.sh

SERVERS=(
    "iohttp:./iohttp_hello"
    "nginx:nginx -c nginx.conf"
    "drogon:./drogon_hello"
    "oatpp:./oatpp_hello"
    "crow:./crow_hello"
    "fasthttp:./fasthttp_hello"
    "gin:./gin_hello"
    "fiber:./fiber_hello"
    "hertz:./hertz_hello"
    "bunrouter:./bunrouter_hello"
    "actix:./actix_hello"
    "axum:./axum_hello"
    "vertx:java -jar vertx-hello.jar"
    "netty:java -jar netty-hello.jar"
    "aspnet:./aspnet_hello"
    "fastapi:python fastapi_hello.py"
    "uvicorn:uvicorn uvicorn_hello:app --workers 4"
    "express:node express_hello.js"
    "fastify:node fastify_hello.js"
    "bun:bun run bun_hello.ts"
    "httpz:./httpz_hello"
    "zzz:./zzz_hello"
)

SCENARIOS=(
    "plaintext:/plaintext"
    "json:/json"
)

for server in "${SERVERS[@]}"; do
    IFS=':' read -r name cmd <<< "$server"
    
    echo "Testing $name..."
    
    # Запуск сервера
    $cmd &
    SERVER_PID=$!
    sleep 2
    
    for scenario in "${SCENARIOS[@]}"; do
        IFS=':' read -r scen_path scen_url <<< "$scenario"
        
        echo "  Scenario: $scen_path"
        
        # Warmup
        wrk -t12 -c100 -d10s "http://localhost:8080$scen_url" > /dev/null 2>&1
        
        # Тестирование
        wrk -t12 -c400 -d30s --latency "http://localhost:8080$scen_url" \
            > "results/${name}_${scen_path}.txt"
        
        # Bombardier для максимальной нагрузки
        bombardier -c 1000 -n 1000000 "http://localhost:8080$scen_url" \
            > "results/${name}_${scen_path}_bombardier.txt"
    done
    
    # Остановка сервера
    kill $SERVER_PID 2>/dev/null
    wait $SERVER_PID 2>/dev/null
    sleep 1
done

echo "Benchmark complete!"
```

---

## 7. Метрики и анализ

### 7.1. Собираемые метрики

| Метрика | Описание | Инструмент |
|---------|----------|------------|
| **RPS** | Requests per second | wrk, bombardier |
| **Latency (avg)** | Средняя задержка | wrk --latency |
| **Latency (p50)** | 50th percentile | wrk --latency |
| **Latency (p99)** | 99th percentile | wrk --latency |
| **Latency (max)** | Максимальная задержка | wrk --latency |
| **Throughput** | MB/s | wrk |
| **CPU usage** | Загрузка CPU | perf, mpstat |
| **Memory usage** | Использование памяти | free, pmap |
| **Context switches** | Переключения контекста | perf |
| **Syscalls** | Системные вызовы | strace, perf |

### 7.2. Визуализация

```python
# visualize_results.py
import matplotlib.pyplot as plt
import pandas as pd
import json
import os

def parse_wrk(filename):
    """Парсинг вывода wrk"""
    with open(filename) as f:
        content = f.read()
    
    # Extract RPS
    rps_line = [l for l in content.split('\n') if 'Requests/sec' in l][0]
    rps = float(rps_line.split(':')[1].strip())
    
    # Extract latency
    latency_line = [l for l in content.split('\n') if 'Latency' in l][0]
    latency = float(latency_line.split()[1].strip())
    
    return {'rps': rps, 'latency': latency}

def create_comparison_chart():
    results = []
    
    for filename in os.listdir('results'):
        if filename.endswith('_plaintext.txt'):
            server_name = filename.replace('_plaintext.txt', '')
            data = parse_wrk(f'results/{filename}')
            results.append({'server': server_name, **data})
    
    df = pd.DataFrame(results)
    df = df.sort_values('rps', ascending=False)
    
    # RPS comparison
    plt.figure(figsize=(12, 6))
    plt.bar(df['server'], df['rps'])
    plt.xticks(rotation=45, ha='right')
    plt.ylabel('Requests per second')
    plt.title('HTTP Server Performance Comparison - Hello World')
    plt.tight_layout()
    plt.savefig('results/rps_comparison.png')
    
    # Latency comparison
    plt.figure(figsize=(12, 6))
    plt.bar(df['server'], df['latency'])
    plt.xticks(rotation=45, ha='right')
    plt.ylabel('Average Latency (ms)')
    plt.title('HTTP Server Latency Comparison - Hello World')
    plt.tight_layout()
    plt.savefig('results/latency_comparison.png')
    
    print("Charts saved to results/")

if __name__ == '__main__':
    create_comparison_chart()
```

### 7.3. Отчет

```markdown
# Benchmark Results

## Test Environment
- Date: 2026-03-08
- Hardware: Intel Xeon Gold 6330 @ 2.00GHz (56 cores), 64GB RAM
- OS: Ubuntu 24.04 LTS (Linux 6.8)
- Network: Mellanox ConnectX-6 40Gbps

## Results Summary

### Hello World (Plaintext)

| Server | Language | RPS | Latency (avg) | Latency (p99) |
|--------|----------|-----|---------------|---------------|
| iohttp | C | 2,500,000 | 0.15ms | 0.45ms |
| nginx | C | 1,800,000 | 0.22ms | 0.65ms |
| fiber | Go | 1,500,000 | 0.94ms | 2.50ms |
| axum | Rust | 1,200,000 | 0.80ms | 2.10ms |
| ... | ... | ... | ... | ... |

### JSON Serialization

| Server | Language | RPS | Latency (avg) |
|--------|----------|-----|---------------|
| iohttp | C | 2,200,000 | 0.18ms |
| ... | ... | ... | ... |

## Conclusions

1. iohttp показывает лучшую производительность благодаря io_uring
2. ...
```

---

## 8. Ссылки и ресурсы

### Топ серверы для сравнения (обновлено 2025)

| Язык | Сервер | Версия | Почему |
|------|--------|--------|--------|
| **C** | [nginx](https://nginx.org/) | 1.28.1 | Золотой стандарт, event-driven |
| **C** | [H2O](https://github.com/h2o/h2o) | 2.2.6 | HTTP/1/2/3, picotls |
| **C** | [Lwan](https://github.com/lpereira/lwan) | 0.5 | Coroutines, высокая производительность |
| **C++** | [Drogon](https://github.com/drogonframework/drogon) | 1.9.10 | Async, ORM, C++20 |
| **C++** | [oat++](https://github.com/oatpp/oatpp) | 1.3.0 | Zero-deps, C++17 |
| **C++** | [Crow](https://github.com/crowcpp/crow) | 1.2.0 | Header-only, C++20 |
| **Go** | [FastHTTP](https://github.com/valyala/fasthttp) | 1.59.0 | Zero-allocation, Go 1.24 |
| **Go** | [Fiber](https://github.com/gofiber/fiber) | 2.52.0 | Express-like, prefork |
| **Go** | [Hertz](https://github.com/cloudwego/hertz) | 0.9.0 | CloudWeGo, high-performance |
| **Go** | [BunRouter](https://github.com/uptrace/bunrouter) | 1.0.22 | Fast, zero-allocation |
| **Rust** | [Axum](https://github.com/tokio-rs/axum) | 0.8.3 | Tower ecosystem, Rust 1.85+ |
| **Rust** | [Actix-web](https://github.com/actix/actix-web) | 4.10.2 | Actor model, Rust 1.85+ |
| **Java** | [Vert.x](https://github.com/eclipse-vertx/vert.x) | 4.5.14 | Event-driven, Java 21 LTS |
| **Java** | [Netty](https://github.com/netty/netty) | 4.2.0 | Async NIO, Java 21 LTS |
| **C#** | [ASP.NET Core](https://dotnet.microsoft.com/) | 10.0 | Kestrel, .NET 10 LTS |
| **Python** | [FastAPI](https://github.com/tiangolo/fastapi) | 0.115.12 | ASGI, Python 3.13 |
| **Python** | [Uvicorn](https://github.com/encode/uvicorn) | 0.34.0 | ASGI server, Python 3.13 |
| **Node.js** | [Fastify](https://github.com/fastify/fastify) | 5.2.1 | Самый быстрый в Node.js, Node.js 22 LTS |
| **Node.js** | [Express](https://github.com/expressjs/express) | 4.21.2 | Самый популярный, Node.js 22 LTS |
| **Bun** | [Bun.serve](https://bun.sh/) | 1.2.4 | Новый быстрый runtime |
| **Zig** | [httpz](https://github.com/karlseguin/http.zig) | 0.10.0 | Zig-native, Zig 0.14 |
| **Zig** | [zzz](https://github.com/tardy-org/zzz) | 0.3.0 | tardy runtime, Zig 0.14 |

---

## 9. Рекомендации

### 9.1. Для iohttp

1. **Тестировать на bare metal** для получения точных результатов
2. **Использовать wrk2** для latency-под нагрузкой (constant throughput)
3. **Использовать JMeter** для сложных сценариев и GUI-разработки тестов
4. **Запускать минимум 3 раза** и брать среднее
5. **Мониторить системные метрики** (CPU, memory, syscalls)
6. **Сравнивать с похожими архитектурами** (nginx, H2O, Lwan)

### 9.2. Для воспроизводимости

1. **Документировать всё**: версии ПО, системные настройки, окружение
2. **Использовать Docker** для CI/CD тестирования
3. **Публиковать сырые данные** вместе с отчетом
4. **Автоматизировать** весь процесс

---

**Согласовано:**  
**Дата:** 2026-03-08  
**Версия:** 1.1 (обновлено с актуальными версиями ПО)
