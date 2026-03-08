/**
 * @file io_auth.c
 * @brief Basic and Bearer authentication middleware implementation.
 */

#include "middleware/io_auth.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    io_auth_verify_fn verify;
    void *ctx;
} io_auth_state_t;

static _Thread_local io_auth_state_t *tl_basic_state;
static _Thread_local io_auth_state_t *tl_bearer_state;

/* ---- Simple Base64 decoder ---- */

static const int8_t B64_TABLE[256] = {
    ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,
    ['E'] = 4,  ['F'] = 5,  ['G'] = 6,  ['H'] = 7,
    ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11,
    ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
    ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19,
    ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
    ['Y'] = 24, ['Z'] = 25,
    ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29,
    ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33,
    ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37,
    ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41,
    ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45,
    ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
    ['y'] = 50, ['z'] = 51,
    ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
    ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59,
    ['8'] = 60, ['9'] = 61,
    ['+'] = 62, ['/'] = 63,
};

/**
 * Decode base64 into out buffer. Returns decoded length, or -1 on error.
 */
static int b64_decode(const char *src, size_t src_len,
                      char *out, size_t out_size)
{
    if (src_len == 0) {
        return 0;
    }

    /* strip padding */
    size_t len = src_len;
    while (len > 0 && src[len - 1] == '=') {
        len--;
    }

    size_t needed = (len * 3) / 4;
    if (needed >= out_size) {
        return -1;
    }

    size_t j = 0;
    uint32_t accum = 0;
    uint32_t bits = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)src[i];
        int8_t val = B64_TABLE[ch];
        /* check for invalid chars: A maps to 0 but is valid */
        if (val == 0 && ch != 'A') {
            return -1;
        }
        accum = (accum << 6) | (uint32_t)val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[j++] = (char)(accum >> bits);
            accum &= (1U << bits) - 1;
        }
    }

    out[j] = '\0';
    return (int)j;
}

static int basic_middleware(io_request_t *req, io_response_t *resp,
                            int (*next)(io_request_t *, io_response_t *))
{
    io_auth_state_t *st = tl_basic_state;
    const char *auth = io_request_header(req, "Authorization");

    if (auth == nullptr || strncmp(auth, "Basic ", 6) != 0) {
        resp->status = 401;
        (void)io_response_set_header(resp, "WWW-Authenticate",
                                     "Basic realm=\"iohttp\"");
        return 0;
    }

    const char *encoded = auth + 6;
    size_t enc_len = strlen(encoded);
    char decoded[512];

    int dec_len = b64_decode(encoded, enc_len, decoded, sizeof(decoded));
    if (dec_len < 0) {
        resp->status = 401;
        (void)io_response_set_header(resp, "WWW-Authenticate",
                                     "Basic realm=\"iohttp\"");
        return 0;
    }

    if (!st->verify(decoded, st->ctx)) {
        resp->status = 401;
        (void)io_response_set_header(resp, "WWW-Authenticate",
                                     "Basic realm=\"iohttp\"");
        return 0;
    }

    return next(req, resp);
}

static int bearer_middleware(io_request_t *req, io_response_t *resp,
                             int (*next)(io_request_t *, io_response_t *))
{
    io_auth_state_t *st = tl_bearer_state;
    const char *auth = io_request_header(req, "Authorization");

    if (auth == nullptr || strncmp(auth, "Bearer ", 7) != 0) {
        resp->status = 401;
        (void)io_response_set_header(resp, "WWW-Authenticate", "Bearer");
        return 0;
    }

    const char *token = auth + 7;

    if (!st->verify(token, st->ctx)) {
        resp->status = 401;
        (void)io_response_set_header(resp, "WWW-Authenticate", "Bearer");
        return 0;
    }

    return next(req, resp);
}

io_middleware_fn io_auth_basic_create(io_auth_verify_fn verify, void *ctx)
{
    if (verify == nullptr) {
        return nullptr;
    }

    io_auth_state_t *st = malloc(sizeof(*st));
    if (st == nullptr) {
        return nullptr;
    }
    st->verify = verify;
    st->ctx = ctx;
    tl_basic_state = st;
    return basic_middleware;
}

io_middleware_fn io_auth_bearer_create(io_auth_verify_fn verify, void *ctx)
{
    if (verify == nullptr) {
        return nullptr;
    }

    io_auth_state_t *st = malloc(sizeof(*st));
    if (st == nullptr) {
        return nullptr;
    }
    st->verify = verify;
    st->ctx = ctx;
    tl_bearer_state = st;
    return bearer_middleware;
}

void io_auth_destroy(void)
{
    free(tl_basic_state);
    tl_basic_state = nullptr;
    free(tl_bearer_state);
    tl_bearer_state = nullptr;
}
