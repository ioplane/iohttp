# Технический аудит iohttp: 12 фактических ошибок и 14 архитектурных пробелов

Аудит двух документов проекта iohttp выявил **4 прямые фактические ошибки**, **8 неточностей** и **14 значимых архитектурных пробелов**, требующих исправления перед production-развёртыванием. Наиболее критичны: ложное утверждение о не-встраиваемости H2O (libh2o существует и активно используется), неверное указание BoringSSL вместо OpenSSL в стеке H2O, и неразрешённый конфликт лицензий wolfSSL (GPLv2 vs GPLv3). Архитектурный документ описывает сильный технический фундамент, но умалчивает о graceful shutdown, backpressure, защите от request smuggling и QUIC connection migration — все эти компоненты обязательны для production-grade сервера.

---

## Проверка фактов: что верно, что нет

Из 10 ключевых заявлений документа Comparison **только 3 полностью корректны**, 4 содержат неточности, а 3 включают прямые ошибки.

**Полностью подтверждённые факты.** Mongoose действительно не поддерживает HTTP/2 и HTTP/3 — это подтверждено в GitHub Discussion #1427, где мейнтейнер прямо заявил: «Mongoose does not support HTTP 2. Only HTTP 1.0 or 1.1». Последний релиз v7.20 (ноябрь 2025) не добавил поддержку. H2O действительно использует picohttpparser — обе библиотеки разработаны Kazuho Oku и входят в организацию h2o на GitHub. Утверждение о quicly (а не ngtcp2) в H2O также верно — quicly является собственной QUIC-реализацией H2O с MIT-лицензией.

**Ошибки, требующие исправления:**

| Заявление                        | Вердикт    | Исправление                                                                                                                                            |
| -------------------------------- | ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| H2O использует BoringSSL         | **ОШИБКА** | H2O использует **OpenSSL** (libcrypto). picotls поддерживает OpenSSL, minicrypto и MbedTLS. BoringSSL не поддерживается из-за отсутствия OCSP Stapling |
| H2O не встраивается              | **ОШИБКА** | **libh2o** — официальная встраиваемая библиотека с примерами в `examples/libh2o/`, используемая PearlDB, ticketd и другими проектами                   |
| CivetWeb не поддерживает wolfSSL | **ОШИБКА** | CivetWeb имеет **частичную** поддержку wolfSSL. Есть документ `docs/yaSSL.md`, фиксы в issues #436 и #437. Однако OpenSSL — основной бэкенд            |
| Lwan не имеет WebSocket          | **ОШИБКА** | Официальный сайт lwan.ws **явно перечисляет WebSocket** в списке поддерживаемых функций                                                                |

**Неточности, требующие уточнения.** libmicrohttpd описан как «без WebSocket (только experimental)» — это устарело. С версии **v0.9.74** библиотека `libmicrohttpd_ws` предоставляет полноценную поддержку WebSocket, а с v1.0.0 (первый мажорный релиз) проект активно финансируется Sovereign Tech Agency и прошёл аудит безопасности в августе 2025. Статус facil.io как «mostly abandoned» неточен — проект **мигрировал** в новую организацию `facil-io` на GitHub, репозиторий `facil-io/cstl` обновлялся 28 февраля 2026. Это реструктуризация, не заброшенность. TLS в Lwan описан как «mbedTLS experimental», но на деле TLS функционален — поддерживается kTLS offload, AES-NI, с TLSv1.3 в разработке.

Что касается производительности picohttpparser: заявленные **~2.5 GB/s консервативны**. Независимые бенчмарки показывают **4.3–4.5 GB/s** с SSE4.2 на типичных HTTP-запросах. Cloudflare дополнительно ускорила парсер через AVX2, достигнув 1.68–1.79× ускорения поверх SSE4.2. Цифра 2.5 GB/s скорее соответствует httparse (Rust). Заявление о yyjson ~2.4 GB/s **правдоподобно**, но требует контекста: throughput зависит от датасета, оборудования и типа операции (парсинг vs сериализация). Официальная документация говорит о «gigabytes per second» без конкретных цифр.

---

## Заявление о «единственной QUIC-библиотеке с wolfSSL» проверено

Утверждение, что ngtcp2 — единственная QUIC-библиотека с нативной поддержкой wolfSSL, **подтверждается**. Проверены все основные QUIC-реализации: quiche (Cloudflare) использует исключительно BoringSSL, MsQuic (Microsoft) — Schannel/OpenSSL, lsquic (LiteSpeed) — BoringSSL, picoquic — picotls, Quinn — rustls, quic-go — Go crypto/tls, mvfst (Facebook) — Fizz. Только ngtcp2 предоставляет `libngtcp2_crypto_wolfssl` как first-party крипто-бэкенд. Интеграция wolfSSL в ngtcp2 была добавлена Stefan Eissing в августе 2022 и используется curl для HTTP/3.

---

## Лицензионный конфликт wolfSSL требует немедленного внимания

Обнаружена **серьёзная лицензионная неопределённость**. GitHub-репозиторий wolfSSL в файле LICENSING указывает **GPLv2**, а страница wolfssl.com/license — **GPLv3**. Мануал Chapter 14 снова говорит «GPLv2». Ни один источник не указывает «GPLv2+» (GPLv2 or later) — что критично, потому что:

- **GPLv2 (strict) несовместим с GPLv3**. Код под GPLv2-only нельзя комбинировать с GPLv3 в одной программе.
- **GPLv2+** совместим с GPLv3, так как можно выбрать «or later» и применить GPLv3.
- Если iohttp лицензирован под GPLv3, а wolfSSL — строго GPLv2, это **прямое нарушение лицензии**.

Остальные зависимости (ngtcp2, nghttp2, nghttp3, picohttpparser, yyjson) имеют MIT-лицензию и совместимы с любым GPL. **Рекомендация**: связаться с wolfSSL Inc. для уточнения лицензии или приобрести коммерческую лицензию wolfSSL, которая снимает все GPL-ограничения.

---

## io_uring: ядро 6.7 завышено, SQPOLL сомнителен

Заявленный минимум ядра **Linux 6.7 корректен, но избыточно консервативен**. Все перечисленные io_uring-фичи доступны начиная с **6.0**:

| Фича                     | Реальный минимум | Примечание                            |
| ------------------------ | ---------------- | ------------------------------------- |
| Multishot accept         | **5.19**         | `IORING_ACCEPT_MULTISHOT`             |
| Provided buffer rings    | **5.19**         | `IORING_REGISTER_PBUF_RING`           |
| Zero-copy send (SEND_ZC) | **6.0**          | `IORING_OP_SEND_ZC`, SENDMSG_ZC в 6.1 |
| SQPOLL                   | **5.1**          | Unprivileged с 5.13                   |
| Registered buffers/files | **5.1**          | Базовая io_uring функциональность     |

Если проект использует `IORING_SETUP_SINGLE_ISSUER` + `IORING_SETUP_DEFER_TASKRUN` (оба с 6.0) или incremental buffer consumption (`IOU_PBUF_RING_INC`, 6.12), то минимум может быть обоснован выше. Рекомендуется **явно указать**, какие фичи требуют 6.7, или понизить минимум до 6.0.

**Ring-per-thread подтверждён как industry best practice.** Это паттерн, используемый TigerBeetle, Glommio, uRocket, Apache Iggy и .NET runtime. Ключевые оптимизации: `IORING_SETUP_SINGLE_ISSUER` позволяет ядру оптимизировать для single-writer, `IORING_SETUP_DEFER_TASKRUN` улучшает cache locality. Оба требуют ring-per-thread и несовместимы с shared ring.

**SQPOLL под вопросом для HTTP-сервера.** Режим SQPOLL сжигает целый CPU core для поллинга SQ, что оправдано только при стабильно высокой нагрузке. Для HTTP-серверов с типичной бурстовой нагрузкой **`DEFER_TASKRUN` + `SINGLE_ISSUER` предпочтительнее** — они дают отличную производительность без постоянного расхода CPU. SQPOLL также **взаимоисключающ с DEFER_TASKRUN**. Рекомендуется сделать SQPOLL опциональным, не включённым по умолчанию.

**SEND_ZC полезен только условно.** Zero-copy send генерирует **два CQE** на операцию (completion + notification о доступности буфера) и даёт выигрыш только для payload > 4–16 KB. Для мелких HTTP-ответов overhead двойного CQE нивелирует преимущества. На localhost/loopback zero-copy **молча деградирует** до копирования. Флаг `IORING_SEND_ZC_REPORT_USAGE` позволяет диагностировать это в рантайме. Документ Architecture должен описать стратегию: когда используется ZC, а когда обычный send.

**Критическая уязвимость CVE-2024-0582** затрагивает provided buffer rings в ядрах 6.4–6.6.4 (use-after-free в `IORING_REGISTER_PBUF_RING`). Если проект поддерживает ядра < 6.6.5, это нужно явно указать как requirement. Также стоит упомянуть, что Google отключил io_uring на production-серверах, в Android и ChromeOS из-за того, что **60% эксплойтов** в 2022 были направлены на io_uring.

---

## wolfSSL + io_uring: фундаментальная проблема синхронности

Интеграция wolfSSL с io_uring имеет **документированную архитектурную проблему** (wolfSSL GitHub Issue #4605): wolfSSL callback'и `CallbackIORecv`/`CallbackIOSend` **синхронны** и должны возвращать результат немедленно, тогда как io_uring — completion-based и возвращает результат асинхронно.

**Решение — buffer-mediated pattern**: io_uring получает шифротекст в per-connection буфер, wolfSSL читает из этого буфера через custom callback (или через `wolfSSL_inject()`), а исходящий шифротекст после `wolfSSL_write()` копируется в outgoing buffer и отправляется через io_uring. Handshake требует state machine: `wolfSSL_accept()` вызывается многократно, с отправкой/получением данных через io_uring между вызовами.

Важное ограничение wolfSSL: **единый I/O буфер** на SSL-объект для отправки и получения. Это означает, что `wolfSSL_read()` и `wolfSSL_write()` на одном WOLFSSL-объекте **должны быть сериализованы** (mutex или write-duplicate feature). Архитектурный документ должен описать выбранный подход.

Выбор нативного wolfSSL API (не OpenSSL compat layer) **обоснован**: меньший binary size (~20–30% экономии), отсутствие namespace conflicts, прямой доступ к `wolfSSL_inject()`, static memory allocation и QUIC API. Потери: нет drop-in совместимости с OpenSSL-кодом и меньше community-примеров.

---

## 14 архитектурных пробелов, которые нужно закрыть

### Критические (блокируют production)

**1. Graceful shutdown.** Документ не описывает стратегию завершения. Необходимо: signalfd + io_uring poll для SIGTERM/SIGINT, отмена multishot accept, протокол-специфичная drain-фаза (`Connection: close` для HTTP/1.1, GOAWAY для HTTP/2, `H3_GOAWAY` + `CONNECTION_CLOSE` для HTTP/3), timeout через `io_uring_prep_timeout()`, и `io_uring_prep_cancel()` с `IORING_ASYNC_CANCEL_ALL`. Критический нюанс: закрытие сокета **не отменяет** pending io_uring operations автоматически.

**2. HTTP/2 GOAWAY и connection draining.** nghttp2 предоставляет `nghttp2_submit_goaway()`, но best practice — двухфазный GOAWAY: сначала с `last_stream_id = 2³¹-1` (запрет новых потоков), затем с реальным last_stream_id. Соединение закрывается когда `nghttp2_session_want_read() == 0 && nghttp2_session_want_write() == 0`.

**3. Защита от request smuggling.** Не упомянута, хотя это **критический вектор атаки**. Минимум: reject запросов с одновременно Content-Length и Transfer-Encoding, reject множественных Content-Length, строгий парсинг chunk sizes, запрет CR/LF в заголовках. Для HTTP/2→HTTP/1.1 desync: валидация при downgrade, strip `Transfer-Encoding` из HTTP/2 (RFC 9113 запрещает).

**4. Memory pressure и backpressure.** io_uring может аллоцировать неограниченную kernel memory для pending operations (liburing issue #293), вызывая OOM. Необходимо: `IORING_SETUP_CQ_NODROP`, лимиты in-flight операций, watermark-based flow control (stop recv при high watermark), мониторинг `-ENOBUFS` от provided buffers.

**5. Signal handling.** Минимум: `signal(SIGPIPE, SIG_IGN)` (io_uring возвращает ошибки, но wolfSSL может trigger SIGPIPE), signalfd для SIGTERM/SIGINT/SIGHUP, интеграция с io_uring event loop. Без `SIGPIPE` ignore сервер может **crash** при write на закрытый сокет через wolfSSL.

### Важные (влияют на производительность и операционность)

**6. UDP GSO/GRO для HTTP/3.** Не упомянуты, хотя дают значительный прирост throughput для QUIC. ngtcp2 **нативно поддерживает GSO** через `ngtcp2_conn_write_aggregate_pkt()`. GSO доступен с Linux 4.18, GRO — с 5.0. Интегрируется с io_uring через `io_uring_prep_sendmsg()` с `cmsg` для `UDP_SEGMENT`.

**7. QUIC congestion control.** ngtcp2 поддерживает CUBIC (default), NewReno и **BBRv2**. Для general-purpose HTTP/3 сервера BBRv2 предпочтителен — он даёт **21% лучше p99 latency** на lossy сетях (типичный QUIC-сценарий: мобильные клиенты). CUBIC деградирует уже при 0.1% packet loss. Должен быть конфигурируемым.

**8. QUIC connection migration.** Ключевое преимущество QUIC для мобильных клиентов не описано. ngtcp2 полностью поддерживает миграцию. Серверные требования: CID-to-connection lookup table, `SO_REUSEPORT` + eBPF для маршрутизации по CID (не по source IP), обработка PATH_CHALLENGE/PATH_RESPONSE, stateless reset tokens.

**9. Observability.** Упомянут Prometheus metrics, но не специфицированы. Стандартный набор: `http_requests_total{method,status,path}`, `http_request_duration_seconds`, `http_connections_active{protocol}`, `http3_quic_handshake_duration_seconds`, `io_uring_sqe_submitted_total`. Также нужны health endpoints (`/healthz` — liveness, `/readyz` — readiness с проверкой TLS certs, buffer pools, connection counts).

**10. Slowloris/Slow POST.** io_uring event-driven архитектура устойчивее thread-per-connection, но не иммунна. Нужны: header read timeout 5–15 сек (через `io_uring_prep_link_timeout()`), minimum transfer rate enforcement, per-IP connection limits (10–20), idle timeout для keep-alive.

### Желательные (улучшают полноту)

**11. Hot reload TLS-сертификатов.** Описан, но без деталей. Рекомендуемый паттерн: atomic pointer swap с reference counting — создать новый `WOLFSSL_CTX`, загрузить сертификаты через `wolfSSL_CTX_use_certificate_buffer()`, атомарно подменить глобальный указатель. Существующие соединения продолжают использовать старый CTX и освобождают его при закрытии.

**12. HTTP/2 Rapid Reset (CVE-2023-44487).** Не упомянута защита через `SETTINGS_MAX_CONCURRENT_STREAMS` в nghttp2. Без ограничения потоков сервер уязвим к атаке, которая положила множество production-серверов в октябре 2023.

**13. Distributed tracing.** Propagation W3C `traceparent`/`tracestate` headers не упомянута. Для enterprise-использования это стандартное требование.

**14. Configuration hot reload.** Помимо TLS-сертификатов, нужна стратегия reload для rate limits, CORS rules, router rules без перезапуска.

---

## liboas и OpenAPI 3.2.0: фантомная зависимость

C-библиотека «liboas» **не обнаружена** ни в одном публичном репозитории. Ближайший аналог — `libopenapi` на Go (pb33f). Если liboas — собственная разработка проекта, это нужно явно указать. OpenAPI **3.2.0 существует** — спецификация выпущена 23 сентября 2025.

Поддержка C23 `#embed` **достигла production-readiness**: GCC 15 (с `-std=gnu23` по умолчанию) и Clang 19+ полностью поддерживают директиву. Это обоснованный выбор для встраивания статических ресурсов в бинарник сервера.

---

## Сравнительный документ: 5 пропущенных конкурентов

Документ Comparison упускает несколько значимых альтернатив, которые стоит как минимум упомянуть для контекста.

**libh2o** — встраиваемая версия H2O — прямой конкурент с HTTP/1.1, HTTP/2, HTTP/3 и WebSocket. Используется Fastly и PowerDNS. Утверждение о не-встраиваемости H2O подрывает один из ключевых аргументов comparison документа.

**NGINX Unit** менее релевантен — он не поддерживает HTTP/2 (GitHub issue #388 открыт) и ориентирован на polyglot application serving, не на embedded HTTP. Но как контекстное упоминание полезен.

**Rust-альтернативы** (hyper, actix-web) стоит упомянуть не для прямого сравнения, а как контекст «что выбирают разработчики» — memory safety Rust устраняет целые классы уязвимостей, которые в C-сервере нужно предотвращать вручную.

**Drogon** (C++17, HTTP/1.1+HTTP/2, ORM, WebSocket) и **Boost.Beast** (HTTP/WebSocket на Asio) — ближайшие C++ альтернативы. Ни один не поддерживает HTTP/3 или io_uring, что является дифференциатором iohttp.

---

## Заключение: сильный фундамент, незрелый production story

Архитектура iohttp построена на правильных решениях: ring-per-thread, provided buffer rings, ngtcp2+nghttp2+nghttp3 — это best-in-class выбор компонентов. Заявленные ~14K–23K LOC собственного кода реалистичны для описанного scope. Однако документация содержит **4 фактические ошибки** в сравнении (H2O embeddability, BoringSSL, Lwan WebSocket, CivetWeb wolfSSL) и **лицензионный риск** с wolfSSL, который может сделать GPLv3-лицензирование проекта нелегитимным.

Три приоритета для немедленного исправления: (1) исправить фактические ошибки в comparison, особенно про H2O — это наиболее сильный конкурент; (2) разрешить лицензионный вопрос wolfSSL до публикации кода; (3) добавить в архитектурный документ секции по graceful shutdown, backpressure и security hardening — без них заявление о production-readiness не выдерживает технического scrutiny.
