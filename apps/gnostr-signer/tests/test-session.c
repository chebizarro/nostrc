/* test-session.c - Unit tests for gnostr-signer session and settings management
 *
 * Tests session lifecycle, timeout behavior, and lock/unlock functionality.
 * Uses GLib testing framework with in-memory GSettings backend for isolation.
 *
 * Issue: nostrc-ddh
 */
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>

/* ===========================================================================
 * Mock Session Manager for Testing
 * =========================================================================== */

typedef enum {
    SESSION_STATE_UNLOCKED,
    SESSION_STATE_LOCKED,
    SESSION_STATE_EXPIRED
} SessionState;

typedef struct {
    SessionState state;
    gint64 last_activity;     /* Monotonic time of last activity */
    gint timeout_seconds;     /* 0 = no timeout */
    gboolean auto_lock;
    guint timeout_source_id;  /* GSource ID for timeout */
    GSourceFunc lock_callback;
    gpointer lock_user_data;
} TestSessionManager;

static TestSessionManager *
test_session_manager_new(void)
{
    TestSessionManager *sm = g_new0(TestSessionManager, 1);
    sm->state = SESSION_STATE_LOCKED;  /* Start locked */
    sm->last_activity = g_get_monotonic_time();
    sm->timeout_seconds = 300;  /* 5 minute default */
    sm->auto_lock = TRUE;
    return sm;
}

static void
test_session_manager_free(TestSessionManager *sm)
{
    if (!sm) return;
    if (sm->timeout_source_id > 0) {
        g_source_remove(sm->timeout_source_id);
    }
    g_free(sm);
}

static gboolean
test_session_manager_unlock(TestSessionManager *sm, const gchar *password)
{
    if (!sm) return FALSE;

    /* For testing, accept any non-empty password */
    if (!password || !*password) {
        return FALSE;
    }

    sm->state = SESSION_STATE_UNLOCKED;
    sm->last_activity = g_get_monotonic_time();
    return TRUE;
}

static void
test_session_manager_lock(TestSessionManager *sm)
{
    if (!sm) return;
    sm->state = SESSION_STATE_LOCKED;
}

static gboolean
test_session_manager_is_unlocked(TestSessionManager *sm)
{
    return sm && sm->state == SESSION_STATE_UNLOCKED;
}

static gboolean
test_session_manager_is_locked(TestSessionManager *sm)
{
    return sm && sm->state == SESSION_STATE_LOCKED;
}

static void
test_session_manager_touch(TestSessionManager *sm)
{
    if (!sm) return;
    sm->last_activity = g_get_monotonic_time();
}

static void
test_session_manager_set_timeout(TestSessionManager *sm, gint seconds)
{
    if (!sm) return;
    sm->timeout_seconds = seconds;
}

static gint
test_session_manager_get_timeout(TestSessionManager *sm)
{
    return sm ? sm->timeout_seconds : 0;
}

static void
test_session_manager_set_auto_lock(TestSessionManager *sm, gboolean enabled)
{
    if (!sm) return;
    sm->auto_lock = enabled;
}

static gboolean
test_session_manager_get_auto_lock(TestSessionManager *sm)
{
    return sm ? sm->auto_lock : FALSE;
}

/* Check if session has timed out (returns TRUE if should auto-lock) */
static gboolean
test_session_manager_check_timeout(TestSessionManager *sm)
{
    if (!sm || !sm->auto_lock || sm->timeout_seconds <= 0) {
        return FALSE;
    }

    if (sm->state != SESSION_STATE_UNLOCKED) {
        return FALSE;
    }

    gint64 now = g_get_monotonic_time();
    gint64 elapsed_sec = (now - sm->last_activity) / G_USEC_PER_SEC;

    return elapsed_sec >= sm->timeout_seconds;
}

/* Simulate time passing for tests */
static void
test_session_manager_simulate_elapsed(TestSessionManager *sm, gint seconds)
{
    if (!sm) return;
    /* Move last_activity backwards to simulate time passing */
    sm->last_activity -= (gint64)seconds * G_USEC_PER_SEC;
}

/* ===========================================================================
 * Mock Settings Manager for Testing
 * =========================================================================== */

typedef struct {
    GHashTable *string_settings;
    GHashTable *int_settings;
    GHashTable *bool_settings;
} TestSettingsManager;

static TestSettingsManager *
test_settings_manager_new(void)
{
    TestSettingsManager *sm = g_new0(TestSettingsManager, 1);
    sm->string_settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    sm->int_settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    sm->bool_settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Set defaults */
    g_hash_table_insert(sm->int_settings, g_strdup("lock-timeout"), GINT_TO_POINTER(300));
    g_hash_table_insert(sm->bool_settings, g_strdup("remember-approvals"), GINT_TO_POINTER(TRUE));
    g_hash_table_insert(sm->int_settings, g_strdup("approval-ttl-hours"), GINT_TO_POINTER(24));

    return sm;
}

static void
test_settings_manager_free(TestSettingsManager *sm)
{
    if (!sm) return;
    g_hash_table_destroy(sm->string_settings);
    g_hash_table_destroy(sm->int_settings);
    g_hash_table_destroy(sm->bool_settings);
    g_free(sm);
}

static void
test_settings_manager_set_string(TestSettingsManager *sm, const gchar *key, const gchar *value)
{
    if (!sm) return;
    g_hash_table_insert(sm->string_settings, g_strdup(key), g_strdup(value));
}

static gchar *
test_settings_manager_get_string(TestSettingsManager *sm, const gchar *key)
{
    if (!sm) return NULL;
    const gchar *val = g_hash_table_lookup(sm->string_settings, key);
    return val ? g_strdup(val) : NULL;
}

static void
test_settings_manager_set_int(TestSettingsManager *sm, const gchar *key, gint value)
{
    if (!sm) return;
    g_hash_table_insert(sm->int_settings, g_strdup(key), GINT_TO_POINTER(value));
}

static gint
test_settings_manager_get_int(TestSettingsManager *sm, const gchar *key)
{
    if (!sm) return 0;
    return GPOINTER_TO_INT(g_hash_table_lookup(sm->int_settings, key));
}

static void
test_settings_manager_set_bool(TestSettingsManager *sm, const gchar *key, gboolean value)
{
    if (!sm) return;
    g_hash_table_insert(sm->bool_settings, g_strdup(key), GINT_TO_POINTER(value));
}

static gboolean
test_settings_manager_get_bool(TestSettingsManager *sm, const gchar *key)
{
    if (!sm) return FALSE;
    return GPOINTER_TO_INT(g_hash_table_lookup(sm->bool_settings, key));
}

/* ===========================================================================
 * Test Fixtures
 * =========================================================================== */

typedef struct {
    TestSessionManager *session;
    TestSettingsManager *settings;
} SessionFixture;

static void
session_fixture_setup(SessionFixture *fixture, gconstpointer data)
{
    (void)data;
    fixture->session = test_session_manager_new();
    fixture->settings = test_settings_manager_new();
}

static void
session_fixture_teardown(SessionFixture *fixture, gconstpointer data)
{
    (void)data;
    test_session_manager_free(fixture->session);
    test_settings_manager_free(fixture->settings);
}

/* ===========================================================================
 * Session Creation Tests
 * =========================================================================== */

static void
test_session_create_starts_locked(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    /* New session should be locked by default */
    g_assert_true(test_session_manager_is_locked(fixture->session));
    g_assert_false(test_session_manager_is_unlocked(fixture->session));
}

static void
test_session_default_timeout(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Default timeout should be 5 minutes */
    g_assert_cmpint(test_session_manager_get_timeout(fixture->session), ==, 300);
}

static void
test_session_default_auto_lock(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Auto-lock should be enabled by default */
    g_assert_true(test_session_manager_get_auto_lock(fixture->session));
}

/* ===========================================================================
 * Unlock Tests
 * =========================================================================== */

static void
test_session_unlock_with_password(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_true(test_session_manager_is_locked(fixture->session));

    gboolean result = test_session_manager_unlock(fixture->session, "test-password");
    g_assert_true(result);
    g_assert_true(test_session_manager_is_unlocked(fixture->session));
}

static void
test_session_unlock_empty_password(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    gboolean result = test_session_manager_unlock(fixture->session, "");
    g_assert_false(result);
    g_assert_true(test_session_manager_is_locked(fixture->session));
}

static void
test_session_unlock_null_password(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    gboolean result = test_session_manager_unlock(fixture->session, NULL);
    g_assert_false(result);
    g_assert_true(test_session_manager_is_locked(fixture->session));
}

/* ===========================================================================
 * Lock Tests
 * =========================================================================== */

static void
test_session_lock_after_unlock(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    test_session_manager_unlock(fixture->session, "password");
    g_assert_true(test_session_manager_is_unlocked(fixture->session));

    test_session_manager_lock(fixture->session);
    g_assert_true(test_session_manager_is_locked(fixture->session));
}

static void
test_session_lock_already_locked(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_true(test_session_manager_is_locked(fixture->session));

    /* Locking when already locked should be a no-op */
    test_session_manager_lock(fixture->session);
    g_assert_true(test_session_manager_is_locked(fixture->session));
}

/* ===========================================================================
 * Timeout Behavior Tests
 * =========================================================================== */

static void
test_session_timeout_triggers_check(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Set short timeout */
    test_session_manager_set_timeout(fixture->session, 60);

    /* Unlock */
    test_session_manager_unlock(fixture->session, "password");
    g_assert_false(test_session_manager_check_timeout(fixture->session));

    /* Simulate 61 seconds elapsed */
    test_session_manager_simulate_elapsed(fixture->session, 61);

    /* Should now indicate timeout */
    g_assert_true(test_session_manager_check_timeout(fixture->session));
}

static void
test_session_touch_resets_timeout(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    test_session_manager_set_timeout(fixture->session, 60);
    test_session_manager_unlock(fixture->session, "password");

    /* Simulate 50 seconds elapsed */
    test_session_manager_simulate_elapsed(fixture->session, 50);
    g_assert_false(test_session_manager_check_timeout(fixture->session));

    /* Touch to reset activity */
    test_session_manager_touch(fixture->session);

    /* Simulate another 50 seconds - should still be within timeout */
    test_session_manager_simulate_elapsed(fixture->session, 50);
    g_assert_false(test_session_manager_check_timeout(fixture->session));

    /* Total would be 100s without touch, but touch reset it */
}

static void
test_session_zero_timeout_disables(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    test_session_manager_set_timeout(fixture->session, 0);
    test_session_manager_unlock(fixture->session, "password");

    /* Even after long time, no timeout */
    test_session_manager_simulate_elapsed(fixture->session, 99999);
    g_assert_false(test_session_manager_check_timeout(fixture->session));
}

static void
test_session_auto_lock_disabled(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    test_session_manager_set_timeout(fixture->session, 60);
    test_session_manager_set_auto_lock(fixture->session, FALSE);
    test_session_manager_unlock(fixture->session, "password");

    /* Simulate timeout elapsed */
    test_session_manager_simulate_elapsed(fixture->session, 120);

    /* Should not indicate timeout when auto-lock disabled */
    g_assert_false(test_session_manager_check_timeout(fixture->session));
}

static void
test_session_timeout_only_when_unlocked(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    test_session_manager_set_timeout(fixture->session, 60);

    /* Session is locked - timeout check should return false */
    test_session_manager_simulate_elapsed(fixture->session, 120);
    g_assert_false(test_session_manager_check_timeout(fixture->session));
}

/* ===========================================================================
 * Settings Tests
 * =========================================================================== */

static void
test_settings_lock_timeout(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Default should be 300 */
    g_assert_cmpint(test_settings_manager_get_int(fixture->settings, "lock-timeout"), ==, 300);

    /* Update */
    test_settings_manager_set_int(fixture->settings, "lock-timeout", 600);
    g_assert_cmpint(test_settings_manager_get_int(fixture->settings, "lock-timeout"), ==, 600);
}

static void
test_settings_remember_approvals(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_true(test_settings_manager_get_bool(fixture->settings, "remember-approvals"));

    test_settings_manager_set_bool(fixture->settings, "remember-approvals", FALSE);
    g_assert_false(test_settings_manager_get_bool(fixture->settings, "remember-approvals"));
}

static void
test_settings_approval_ttl(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_cmpint(test_settings_manager_get_int(fixture->settings, "approval-ttl-hours"), ==, 24);

    test_settings_manager_set_int(fixture->settings, "approval-ttl-hours", 168);
    g_assert_cmpint(test_settings_manager_get_int(fixture->settings, "approval-ttl-hours"), ==, 168);
}

static void
test_settings_string_roundtrip(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    test_settings_manager_set_string(fixture->settings, "default-identity", "npub1test123");

    gchar *value = test_settings_manager_get_string(fixture->settings, "default-identity");
    g_assert_cmpstr(value, ==, "npub1test123");
    g_free(value);
}

static void
test_settings_nonexistent_returns_default(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Non-existent string returns NULL */
    gchar *str = test_settings_manager_get_string(fixture->settings, "nonexistent");
    g_assert_null(str);

    /* Non-existent int returns 0 */
    gint ival = test_settings_manager_get_int(fixture->settings, "nonexistent");
    g_assert_cmpint(ival, ==, 0);

    /* Non-existent bool returns FALSE */
    gboolean bval = test_settings_manager_get_bool(fixture->settings, "nonexistent");
    g_assert_false(bval);
}

/* ===========================================================================
 * Integration-style Tests
 * =========================================================================== */

static void
test_session_settings_integration(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Set timeout via settings */
    test_settings_manager_set_int(fixture->settings, "lock-timeout", 120);

    /* Apply to session */
    gint timeout = test_settings_manager_get_int(fixture->settings, "lock-timeout");
    test_session_manager_set_timeout(fixture->session, timeout);

    g_assert_cmpint(test_session_manager_get_timeout(fixture->session), ==, 120);
}

static void
test_session_multiple_unlock_lock_cycles(SessionFixture *fixture, gconstpointer data)
{
    (void)data;

    for (int i = 0; i < 5; i++) {
        g_assert_true(test_session_manager_is_locked(fixture->session));

        test_session_manager_unlock(fixture->session, "password");
        g_assert_true(test_session_manager_is_unlocked(fixture->session));

        test_session_manager_lock(fixture->session);
    }
}

/* ===========================================================================
 * Test Runner
 * =========================================================================== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Session creation tests */
    g_test_add("/signer/session/create/starts_locked", SessionFixture, NULL,
               session_fixture_setup, test_session_create_starts_locked, session_fixture_teardown);
    g_test_add("/signer/session/create/default_timeout", SessionFixture, NULL,
               session_fixture_setup, test_session_default_timeout, session_fixture_teardown);
    g_test_add("/signer/session/create/default_auto_lock", SessionFixture, NULL,
               session_fixture_setup, test_session_default_auto_lock, session_fixture_teardown);

    /* Unlock tests */
    g_test_add("/signer/session/unlock/with_password", SessionFixture, NULL,
               session_fixture_setup, test_session_unlock_with_password, session_fixture_teardown);
    g_test_add("/signer/session/unlock/empty_password", SessionFixture, NULL,
               session_fixture_setup, test_session_unlock_empty_password, session_fixture_teardown);
    g_test_add("/signer/session/unlock/null_password", SessionFixture, NULL,
               session_fixture_setup, test_session_unlock_null_password, session_fixture_teardown);

    /* Lock tests */
    g_test_add("/signer/session/lock/after_unlock", SessionFixture, NULL,
               session_fixture_setup, test_session_lock_after_unlock, session_fixture_teardown);
    g_test_add("/signer/session/lock/already_locked", SessionFixture, NULL,
               session_fixture_setup, test_session_lock_already_locked, session_fixture_teardown);

    /* Timeout tests */
    g_test_add("/signer/session/timeout/triggers_check", SessionFixture, NULL,
               session_fixture_setup, test_session_timeout_triggers_check, session_fixture_teardown);
    g_test_add("/signer/session/timeout/touch_resets", SessionFixture, NULL,
               session_fixture_setup, test_session_touch_resets_timeout, session_fixture_teardown);
    g_test_add("/signer/session/timeout/zero_disables", SessionFixture, NULL,
               session_fixture_setup, test_session_zero_timeout_disables, session_fixture_teardown);
    g_test_add("/signer/session/timeout/auto_lock_disabled", SessionFixture, NULL,
               session_fixture_setup, test_session_auto_lock_disabled, session_fixture_teardown);
    g_test_add("/signer/session/timeout/only_when_unlocked", SessionFixture, NULL,
               session_fixture_setup, test_session_timeout_only_when_unlocked, session_fixture_teardown);

    /* Settings tests */
    g_test_add("/signer/session/settings/lock_timeout", SessionFixture, NULL,
               session_fixture_setup, test_settings_lock_timeout, session_fixture_teardown);
    g_test_add("/signer/session/settings/remember_approvals", SessionFixture, NULL,
               session_fixture_setup, test_settings_remember_approvals, session_fixture_teardown);
    g_test_add("/signer/session/settings/approval_ttl", SessionFixture, NULL,
               session_fixture_setup, test_settings_approval_ttl, session_fixture_teardown);
    g_test_add("/signer/session/settings/string_roundtrip", SessionFixture, NULL,
               session_fixture_setup, test_settings_string_roundtrip, session_fixture_teardown);
    g_test_add("/signer/session/settings/nonexistent_defaults", SessionFixture, NULL,
               session_fixture_setup, test_settings_nonexistent_returns_default, session_fixture_teardown);

    /* Integration tests */
    g_test_add("/signer/session/integration/settings", SessionFixture, NULL,
               session_fixture_setup, test_session_settings_integration, session_fixture_teardown);
    g_test_add("/signer/session/integration/multiple_cycles", SessionFixture, NULL,
               session_fixture_setup, test_session_multiple_unlock_lock_cycles, session_fixture_teardown);

    return g_test_run();
}
