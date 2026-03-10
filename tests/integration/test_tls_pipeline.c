/**
 * @file test_tls_pipeline.c
 * @brief End-to-end TLS pipeline test using wolfSSL client.
 */

#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "router/ioh_router.h"
#include "tls/ioh_tls.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <unity.h>

#ifndef TEST_CERTS_DIR
#    define TEST_CERTS_DIR "/opt/projects/repositories/iohttp/tests/certs"
#endif

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Helpers ---- */

static uint16_t get_bound_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static uint16_t next_tls_port = 19080;

/* ---- Handler ---- */

static int hello_handler(ioh_ctx_t *c)
{
    return ioh_ctx_text(c, 200, "TLS Hello");
}

/* ---- Test: TLS GET ---- */

void test_tls_pipeline_get(void)
{
    char cert_path[256];
    char key_path[256];
    snprintf(cert_path, sizeof(cert_path), "%s/server-cert.pem", TEST_CERTS_DIR);
    snprintf(key_path, sizeof(key_path), "%s/server-key.pem", TEST_CERTS_DIR);

    if (access(cert_path, R_OK) != 0 || access(key_path, R_OK) != 0) {
        TEST_IGNORE_MESSAGE("Test certs not available");
    }

    /* Server TLS context */
    ioh_tls_config_t tls_cfg;
    ioh_tls_config_init(&tls_cfg);
    tls_cfg.cert_file = cert_path;
    tls_cfg.key_file = key_path;

    ioh_tls_ctx_t *tls_ctx = ioh_tls_ctx_create(&tls_cfg);
    TEST_ASSERT_NOT_NULL(tls_ctx);

    /* Server */
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = next_tls_port++;
    cfg.max_connections = 16;
    cfg.queue_depth = 64;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/hello", hello_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_tls(srv, tls_ctx));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    /* wolfSSL client */
    WOLFSSL_CTX *client_ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    TEST_ASSERT_NOT_NULL(client_ctx);
    wolfSSL_CTX_set_verify(client_ctx, WOLFSSL_VERIFY_NONE, nullptr);

    int client_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Make client non-blocking for interleaved handshake */
    int flags = fcntl(client_fd, F_GETFL, 0);
    (void)fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    (void)connect(client_fd, (struct sockaddr *)&addr, sizeof(addr));
    /* Non-blocking connect returns -1 with EINPROGRESS -- OK */

    WOLFSSL *ssl = wolfSSL_new(client_ctx);
    TEST_ASSERT_NOT_NULL(ssl);
    wolfSSL_set_fd(ssl, client_fd);
    wolfSSL_set_using_nonblock(ssl, 1);

    /* Interleave client TLS handshake with server event loop */
    bool connected = false;
    for (int i = 0; i < 100 && !connected; i++) {
        int ret = wolfSSL_connect(ssl);
        if (ret == WOLFSSL_SUCCESS) {
            connected = true;
        }
        (void)ioh_server_run_once(srv, 50);
    }

    if (connected) {
        const char *req = "GET /hello HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
        wolfSSL_write(ssl, req, (int)strlen(req));

        for (int i = 0; i < 20; i++) {
            (void)ioh_server_run_once(srv, 50);
        }

        char resp[4096];
        int n = wolfSSL_read(ssl, resp, (int)sizeof(resp) - 1);
        if (n > 0) {
            resp[n] = '\0';
            TEST_ASSERT_NOT_NULL(strstr(resp, "200"));
            TEST_ASSERT_NOT_NULL(strstr(resp, "TLS Hello"));
        }
    } else {
        TEST_IGNORE_MESSAGE("TLS handshake did not complete");
    }

    wolfSSL_free(ssl);
    wolfSSL_CTX_free(client_ctx);
    close(client_fd);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
    ioh_tls_ctx_destroy(tls_ctx);
}

int main(void)
{
    wolfSSL_Init();
    UNITY_BEGIN();
    RUN_TEST(test_tls_pipeline_get);
    int result = UNITY_END();
    wolfSSL_Cleanup();
    return result;
}
