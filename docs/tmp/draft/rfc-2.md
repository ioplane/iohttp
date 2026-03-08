# RFC для разработки iohttp

---

## 🔷 HTTP Core (Семантика и общие механизмы)

| RFC          | Название                                         | Описание                                                                      |
| ------------ | ------------------------------------------------ | ----------------------------------------------------------------------------- |
| **RFC 9110** | HTTP Semantics                                   | Основная семантика HTTP (методы, заголовки, статус-коды) — **фундамент**      |
| **RFC 9111** | HTTP Caching                                     | Кэширование HTTP (Cache-Control, ETag, Last-Modified, Vary)                   |
| **RFC 7233** | Range Requests                                   | Частичные запросы (Range, Content-Range, Accept-Ranges) — для resume download |
| **RFC 7232** | Conditional Requests                             | Условные запросы (If-Match, If-None-Match, If-Modified-Since)                 |
| **RFC 7231** | Semantics and Content (устарел, заменен на 9110) | Старый стандарт семантики                                                     |

---

## 🔷 HTTP/1.1

| RFC          | Название | Описание                                                                        |
| ------------ | -------- | ------------------------------------------------------------------------------- |
| **RFC 9112** | HTTP/1.1 | Спецификация HTTP/1.1 (message format, connection management, chunked encoding) |

---

## 🔷 HTTP/2

| RFC          | Название         | Описание                                                              |
| ------------ | ---------------- | --------------------------------------------------------------------- |
| **RFC 9113** | HTTP/2           | HTTP/2 (frames, streams, HPACK, flow control, server push deprecated) |
| **RFC 7540** | HTTP/2 (устарел) | Первая версия HTTP/2 — заменена на RFC 9113                           |

---

## 🔷 HTTP/3 и QUIC

| RFC          | Название                 | Описание                                               |
| ------------ | ------------------------ | ------------------------------------------------------ |
| **RFC 9114** | HTTP/3                   | HTTP/3 поверх QUIC                                     |
| **RFC 9204** | QPACK                    | Header compression для HTTP/3 (аналог HPACK)           |
| **RFC 9000** | QUIC Transport           | Транспортный протокол QUIC                             |
| **RFC 9001** | QUIC-TLS                 | Использование TLS 1.3 для QUIC                         |
| **RFC 9002** | QUIC Loss Detection      | Обнаружение потерь и контроль перегрузки в QUIC        |
| **RFC 9221** | QUIC Datagrams           | Ненадежные датаграммы в QUIC (WebTransport foundation) |
| **RFC 9369** | QUIC Version 2           | Версия 2 QUIC                                          |
| **RFC 9368** | QUIC Version Negotiation | Согласование версий QUIC                               |

---

## 🔷 WebSocket

| RFC          | Название              | Описание                                                 |
| ------------ | --------------------- | -------------------------------------------------------- |
| **RFC 6455** | WebSocket Protocol    | Основной протокол WebSocket (handshake, frames, masking) |
| **RFC 7692** | WebSocket Compression | Сжатие permessage-deflate для WebSocket                  |

---

## 🔷 Server-Sent Events (SSE)

| RFC/Spec                  | Название                        | Описание                                           |
| ------------------------- | ------------------------------- | -------------------------------------------------- |
| **HTML5 Living Standard** | Server-Sent Events              | Спецификация SSE (EventSource API, message format) |
| **RFC 6202**              | Known Issues and Best Practices | Рекомендации по использованию SSE                  |

---

## 🔷 TLS и Security

| RFC          | Название                                  | Описание                                  |
| ------------ | ----------------------------------------- | ----------------------------------------- |
| **RFC 8446** | TLS 1.3                                   | Transport Layer Security 1.3              |
| **RFC 5246** | TLS 1.2 (устарел)                         | Предыдущая версия TLS                     |
| **RFC 6066** | TLS Extensions                            | Расширения TLS (SNI, OCSP stapling, etc.) |
| **RFC 6961** | TLS Multiple Certificate Status Extension | OCSP stapling для multiple certificates   |

---

## 🔷 Безопасность и заголовки

| RFC          | Название                              | Описание                                             |
| ------------ | ------------------------------------- | ---------------------------------------------------- |
| **RFC 6797** | HTTP Strict Transport Security (HSTS) | Strict-Transport-Security header                     |
| **RFC 7034** | X-Frame-Options                       | Защита от clickjacking                               |
| **RFC 7469** | Public Key Pinning (HPKP)             | Пиннинг публичных ключей (deprecated, но для legacy) |
| **RFC 6454** | The Web Origin Concept                | Same-origin policy                                   |
| **RFC 3864** | Message Header Field Registration     | Регистрация HTTP-заголовков                          |

---

## 🔷 Аутентификация и авторизация

| RFC                               | Название                          | Описание                                    |
| --------------------------------- | --------------------------------- | ------------------------------------------- |
| **RFC 7617**                      | Basic HTTP Authentication Scheme  | Basic auth (user:pass в Base64)             |
| **RFC 7616**                      | Digest HTTP Authentication Scheme | Digest auth (challenge-response)            |
| **RFC 6750**                      | Bearer Token Usage                | OAuth 2.0 Bearer tokens                     |
| **RFC 7235**                      | HTTP Authentication Framework     | Общая framework для HTTP-аутентификации     |
| **RFC 6265**                      | HTTP State Management (Cookies)   | Cookies (Set-Cookie, Cookie headers)        |
| **draft-ietf-httpbis-rfc6265bis** | Cookies (обновление)              | Обновленная спецификация cookies с SameSite |

---

## 🔷 CORS и cross-origin

| RFC/Spec                  | Название                          | Описание                                            |
| ------------------------- | --------------------------------- | --------------------------------------------------- |
| **Fetch Living Standard** | CORS                              | Cross-Origin Resource Sharing (спецификация WHATWG) |
| **RFC 6598**              | IANA Registration of CORS Headers | Регистрация CORS-заголовков                         |

---

## 🔷 URI и URL

| RFC          | Название                           | Описание                           |
| ------------ | ---------------------------------- | ---------------------------------- |
| **RFC 3986** | Uniform Resource Identifier (URI)  | Общий синтаксис URI (уже упомянут) |
| **RFC 6874** | Representing IPv6 Zone Identifiers | IPv6 zone IDs в URIs               |
| **RFC 7320** | URI Design and Ownership           | Рекомендации по дизайну URI        |

---

## 🔷 Кодирование и сжатие

| RFC          | Название                       | Описание         |
| ------------ | ------------------------------ | ---------------- |
| **RFC 1950** | ZLIB Compressed Data Format    | Формат ZLIB      |
| **RFC 1951** | DEFLATE Compressed Data Format | Алгоритм DEFLATE |
| **RFC 1952** | GZIP File Format               | Формат GZIP      |
| **RFC 7932** | Brotli Compressed Data Format  | Алгоритм Brotli  |

---

## 🔷 Multipart и формы

| RFC          | Название                                         | Описание                                |
| ------------ | ------------------------------------------------ | --------------------------------------- |
| **RFC 7578** | Returning Values from Forms: multipart/form-data | Multipart/form-data для загрузки файлов |
| **RFC 2388** | multipart/form-data (устарел)                    | Старая версия — заменена на RFC 7578    |
| **RFC 2046** | MIME Part Two: Media Types                       | MIME-типы и multipart/\*                |
| **RFC 2183** | Content-Disposition Header                       | Content-Disposition header              |

---

## 🔷 JSON

| RFC          | Название                     | Описание                 |
| ------------ | ---------------------------- | ------------------------ |
| **RFC 8259** | JSON Data Interchange Format | JSON (обязательно UTF-8) |
| **RFC 7159** | JSON (устарел)               | Предыдущая версия JSON   |

---

## 🔷 PROXY Protocol

| Spec                     | Название               | Описание                                                                        |
| ------------------------ | ---------------------- | ------------------------------------------------------------------------------- |
| **PROXY Protocol v1/v2** | HAProxy PROXY Protocol | Декодирование реального IP за load balancer (не IETF RFC, но de facto стандарт) |

---

## 🔷 Расширенные возможности

| RFC          | Название                       | Описание                                            |
| ------------ | ------------------------------ | --------------------------------------------------- |
| **RFC 8297** | 103 Early Hints                | Informational ответ для предзагрузки ресурсов       |
| **RFC 7807** | Problem Details for HTTP APIs  | Стандартный формат ошибок API (заменен на RFC 9457) |
| **RFC 9457** | Problem Details (обновление)   | Обновленный формат ошибок API                       |
| **RFC 7240** | Prefer Header for HTTP         | Header Prefer для указания предпочтений             |
| **RFC 6585** | Additional HTTP Status Codes   | Коды 428, 429, 431, 511                             |
| **RFC 7538** | HTTP Status Code 308           | Permanent Redirect (аналог 301 без method change)   |
| **RFC 7725** | HTTP Status Code 451           | Unavailable For Legal Reasons                       |
| **RFC 8942** | HTTP Client Hints              | Client hints для адаптивного контента               |
| **RFC 9290** | HTTP Client Hints (обновление) | Расширенные client hints                            |

---

## 🔷 Server Push и подписки

| RFC          | Название                               | Описание                  |
| ------------ | -------------------------------------- | ------------------------- |
| **RFC 8030** | Generic Event Delivery Using HTTP Push | WebSub/Web Push протоколы |

---

## 🔷 Certificate Transparency

| RFC          | Название                              | Описание                                           |
| ------------ | ------------------------------------- | -------------------------------------------------- |
| **RFC 6962** | Certificate Transparency              | CT logs, SCTs (устаревает, но широко используется) |
| **RFC 9162** | Certificate Transparency (обновление) | Обновленная версия CT                              |

---

## 🔷 Rate Limiting

| Draft/RFC                                | Название                | Описание                                       |
| ---------------------------------------- | ----------------------- | ---------------------------------------------- |
| **draft-ietf-httpbis-ratelimit-headers** | RateLimit Header Fields | Стандартные заголовки RateLimit (в разработке) |

---

## 📋 Итоговый чек-лист для iohttp

### Критически важные (обязательны):

- [ ] **RFC 9110** — HTTP Semantics
- [ ] **RFC 9112** — HTTP/1.1
- [ ] **RFC 9113** — HTTP/2
- [ ] **RFC 9114** — HTTP/3
- [ ] **RFC 9000** — QUIC Transport
- [ ] **RFC 9001** — QUIC-TLS
- [ ] **RFC 6455** — WebSocket
- [ ] **RFC 8446** — TLS 1.3
- [ ] **RFC 3986** — URI (уже учитывается)

### Важные для production:

- [ ] **RFC 9111** — HTTP Caching
- [ ] **RFC 7233** — Range Requests
- [ ] **RFC 6265** — Cookies
- [ ] **RFC 6797** — HSTS
- [ ] **RFC 7578** — multipart/form-data
- [ ] **RFC 8259** — JSON
- [ ] **RFC 1952** — GZIP
- [ ] **RFC 7932** — Brotli

### Для расширенной функциональности:

- [ ] **RFC 8297** — Early Hints (103)
- [ ] **RFC 9457** — Problem Details
- [ ] **RFC 9221** — QUIC Datagrams (для WebTransport)
- [ ] **draft-ietf-httpbis-ratelimit-headers** — RateLimit headers

---
