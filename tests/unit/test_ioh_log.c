/**
 * @file test_io_log.c
 * @brief Unit tests for ioh_log structured logging module.
 */

#include "core/ioh_log.h"

#include <string.h>

#include <unity.h>

/* ---- Test sink state ---- */

typedef struct {
    ioh_log_level_t last_level;
    char last_module[64];
    char last_message[256];
    int call_count;
} test_sink_state_t;

static test_sink_state_t g_state;

static void test_sink(ioh_log_level_t level, const char *module, const char *message,
                      void *user_data)
{
    test_sink_state_t *state = (test_sink_state_t *)user_data;
    state->last_level = level;
    if (module != nullptr) {
        strncpy(state->last_module, module, sizeof(state->last_module) - 1);
        state->last_module[sizeof(state->last_module) - 1] = '\0';
    } else {
        state->last_module[0] = '\0';
    }
    if (message != nullptr) {
        strncpy(state->last_message, message, sizeof(state->last_message) - 1);
        state->last_message[sizeof(state->last_message) - 1] = '\0';
    } else {
        state->last_message[0] = '\0';
    }
    state->call_count++;
}

void setUp(void)
{
    memset(&g_state, 0, sizeof(g_state));
    ioh_log_set_level(IOH_LOG_DEBUG);
    ioh_log_set_sink(test_sink, &g_state);
}

void tearDown(void)
{
    ioh_log_set_sink(nullptr, nullptr);
    ioh_log_set_level(IOH_LOG_INFO);
}

/* ---- test_log_basic_message ---- */

void test_log_basic_message(void)
{
    ioh_log(IOH_LOG_INFO, "server", "hello %s", "world");

    TEST_ASSERT_EQUAL_INT(1, g_state.call_count);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_INFO, g_state.last_level);
    TEST_ASSERT_EQUAL_STRING("server", g_state.last_module);
    TEST_ASSERT_EQUAL_STRING("hello world", g_state.last_message);
}

/* ---- test_log_level_filtering ---- */

void test_log_level_filtering(void)
{
    ioh_log_set_level(IOH_LOG_WARN);

    ioh_log(IOH_LOG_DEBUG, "test", "should be filtered");
    TEST_ASSERT_EQUAL_INT(0, g_state.call_count);

    ioh_log(IOH_LOG_INFO, "test", "also filtered");
    TEST_ASSERT_EQUAL_INT(0, g_state.call_count);

    ioh_log(IOH_LOG_WARN, "test", "should pass");
    TEST_ASSERT_EQUAL_INT(1, g_state.call_count);
    TEST_ASSERT_EQUAL_STRING("should pass", g_state.last_message);

    ioh_log(IOH_LOG_ERROR, "test", "also passes");
    TEST_ASSERT_EQUAL_INT(2, g_state.call_count);
    TEST_ASSERT_EQUAL_STRING("also passes", g_state.last_message);
}

/* ---- test_log_level_names ---- */

void test_log_level_names(void)
{
    TEST_ASSERT_EQUAL_STRING("ERROR", ioh_log_level_name(IOH_LOG_ERROR));
    TEST_ASSERT_EQUAL_STRING("WARN", ioh_log_level_name(IOH_LOG_WARN));
    TEST_ASSERT_EQUAL_STRING("INFO", ioh_log_level_name(IOH_LOG_INFO));
    TEST_ASSERT_EQUAL_STRING("DEBUG", ioh_log_level_name(IOH_LOG_DEBUG));

    /* Out-of-range level returns "UNKNOWN" */
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", ioh_log_level_name((ioh_log_level_t)99));
}

/* ---- test_log_convenience_macros ---- */

void test_log_convenience_macros(void)
{
    IOH_LOG_ERROR("core", "error %d", 42);
    TEST_ASSERT_EQUAL_INT(1, g_state.call_count);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_ERROR, g_state.last_level);
    TEST_ASSERT_EQUAL_STRING("core", g_state.last_module);
    TEST_ASSERT_EQUAL_STRING("error 42", g_state.last_message);

    IOH_LOG_WARN("net", "warning");
    TEST_ASSERT_EQUAL_INT(2, g_state.call_count);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_WARN, g_state.last_level);

    IOH_LOG_INFO("http", "info");
    TEST_ASSERT_EQUAL_INT(3, g_state.call_count);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_INFO, g_state.last_level);

    IOH_LOG_DEBUG("tls", "debug");
    TEST_ASSERT_EQUAL_INT(4, g_state.call_count);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_DEBUG, g_state.last_level);
}

/* ---- test_log_get_set_level ---- */

void test_log_get_set_level(void)
{
    ioh_log_set_level(IOH_LOG_ERROR);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_ERROR, ioh_log_get_level());

    ioh_log_set_level(IOH_LOG_DEBUG);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_DEBUG, ioh_log_get_level());

    ioh_log_set_level(IOH_LOG_WARN);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_WARN, ioh_log_get_level());

    ioh_log_set_level(IOH_LOG_INFO);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_INFO, ioh_log_get_level());
}

/* ---- test_log_null_inputs ---- */

void test_log_null_inputs(void)
{
    /* nullptr module should not crash */
    ioh_log(IOH_LOG_INFO, nullptr, "msg with null module");
    TEST_ASSERT_EQUAL_INT(1, g_state.call_count);
    TEST_ASSERT_EQUAL_STRING("unknown", g_state.last_module);
    TEST_ASSERT_EQUAL_STRING("msg with null module", g_state.last_message);

    /* nullptr fmt should not crash */
    ioh_log(IOH_LOG_WARN, "test", nullptr); // -V618
    TEST_ASSERT_EQUAL_INT(2, g_state.call_count);
    TEST_ASSERT_EQUAL_STRING("", g_state.last_message);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_log_basic_message);
    RUN_TEST(test_log_level_filtering);
    RUN_TEST(test_log_level_names);
    RUN_TEST(test_log_convenience_macros);
    RUN_TEST(test_log_get_set_level);
    RUN_TEST(test_log_null_inputs);

    return UNITY_END();
}
