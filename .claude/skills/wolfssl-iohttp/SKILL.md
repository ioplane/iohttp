---
name: wolfssl-iohttp
description: Use when writing wolfSSL integration code in iohttp — custom I/O callbacks for io_uring, non-blocking TLS, ALPN/SNI, mTLS, TLS metadata normalization, QUIC crypto, I/O serialization. MANDATORY for all src/tls/ code.
---

# wolfSSL Integration Patterns for iohttp

## Core Rule

Use wolfSSL **native API** — NOT the OpenSSL compatibility layer. Import `<wolfssl/ssl.h>` and `<wolfssl/wolfcrypt/*.h>` directly.

## CRITICAL: I/O Serialization

wolfSSL has a **single I/O buffer per SSL object**. Reads and writes MUST be serialized — never issue concurrent wolfSSL_read() and wolfSSL_write() on the same SSL object. This is natural in ring-per-thread model where each connection is owned by exactly one worker thread.

## CRITICAL: SIGPIPE

wolfSSL can trigger SIGPIPE through internal socket writes. **MUST** call `signal(SIGPIPE, SIG_IGN)` at startup before any TLS operations.

## Non-Blocking I/O Callback Pattern

wolfSSL reads/writes through application-controlled buffers, NOT directly from sockets:

```c
// Data flow:
// RECV: io_uring recv → cipher_buf → wolfSSL_read() → plaintext → HTTP parser
// SEND: HTTP response → wolfSSL_write() → cipher_buf → io_uring send

// Custom I/O callbacks (set per-SSL object):
static int io_tls_recv_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    io_tls_ctx_t *tls = ctx;
    if (tls->cipher_in_len == 0) {
        return WOLFSSL_CBIO_ERR_WANT_READ;  // Need more data from io_uring
    }
    size_t copy = (size_t)sz < tls->cipher_in_len ? (size_t)sz : tls->cipher_in_len;
    memcpy(buf, tls->cipher_in_buf + tls->cipher_in_pos, copy);
    tls->cipher_in_pos += copy;
    tls->cipher_in_len -= copy;
    return (int)copy;
}

static int io_tls_send_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    io_tls_ctx_t *tls = ctx;
    if (tls->cipher_out_len + (size_t)sz > tls->cipher_out_cap) {
        return WOLFSSL_CBIO_ERR_WANT_WRITE;  // Output buffer full
    }
    memcpy(tls->cipher_out_buf + tls->cipher_out_len, buf, (size_t)sz);
    tls->cipher_out_len += (size_t)sz;
    return sz;
}
```

## CRITICAL: SSL_MODE_ENABLE_PARTIAL_WRITE

```c
// MUST set — io_uring is non-blocking, partial writes are normal
wolfSSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
```

## WANT_READ / WANT_WRITE Handling

```c
int ret = wolfSSL_read(ssl, plain_buf, plain_sz);
if (ret <= 0) {
    int err = wolfSSL_get_error(ssl, ret);
    switch (err) {
    case WOLFSSL_ERROR_WANT_READ:
        // Submit io_uring recv SQE for more ciphertext
        // State: wait for CQE, then retry wolfSSL_read
        return IO_TLS_WANT_READ;
    case WOLFSSL_ERROR_WANT_WRITE:
        // Submit io_uring send SQE to flush cipher_out_buf
        // Then retry wolfSSL_read
        return IO_TLS_WANT_WRITE;
    default:
        // Real error — log and close
        return -EIO;
    }
}
```

## TLS Context Setup

```c
WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());

// TLS 1.3 primary, 1.2 optional compat
wolfSSL_CTX_SetMinVersion(ctx, WOLFSSL_TLSV1_2);

// ALPN for HTTP/2 negotiation
const char *protos = "\x02h2\x08http/1.1";
wolfSSL_CTX_UseALPN(ctx, protos, (unsigned)strlen(protos), WOLFSSL_ALPN_FAILED_ON_MISMATCH);

// SNI callback for multi-tenant
wolfSSL_CTX_set_servername_callback(ctx, io_tls_sni_cb);

// Partial write for non-blocking
wolfSSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);

// Session tickets
wolfSSL_CTX_set_timeout(ctx, 3600);  // 1 hour

// Certificate + key
wolfSSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM);
wolfSSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM);
```

## TLS Metadata Normalization

Extract and normalize for upper layers (middleware, liboas):

```c
typedef struct {
    bool             tls_active;
    int              version;          // e.g., WOLFSSL_TLSV1_3
    const char      *cipher_name;      // wolfSSL_get_cipher_name()
    const char      *alpn_proto;       // negotiated ALPN
    bool             client_cert;      // peer cert present
    int              verify_result;    // wolfSSL_get_verify_result()
    // Client cert details (if mTLS):
    char             subject[256];
    char             issuer[256];
    uint8_t          fingerprint[32];  // SHA-256
    // SAN list
} io_tls_peer_info_t;

// Populate after successful handshake:
void io_tls_extract_peer_info(WOLFSSL *ssl, io_tls_peer_info_t *info);
```

## mTLS

```c
// Require client certificate
wolfSSL_CTX_set_verify(ctx,
    SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
    io_tls_verify_cb);

// CRL checking
wolfSSL_CTX_LoadCRL(ctx, crl_path, SSL_FILETYPE_PEM, 0);
wolfSSL_CTX_EnableCRL(ctx, WOLFSSL_CRL_CHECKALL);
```

## Certificate Hot Reload (Without Restart)

```c
// Signal handler or admin API triggers:
// Atomic pointer swap with reference counting — old ctx freed after all connections close
int io_tls_reload_certs(io_tls_t *tls, const char *cert, const char *key) {
    WOLFSSL_CTX *new_ctx = /* create and configure new ctx */;
    WOLFSSL_CTX *old_ctx = tls->ctx;
    atomic_store_explicit(&tls->ctx, new_ctx, memory_order_release);
    // Old ctx freed after all connections using it close (refcount)
    return 0;
}
```

## QUIC Crypto (HTTP/3)

```c
// ngtcp2 has first-class wolfSSL support (only QUIC lib with native wolfSSL)
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>

// Setup QUIC crypto context
ngtcp2_crypto_wolfssl_configure_server_context(wolf_ctx);
```

## Buffer Separation

NEVER mix ciphertext and plaintext buffer ownership:

```
io_uring recv → cipher_in_buf (owned by TLS layer)
                    ↓
              wolfSSL_read()
                    ↓
              plain_buf (owned by HTTP parser / request)

HTTP response → wolfSSL_write() → cipher_out_buf (owned by TLS layer)
                                       ↓
                                  io_uring send
```

## Security Defaults

- Cipher suite restriction: TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256, TLS_AES_128_GCM_SHA256
- Disable: SSL 3.0, TLS 1.0, TLS 1.1
- OCSP stapling: optional, configure per-listener
- Session ticket key rotation
- `memset_explicit()` for all key material after use (C23), `explicit_bzero()` fallback

## RFC Cross-References

- RFC 8446 — TLS 1.3 (full implementation by wolfSSL)
- RFC 7301 — ALPN (`wolfSSL_CTX_UseALPN()`)
- RFC 6066 §3 — SNI (`wolfSSL_CTX_set_servername_callback()`)
- RFC 6066 §8 — OCSP Stapling (`wolfSSL_CTX_EnableOCSP()`)
- RFC 9001 — QUIC-TLS (`ngtcp2_crypto_wolfssl`)
- RFC 9325 — Secure Use of TLS (cipher suite restrictions, min version)
- RFC 8879 — TLS Certificate Compression

## wolfSSL License Note

wolfSSL license requires clarification before iohttp release:
- GitHub LICENSING: GPLv2
- wolfssl.com: GPLv3
- wolfSSL manual: GPLv2
If strictly GPLv2 (not "or later"), it's incompatible with iohttp's GPLv3. Resolve with wolfSSL Inc. or acquire commercial license.
