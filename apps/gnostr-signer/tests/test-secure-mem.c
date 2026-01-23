/* test-secure-mem.c - Unit tests for secure memory management
 *
 * Tests secure memory allocation and handling including:
 * - gnostr_secure_alloc/free with mlock
 * - gnostr_secure_clear (explicit_bzero behavior)
 * - gnostr_secure_strdup/strfree
 * - Guard pages and canary detection
 * - Constant-time comparison
 * - Memory statistics tracking
 *
 * Issue: nostrc-ycd (Secure memory management for sensitive data)
 */
#include <glib.h>
#include <string.h>
#include <stdlib.h>

/* Include the secure memory header */
#include "../src/secure-mem.h"

/* ============================================================
 * Test Fixtures
 * ============================================================ */

static void
test_fixture_setup(void)
{
    /* Initialize secure memory subsystem */
    gnostr_secure_mem_init();
}

static void
test_fixture_teardown(void)
{
    /* Check for leaks and shutdown */
    GnostrSecureMemStats stats = gnostr_secure_mem_get_stats();
    if (stats.allocation_count > 0) {
        g_warning("Memory leak detected: %zu allocations remaining", stats.allocation_count);
    }
    gnostr_secure_mem_shutdown();
}

/* ============================================================
 * Basic Allocation Tests
 * ============================================================ */

static void
test_secure_alloc_basic(void)
{
    test_fixture_setup();

    /* Test basic allocation */
    void *ptr = gnostr_secure_alloc(64);
    g_assert_nonnull(ptr);

    /* Verify memory is zero-initialized */
    guint8 *bytes = (guint8 *)ptr;
    for (size_t i = 0; i < 64; i++) {
        g_assert_cmpuint(bytes[i], ==, 0);
    }

    /* Write some data */
    memset(ptr, 0x42, 64);

    /* Free it */
    gnostr_secure_free(ptr, 64);

    test_fixture_teardown();
}

static void
test_secure_alloc_zero_size(void)
{
    test_fixture_setup();

    /* Zero size should return NULL */
    void *ptr = gnostr_secure_alloc(0);
    g_assert_null(ptr);

    test_fixture_teardown();
}

static void
test_secure_alloc_large(void)
{
    test_fixture_setup();

    /* Test larger allocation (1MB) */
    size_t size = 1024 * 1024;
    void *ptr = gnostr_secure_alloc(size);
    g_assert_nonnull(ptr);

    /* Write pattern */
    memset(ptr, 0xAB, size);

    gnostr_secure_free(ptr, size);

    test_fixture_teardown();
}

static void
test_secure_alloc_multiple(void)
{
    test_fixture_setup();

    /* Allocate multiple buffers */
    void *ptr1 = gnostr_secure_alloc(32);
    void *ptr2 = gnostr_secure_alloc(64);
    void *ptr3 = gnostr_secure_alloc(128);

    g_assert_nonnull(ptr1);
    g_assert_nonnull(ptr2);
    g_assert_nonnull(ptr3);

    /* Verify they are distinct */
    g_assert_true(ptr1 != ptr2);
    g_assert_true(ptr2 != ptr3);
    g_assert_true(ptr1 != ptr3);

    /* Check stats */
    GnostrSecureMemStats stats = gnostr_secure_mem_get_stats();
    g_assert_cmpuint(stats.allocation_count, ==, 3);
    g_assert_cmpuint(stats.total_allocated, ==, 32 + 64 + 128);

    gnostr_secure_free(ptr1, 32);
    gnostr_secure_free(ptr2, 64);
    gnostr_secure_free(ptr3, 128);

    /* Verify all freed */
    stats = gnostr_secure_mem_get_stats();
    g_assert_cmpuint(stats.allocation_count, ==, 0);
    g_assert_cmpuint(stats.total_allocated, ==, 0);

    test_fixture_teardown();
}

/* ============================================================
 * Secure Clear Tests
 * ============================================================ */

static void
test_secure_clear(void)
{
    test_fixture_setup();

    /* Allocate and fill with known pattern */
    guint8 buffer[256];
    memset(buffer, 0xFF, sizeof(buffer));

    /* Clear it */
    gnostr_secure_clear(buffer, sizeof(buffer));

    /* Verify it's zeroed */
    for (size_t i = 0; i < sizeof(buffer); i++) {
        g_assert_cmpuint(buffer[i], ==, 0);
    }

    test_fixture_teardown();
}

static void
test_secure_clear_null(void)
{
    test_fixture_setup();

    /* Should handle NULL gracefully */
    gnostr_secure_clear(NULL, 64);
    gnostr_secure_clear(NULL, 0);

    /* Should handle zero size */
    guint8 buffer[16] = {0};
    gnostr_secure_clear(buffer, 0);

    test_fixture_teardown();
}

/* ============================================================
 * String Operations Tests
 * ============================================================ */

static void
test_secure_strdup(void)
{
    test_fixture_setup();

    const char *original = "This is a secret password!";
    gchar *copy = gnostr_secure_strdup(original);

    g_assert_nonnull(copy);
    g_assert_cmpstr(copy, ==, original);

    /* Verify it's a real copy, not just pointer copy */
    g_assert_true(copy != original);

    gnostr_secure_strfree(copy);

    test_fixture_teardown();
}

static void
test_secure_strdup_null(void)
{
    test_fixture_setup();

    gchar *copy = gnostr_secure_strdup(NULL);
    g_assert_null(copy);

    /* Freeing NULL should be safe */
    gnostr_secure_strfree(NULL);

    test_fixture_teardown();
}

static void
test_secure_strndup(void)
{
    test_fixture_setup();

    const char *original = "Hello, World!";

    /* Copy only first 5 characters */
    gchar *copy = gnostr_secure_strndup(original, 5);
    g_assert_nonnull(copy);
    g_assert_cmpstr(copy, ==, "Hello");

    gnostr_secure_strfree(copy);

    /* Copy with n larger than string length */
    copy = gnostr_secure_strndup(original, 100);
    g_assert_nonnull(copy);
    g_assert_cmpstr(copy, ==, original);

    gnostr_secure_strfree(copy);

    test_fixture_teardown();
}

/* ============================================================
 * Memory Locking Tests
 * ============================================================ */

static void
test_mlock_buffer(void)
{
    test_fixture_setup();

    guint8 buffer[4096];

    /* Try to lock the buffer */
    gboolean locked = gnostr_secure_mlock(buffer, sizeof(buffer));

    /* Note: mlock may fail without elevated privileges, which is OK */
    if (locked) {
        g_test_message("mlock succeeded");

        /* Unlock it */
        gnostr_secure_munlock(buffer, sizeof(buffer));
    } else {
        g_test_message("mlock not available (may need elevated privileges)");
    }

    test_fixture_teardown();
}

static void
test_mlock_available(void)
{
    test_fixture_setup();

    gboolean available = gnostr_secure_mlock_available();
    g_test_message("mlock available: %s", available ? "yes" : "no");

    /* Just verify the function works - result depends on system */
    g_assert_true(available == TRUE || available == FALSE);

    test_fixture_teardown();
}

/* ============================================================
 * Constant-Time Comparison Tests
 * ============================================================ */

static void
test_secure_memcmp_equal(void)
{
    test_fixture_setup();

    const guint8 a[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    const guint8 b[] = {0x01, 0x02, 0x03, 0x04, 0x05};

    int result = gnostr_secure_memcmp(a, b, sizeof(a));
    g_assert_cmpint(result, ==, 0);

    test_fixture_teardown();
}

static void
test_secure_memcmp_different(void)
{
    test_fixture_setup();

    const guint8 a[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    const guint8 b[] = {0x01, 0x02, 0x03, 0x04, 0x06};  /* Last byte different */

    int result = gnostr_secure_memcmp(a, b, sizeof(a));
    g_assert_cmpint(result, !=, 0);

    /* Also test when first byte differs */
    const guint8 c[] = {0xFF, 0x02, 0x03, 0x04, 0x05};
    result = gnostr_secure_memcmp(a, c, sizeof(a));
    g_assert_cmpint(result, !=, 0);

    test_fixture_teardown();
}

static void
test_secure_streq(void)
{
    test_fixture_setup();

    /* Equal strings */
    g_assert_true(gnostr_secure_streq("password", "password"));

    /* Different strings */
    g_assert_false(gnostr_secure_streq("password", "Password"));
    g_assert_false(gnostr_secure_streq("password", "password1"));
    g_assert_false(gnostr_secure_streq("password", "passwor"));

    /* NULL handling */
    g_assert_false(gnostr_secure_streq(NULL, "password"));
    g_assert_false(gnostr_secure_streq("password", NULL));
    /* Note: NULL == NULL returns TRUE per implementation (both are same pointer) */

    test_fixture_teardown();
}

/* ============================================================
 * Guard Page Tests
 * ============================================================ */

static void
test_guard_mode_setting(void)
{
    /* Note: Must be called before allocations */
    gnostr_secure_mem_init();

    GnostrGuardPageMode mode = gnostr_secure_get_guard_mode();
    g_test_message("Current guard mode: %d", mode);

    /* Verify mode is valid */
    g_assert_true(mode == GNOSTR_GUARD_NONE ||
                  mode == GNOSTR_GUARD_CANARY ||
                  mode == GNOSTR_GUARD_PAGES);

    gnostr_secure_mem_shutdown();
}

static void
test_guarded_allocation(void)
{
    gnostr_secure_mem_init();

    /* Allocate with explicit guard pages */
    void *ptr = gnostr_secure_alloc_guarded(256);
    g_assert_nonnull(ptr);

    /* Write some data */
    memset(ptr, 0x42, 256);

    /* Read back */
    guint8 *bytes = (guint8 *)ptr;
    for (size_t i = 0; i < 256; i++) {
        g_assert_cmpuint(bytes[i], ==, 0x42);
    }

    /* Free it */
    gnostr_secure_free_guarded(ptr, 256);

    gnostr_secure_mem_shutdown();
}

/* ============================================================
 * Statistics Tests
 * ============================================================ */

static void
test_statistics(void)
{
    test_fixture_setup();

    /* Initial stats */
    GnostrSecureMemStats stats = gnostr_secure_mem_get_stats();
    g_assert_cmpuint(stats.allocation_count, ==, 0);
    g_assert_cmpuint(stats.total_allocated, ==, 0);

    /* Allocate some memory */
    void *ptr1 = gnostr_secure_alloc(100);
    void *ptr2 = gnostr_secure_alloc(200);

    stats = gnostr_secure_mem_get_stats();
    g_assert_cmpuint(stats.allocation_count, ==, 2);
    g_assert_cmpuint(stats.total_allocated, ==, 300);
    g_assert_cmpuint(stats.peak_allocated, >=, 300);

    /* Free one */
    gnostr_secure_free(ptr1, 100);

    stats = gnostr_secure_mem_get_stats();
    g_assert_cmpuint(stats.allocation_count, ==, 1);
    g_assert_cmpuint(stats.total_allocated, ==, 200);
    g_assert_cmpuint(stats.peak_allocated, >=, 300);  /* Peak unchanged */

    /* Free the other */
    gnostr_secure_free(ptr2, 200);

    stats = gnostr_secure_mem_get_stats();
    g_assert_cmpuint(stats.allocation_count, ==, 0);
    g_assert_cmpuint(stats.total_allocated, ==, 0);

    test_fixture_teardown();
}

/* ============================================================
 * Macro Tests
 * ============================================================ */

static void
test_clear_buffer_macro(void)
{
    test_fixture_setup();

    guint8 buffer[32];
    memset(buffer, 0xFF, sizeof(buffer));

    /* Use the macro */
    GNOSTR_SECURE_CLEAR_BUFFER(buffer);

    /* Verify zeroed */
    for (size_t i = 0; i < sizeof(buffer); i++) {
        g_assert_cmpuint(buffer[i], ==, 0);
    }

    test_fixture_teardown();
}

/* ============================================================
 * New Buffer Operations Tests
 * ============================================================ */

static void
test_secure_copy(void)
{
    test_fixture_setup();

    const char *src = "sensitive data";
    char *dest = gnostr_secure_alloc(strlen(src) + 1);
    g_assert_nonnull(dest);

    gnostr_secure_copy(dest, src, strlen(src) + 1);
    g_assert_cmpstr(dest, ==, src);

    gnostr_secure_free(dest, strlen(src) + 1);

    test_fixture_teardown();
}

static void
test_secure_concat(void)
{
    test_fixture_setup();

    gchar *result = gnostr_secure_concat("Hello, ", "World!");
    g_assert_nonnull(result);
    g_assert_cmpstr(result, ==, "Hello, World!");

    gnostr_secure_free(result, strlen(result) + 1);

    /* Test with NULL */
    result = gnostr_secure_concat(NULL, "test");
    g_assert_nonnull(result);
    g_assert_cmpstr(result, ==, "test");
    gnostr_secure_free(result, strlen(result) + 1);

    result = gnostr_secure_concat("test", NULL);
    g_assert_nonnull(result);
    g_assert_cmpstr(result, ==, "test");
    gnostr_secure_free(result, strlen(result) + 1);

    result = gnostr_secure_concat(NULL, NULL);
    g_assert_null(result);

    test_fixture_teardown();
}

static void
test_secure_sprintf(void)
{
    test_fixture_setup();

    gchar *result = gnostr_secure_sprintf("Value: %d, String: %s", 42, "test");
    g_assert_nonnull(result);
    g_assert_cmpstr(result, ==, "Value: 42, String: test");

    gnostr_secure_free(result, strlen(result) + 1);

    /* Test NULL format */
    result = gnostr_secure_sprintf(NULL);
    g_assert_null(result);

    test_fixture_teardown();
}

/* ============================================================
 * Main Test Runner
 * ============================================================ */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Basic allocation tests */
    g_test_add_func("/secure-mem/alloc/basic", test_secure_alloc_basic);
    g_test_add_func("/secure-mem/alloc/zero-size", test_secure_alloc_zero_size);
    g_test_add_func("/secure-mem/alloc/large", test_secure_alloc_large);
    g_test_add_func("/secure-mem/alloc/multiple", test_secure_alloc_multiple);

    /* Secure clear tests */
    g_test_add_func("/secure-mem/clear/basic", test_secure_clear);
    g_test_add_func("/secure-mem/clear/null", test_secure_clear_null);

    /* String operation tests */
    g_test_add_func("/secure-mem/strdup/basic", test_secure_strdup);
    g_test_add_func("/secure-mem/strdup/null", test_secure_strdup_null);
    g_test_add_func("/secure-mem/strndup/basic", test_secure_strndup);

    /* Memory locking tests */
    g_test_add_func("/secure-mem/mlock/buffer", test_mlock_buffer);
    g_test_add_func("/secure-mem/mlock/available", test_mlock_available);

    /* Constant-time comparison tests */
    g_test_add_func("/secure-mem/memcmp/equal", test_secure_memcmp_equal);
    g_test_add_func("/secure-mem/memcmp/different", test_secure_memcmp_different);
    g_test_add_func("/secure-mem/streq", test_secure_streq);

    /* Guard page tests */
    g_test_add_func("/secure-mem/guard/mode", test_guard_mode_setting);
    g_test_add_func("/secure-mem/guard/allocation", test_guarded_allocation);

    /* Statistics tests */
    g_test_add_func("/secure-mem/stats", test_statistics);

    /* Macro tests */
    g_test_add_func("/secure-mem/macro/clear-buffer", test_clear_buffer_macro);

    /* Buffer operation tests */
    g_test_add_func("/secure-mem/ops/copy", test_secure_copy);
    g_test_add_func("/secure-mem/ops/concat", test_secure_concat);
    g_test_add_func("/secure-mem/ops/sprintf", test_secure_sprintf);

    return g_test_run();
}
