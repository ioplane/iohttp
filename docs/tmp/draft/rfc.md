## HTTP/1.1 — ядро

Базовая семантика HTTP переехала в серию 9110–9114 (июнь 2022), заменив серию 7230–7235. Старые RFC **obsoleted**, но знать их полезно для понимания legacy-клиентов.

**RFC 9110** — HTTP Semantics. Методы, статус-коды, заголовки, content negotiation, conditional requests (ETag, If-Match, If-None-Match, If-Modified-Since), range requests, authentication framework. Это **главный документ** — всё остальное ссылается на него.

**RFC 9111** — HTTP Caching. Cache-Control, Age, Expires, Vary, heuristic freshness. Даже если iohttp не кэширует сам, корректная генерация заголовков кэширования для static files обязательна.

**RFC 9112** — HTTP/1.1. Wire format: request line, headers, chunked transfer encoding, message framing, connection management, keep-alive. Именно этот RFC описывает то, что парсит picohttpparser.

**RFC 9113** — HTTP/2. Frames, streams, HPACK, flow control, server push (deprecated), GOAWAY, priority (deprecated в пользу RFC 9218). Всё что делает nghttp2.

**RFC 9114** — HTTP/3. HTTP-over-QUIC framing, QPACK, stream mapping, server push, GOAWAY. Всё что делает nghttp3.

**RFC 9218** — Extensible Prioritization Scheme for HTTP. Заменяет старые HTTP/2 priority trees. Заголовок `Priority` с `u=` (urgency) и `i=` (incremental). Поддерживается nghttp2 и nghttp3.

**RFC 9530** — Digest Fields in HTTP. `Content-Digest` и `Repr-Digest` — замена устаревшего `Digest` header. Для integrity verification ответов.

---

## QUIC Transport

**RFC 9000** — QUIC: A UDP-Based Multiplexed and Secure Transport. Основной транспортный протокол. Streams, flow control, connection migration, loss detection. Реализован в ngtcp2.

**RFC 9001** — Using TLS to Secure QUIC. QUIC-TLS integration, 0-RTT, handshake encryption. Это то, что делает `ngtcp2_crypto_wolfssl`.

**RFC 9002** — QUIC Loss Detection and Congestion Control. Алгоритмы loss detection, RTT estimation, congestion window. ngtcp2 реализует это + BBRv2.

**RFC 9221** — An Unreliable Datagram Extension to QUIC. DATAGRAM frames — не обязателен для HTTP/3, но полезен для WebTransport в будущем.

**RFC 9287** — Greasing the QUIC Bit. Помогает противостоять middlebox ossification. ngtcp2 поддерживает.

**RFC 9368** — Compatible Version Negotiation for QUIC. Механизм согласования версий без round-trip penalty.

**RFC 9369** — QUIC Version 2. QUICv2 с изменёнными salt/key derivation для защиты от ossification. ngtcp2 поддерживает.

---

## TLS

**RFC 8446** — TLS 1.3. Основной протокол. wolfSSL реализует полностью.

**RFC 6066** — TLS Extensions: SNI, Max Fragment Length, OCSP Stapling. SNI обязателен для multi-tenant, OCSP stapling упомянут в архитектуре.

**RFC 5077** / **RFC 8447** — Session Tickets (TLS 1.2) и IANA TLS registry. Для session resumption.

**RFC 7301** — ALPN (Application-Layer Protocol Negotiation). Выбор h2/http/1.1 при TLS handshake. Критичен для HTTP/2 negotiation.

**RFC 8740** — Using TLS 1.3 with HTTP/2. Специфика: запрет renegotiation, post-handshake auth ограничения.

**RFC 9325** — Recommendations for Secure Use of TLS and DTLS. Best practices: минимальные cipher suites, запрет слабых алгоритмов.

---

## WebSocket

**RFC 6455** — The WebSocket Protocol. Уже упомянут в архитектуре. Handshake, framing, masking, close handshake, ping/pong.

**RFC 7692** — Compression Extensions for WebSocket. Per-message deflate (`permessage-deflate`). Если планируется сжатие WS-трафика.

**RFC 8441** — Bootstrapping WebSockets with HTTP/2. WebSocket over HTTP/2 через CONNECT. Если iohttp поддерживает WS поверх h2, этот RFC обязателен.

**RFC 9220** — Bootstrapping WebSockets with HTTP/3. Аналог RFC 8441 для HTTP/3. Extended CONNECT method.

---

## SSE

Формально SSE описан не в RFC, а в **W3C/WHATWG спецификации** — [Server-Sent Events](https://html.spec.whatwg.org/multipage/server-sent-events.html). Но для MIME-типа `text/event-stream` и поведения `Last-Event-ID` это единственный нормативный источник.

---

## CORS и Security Headers

**RFC 9110 §7.1.2** — Origin header (определён в HTTP Semantics).

Сам CORS описан в **Fetch Standard** (WHATWG), не в RFC. Но iohttp должен корректно обрабатывать `Origin`, `Access-Control-*` headers по этой спецификации.

**RFC 6797** — HSTS (HTTP Strict Transport Security). Заголовок `Strict-Transport-Security`.

**RFC 7762** — Initial Assignment for the Content Security Policy Directives Registry. Для CSP.

**RFC 9163** — Expect-CT Header Field for HTTP — deprecated с 2023, но Chrome всё ещё проверяет.

---

## Authentication

**RFC 9110 §11** — HTTP Authentication framework (Basic, Bearer).

**RFC 7617** — The 'Basic' HTTP Authentication Scheme.

**RFC 6750** — Bearer Token Usage (OAuth 2.0). Для `Authorization: Bearer`.

**RFC 7519** — JSON Web Token (JWT). Формат токенов.

**RFC 7515** — JSON Web Signature (JWS). Подписи JWT.

**RFC 7516** — JSON Web Encryption (JWE). Шифрованные JWT.

**RFC 7517** — JSON Web Key (JWK). Формат ключей.

**RFC 7518** — JSON Web Algorithms (JWA). Алгоритмы для JWS/JWE.

**RFC 8705** — OAuth 2.0 Mutual-TLS Client Authentication. Для mTLS-based auth, если iohttp реализует это.

---

## PROXY Protocol

PROXY Protocol описан **не в RFC**, а в спецификации HAProxy: [proxy-protocol.txt](https://www.haproxy.org/download/2.9/doc/proxy-protocol.txt). Версии v1 (text) и v2 (binary). Это де-факто стандарт, используемый HAProxy, nginx, AWS ELB/NLB, Envoy.

---

## Compression

**RFC 9110 §8.4** — Content-Encoding (gzip, deflate, br).

**RFC 7932** — Brotli Compressed Data Format. Формат brotli.

**RFC 8478** — Zstandard Compression and the `application/zstd` Media Type. Zstd набирает популярность — стоит рассмотреть помимо gzip/brotli.

**RFC 7694** — HTTP Client-Initiated Content-Encoding. `Accept-Encoding` в запросах.

---

## Cookies

**RFC 6265bis** (draft → RFC 6265) — Cookies: HTTP State Management Mechanism. Обновлённая версия с SameSite, `__Secure-`/`__Host-` prefixes. Даже если iohttp не управляет cookies напрямую, парсинг `Cookie` header и генерация `Set-Cookie` нужны middleware.

---

## URI и Content Negotiation

**RFC 3986** — URIs (уже есть).

**RFC 6570** — URI Template. Если роутер поддерживает шаблоны.

**RFC 9110 §12** — Content Negotiation. Accept, Accept-Language, Accept-Encoding, Vary.

**RFC 8288** — Web Linking. `Link` header для preload, prefetch, canonical.

---

## Static Files и Range Requests

**RFC 9110 §14** — Range Requests. `Range`, `Content-Range`, `Accept-Ranges`, multipart/byteranges. Обязателен для static file serving (видео стриминг, resume downloads).

**RFC 9110 §8.3** — Content-Type, §8.4 — Content-Encoding, §8.8 — Validators (ETag, Last-Modified).

**RFC 6838** — Media Type Specifications and Registration Procedures. Для MIME types.

---

## Observability и Metrics

**RFC 9457** — Problem Details for HTTP APIs. Формат `application/problem+json` для structured error responses.

**RFC 9512** — YAML Media Type. Если OpenAPI spec раздаётся в YAML.

---

## Distributed Tracing

**W3C Trace Context** — `traceparent` и `tracestate` headers. Не RFC, но W3C Recommendation и де-факто стандарт.

**RFC 9211** — The Cache-Status HTTP Response Header Field. Для диагностики кэширования в цепочке прокси.

---

## Итого: приоритетная карта

| Приоритет                    | RFC                                                        | Зачем                                                            |
| ---------------------------- | ---------------------------------------------------------- | ---------------------------------------------------------------- |
| **P0 — без них не работает** | 9110, 9112, 9113, 9114, 9000, 9001, 9002, 8446, 7301, 6455 | Базовые протоколы HTTP/1.1, /2, /3, QUIC, TLS, WS                |
| **P1 — production-ready**    | 9111, 9218, 6797, 7617, 6750, 7519, 9325, 9457, 9369       | Кэширование, приоритизация, HSTS, auth, security, errors, QUICv2 |
| **P2 — полнота**             | 8441, 9220, 7692, 7932, 8478, 9368, 9287, 8288, 6265bis    | WS over h2/h3, compression, QUIC extensions, cookies, linking    |
| **P3 — nice-to-have**        | 6570, 9221, 9530, 8705, 9512, 9211                         | URI templates, QUIC datagrams, digest, mTLS auth, cache-status   |
