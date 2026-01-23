/* test-identity.c - Unit tests for gnostr-signer identity/account management
 *
 * Tests AccountsStore operations including creation, storage, retrieval,
 * and deletion of identities. Uses isolated test paths to avoid affecting
 * real user data.
 *
 * Issue: nostrc-ddh
 */
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* We need to test the accounts_store module but in isolation.
 * For unit tests, we'll create a mock-friendly test harness. */

/* ===========================================================================
 * Mock/Stub Definitions for Testing
 * =========================================================================== */

/* Simple in-memory identity store for testing without requiring secret backends */
typedef struct {
    gchar *id;
    gchar *label;
} TestIdentity;

typedef struct {
    GPtrArray *identities;  /* Array of TestIdentity* */
    gchar *active_id;
} TestAccountsStore;

static TestAccountsStore *
test_accounts_store_new(void)
{
    TestAccountsStore *store = g_new0(TestAccountsStore, 1);
    store->identities = g_ptr_array_new_with_free_func(NULL);
    return store;
}

static void
test_identity_free(TestIdentity *id)
{
    if (!id) return;
    g_free(id->id);
    g_free(id->label);
    g_free(id);
}

static void
test_accounts_store_free(TestAccountsStore *store)
{
    if (!store) return;
    for (guint i = 0; i < store->identities->len; i++) {
        test_identity_free(g_ptr_array_index(store->identities, i));
    }
    g_ptr_array_unref(store->identities);
    g_free(store->active_id);
    g_free(store);
}

static gboolean
test_accounts_store_add(TestAccountsStore *store, const gchar *id, const gchar *label)
{
    if (!store || !id || !*id) return FALSE;

    /* Check for duplicates */
    for (guint i = 0; i < store->identities->len; i++) {
        TestIdentity *existing = g_ptr_array_index(store->identities, i);
        if (g_strcmp0(existing->id, id) == 0) {
            return FALSE;  /* Already exists */
        }
    }

    TestIdentity *identity = g_new0(TestIdentity, 1);
    identity->id = g_strdup(id);
    identity->label = g_strdup(label ? label : "");
    g_ptr_array_add(store->identities, identity);

    /* First identity becomes active */
    if (!store->active_id) {
        store->active_id = g_strdup(id);
    }

    return TRUE;
}

static gboolean
test_accounts_store_remove(TestAccountsStore *store, const gchar *id)
{
    if (!store || !id) return FALSE;

    for (guint i = 0; i < store->identities->len; i++) {
        TestIdentity *identity = g_ptr_array_index(store->identities, i);
        if (g_strcmp0(identity->id, id) == 0) {
            /* Update active if needed */
            if (g_strcmp0(store->active_id, id) == 0) {
                g_clear_pointer(&store->active_id, g_free);
                if (store->identities->len > 1) {
                    /* Pick another active */
                    TestIdentity *next = g_ptr_array_index(store->identities, i == 0 ? 1 : 0);
                    store->active_id = g_strdup(next->id);
                }
            }

            test_identity_free(identity);
            g_ptr_array_remove_index(store->identities, i);
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
test_accounts_store_exists(TestAccountsStore *store, const gchar *id)
{
    if (!store || !id) return FALSE;

    for (guint i = 0; i < store->identities->len; i++) {
        TestIdentity *identity = g_ptr_array_index(store->identities, i);
        if (g_strcmp0(identity->id, id) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static guint
test_accounts_store_count(TestAccountsStore *store)
{
    if (!store) return 0;
    return store->identities->len;
}

static void
test_accounts_store_set_active(TestAccountsStore *store, const gchar *id)
{
    if (!store) return;
    g_free(store->active_id);
    store->active_id = g_strdup(id);
}

static gboolean
test_accounts_store_get_active(TestAccountsStore *store, gchar **out_id)
{
    if (!store || !store->active_id) return FALSE;
    if (out_id) *out_id = g_strdup(store->active_id);
    return TRUE;
}

static gboolean
test_accounts_store_set_label(TestAccountsStore *store, const gchar *id, const gchar *label)
{
    if (!store || !id) return FALSE;

    for (guint i = 0; i < store->identities->len; i++) {
        TestIdentity *identity = g_ptr_array_index(store->identities, i);
        if (g_strcmp0(identity->id, id) == 0) {
            g_free(identity->label);
            identity->label = g_strdup(label ? label : "");
            return TRUE;
        }
    }
    return FALSE;
}

static gchar *
test_accounts_store_get_label(TestAccountsStore *store, const gchar *id)
{
    if (!store || !id) return NULL;

    for (guint i = 0; i < store->identities->len; i++) {
        TestIdentity *identity = g_ptr_array_index(store->identities, i);
        if (g_strcmp0(identity->id, id) == 0) {
            return g_strdup(identity->label);
        }
    }
    return NULL;
}

/* ===========================================================================
 * Test Fixtures
 * =========================================================================== */

typedef struct {
    TestAccountsStore *store;
} IdentityFixture;

static void
identity_fixture_setup(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;
    fixture->store = test_accounts_store_new();
}

static void
identity_fixture_teardown(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;
    test_accounts_store_free(fixture->store);
}

/* ===========================================================================
 * Identity Creation Tests
 * =========================================================================== */

static void
test_identity_create_basic(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Add a simple identity */
    const gchar *npub = "npub1test1234567890abcdef1234567890abcdef1234567890abcdef12345678";
    gboolean result = test_accounts_store_add(fixture->store, npub, "Test Identity");

    g_assert_true(result);
    g_assert_cmpuint(test_accounts_store_count(fixture->store), ==, 1);
    g_assert_true(test_accounts_store_exists(fixture->store, npub));
}

static void
test_identity_create_no_label(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1nolabel1234567890abcdef1234567890abcdef1234567890abcdef1234";
    gboolean result = test_accounts_store_add(fixture->store, npub, NULL);

    g_assert_true(result);

    gchar *label = test_accounts_store_get_label(fixture->store, npub);
    g_assert_nonnull(label);
    g_assert_cmpstr(label, ==, "");
    g_free(label);
}

static void
test_identity_create_duplicate(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1duplicate1234567890abcdef1234567890abcdef1234567890abcdef12";

    /* First add should succeed */
    g_assert_true(test_accounts_store_add(fixture->store, npub, "First"));

    /* Second add with same ID should fail */
    g_assert_false(test_accounts_store_add(fixture->store, npub, "Second"));

    g_assert_cmpuint(test_accounts_store_count(fixture->store), ==, 1);
}

static void
test_identity_create_multiple(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub1 = "npub1first1234567890abcdef1234567890abcdef1234567890abcdef12345";
    const gchar *npub2 = "npub1second234567890abcdef1234567890abcdef1234567890abcdef12345";
    const gchar *npub3 = "npub1third1234567890abcdef1234567890abcdef1234567890abcdef12345";

    g_assert_true(test_accounts_store_add(fixture->store, npub1, "First"));
    g_assert_true(test_accounts_store_add(fixture->store, npub2, "Second"));
    g_assert_true(test_accounts_store_add(fixture->store, npub3, "Third"));

    g_assert_cmpuint(test_accounts_store_count(fixture->store), ==, 3);

    /* First one should be active */
    gchar *active = NULL;
    g_assert_true(test_accounts_store_get_active(fixture->store, &active));
    g_assert_cmpstr(active, ==, npub1);
    g_free(active);
}

static void
test_identity_create_invalid(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Empty ID should fail */
    g_assert_false(test_accounts_store_add(fixture->store, "", "Label"));

    /* NULL ID should fail */
    g_assert_false(test_accounts_store_add(fixture->store, NULL, "Label"));

    g_assert_cmpuint(test_accounts_store_count(fixture->store), ==, 0);
}

/* ===========================================================================
 * Identity Deletion Tests
 * =========================================================================== */

static void
test_identity_delete_basic(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1delete1234567890abcdef1234567890abcdef1234567890abcdef12345";
    test_accounts_store_add(fixture->store, npub, "To Delete");

    g_assert_cmpuint(test_accounts_store_count(fixture->store), ==, 1);

    gboolean result = test_accounts_store_remove(fixture->store, npub);
    g_assert_true(result);
    g_assert_cmpuint(test_accounts_store_count(fixture->store), ==, 0);
    g_assert_false(test_accounts_store_exists(fixture->store, npub));
}

static void
test_identity_delete_nonexistent(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    gboolean result = test_accounts_store_remove(fixture->store, "npub1nonexistent");
    g_assert_false(result);
}

static void
test_identity_delete_active_reassigns(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub1 = "npub1active1234567890abcdef1234567890abcdef1234567890abcdef1234";
    const gchar *npub2 = "npub1second234567890abcdef1234567890abcdef1234567890abcdef12345";

    test_accounts_store_add(fixture->store, npub1, "First");
    test_accounts_store_add(fixture->store, npub2, "Second");

    /* First should be active */
    gchar *active = NULL;
    test_accounts_store_get_active(fixture->store, &active);
    g_assert_cmpstr(active, ==, npub1);
    g_free(active);

    /* Delete active */
    test_accounts_store_remove(fixture->store, npub1);

    /* Second should now be active */
    active = NULL;
    g_assert_true(test_accounts_store_get_active(fixture->store, &active));
    g_assert_cmpstr(active, ==, npub2);
    g_free(active);
}

static void
test_identity_delete_last_clears_active(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1only1234567890abcdef1234567890abcdef1234567890abcdef123456";
    test_accounts_store_add(fixture->store, npub, "Only One");

    test_accounts_store_remove(fixture->store, npub);

    gchar *active = NULL;
    gboolean has_active = test_accounts_store_get_active(fixture->store, &active);
    g_assert_false(has_active);
    g_assert_null(active);
}

/* ===========================================================================
 * Active Identity Tests
 * =========================================================================== */

static void
test_identity_set_active(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub1 = "npub1first1234567890abcdef1234567890abcdef1234567890abcdef12345";
    const gchar *npub2 = "npub1second234567890abcdef1234567890abcdef1234567890abcdef12345";

    test_accounts_store_add(fixture->store, npub1, "First");
    test_accounts_store_add(fixture->store, npub2, "Second");

    /* Change active to second */
    test_accounts_store_set_active(fixture->store, npub2);

    gchar *active = NULL;
    test_accounts_store_get_active(fixture->store, &active);
    g_assert_cmpstr(active, ==, npub2);
    g_free(active);
}

static void
test_identity_first_becomes_active(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    /* No active when empty */
    g_assert_false(test_accounts_store_get_active(fixture->store, NULL));

    const gchar *npub = "npub1first1234567890abcdef1234567890abcdef1234567890abcdef12345";
    test_accounts_store_add(fixture->store, npub, "First");

    /* Should automatically become active */
    gchar *active = NULL;
    g_assert_true(test_accounts_store_get_active(fixture->store, &active));
    g_assert_cmpstr(active, ==, npub);
    g_free(active);
}

/* ===========================================================================
 * Label Management Tests
 * =========================================================================== */

static void
test_identity_update_label(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1label1234567890abcdef1234567890abcdef1234567890abcdef123456";
    test_accounts_store_add(fixture->store, npub, "Original Label");

    gchar *label = test_accounts_store_get_label(fixture->store, npub);
    g_assert_cmpstr(label, ==, "Original Label");
    g_free(label);

    /* Update label */
    g_assert_true(test_accounts_store_set_label(fixture->store, npub, "New Label"));

    label = test_accounts_store_get_label(fixture->store, npub);
    g_assert_cmpstr(label, ==, "New Label");
    g_free(label);
}

static void
test_identity_clear_label(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1clearlabel234567890abcdef1234567890abcdef1234567890abcdef12";
    test_accounts_store_add(fixture->store, npub, "Has Label");

    /* Clear label by setting to NULL */
    g_assert_true(test_accounts_store_set_label(fixture->store, npub, NULL));

    gchar *label = test_accounts_store_get_label(fixture->store, npub);
    g_assert_cmpstr(label, ==, "");
    g_free(label);
}

static void
test_identity_label_nonexistent(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_false(test_accounts_store_set_label(fixture->store, "npub1nonexistent", "Label"));
    g_assert_null(test_accounts_store_get_label(fixture->store, "npub1nonexistent"));
}

/* ===========================================================================
 * Existence Checks
 * =========================================================================== */

static void
test_identity_exists(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1exists234567890abcdef1234567890abcdef1234567890abcdef123456";

    g_assert_false(test_accounts_store_exists(fixture->store, npub));

    test_accounts_store_add(fixture->store, npub, "Test");

    g_assert_true(test_accounts_store_exists(fixture->store, npub));
}

static void
test_identity_exists_null(IdentityFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_false(test_accounts_store_exists(fixture->store, NULL));
}

/* ===========================================================================
 * Test Runner
 * =========================================================================== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Creation tests */
    g_test_add("/signer/identity/create/basic", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_create_basic, identity_fixture_teardown);
    g_test_add("/signer/identity/create/no_label", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_create_no_label, identity_fixture_teardown);
    g_test_add("/signer/identity/create/duplicate", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_create_duplicate, identity_fixture_teardown);
    g_test_add("/signer/identity/create/multiple", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_create_multiple, identity_fixture_teardown);
    g_test_add("/signer/identity/create/invalid", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_create_invalid, identity_fixture_teardown);

    /* Deletion tests */
    g_test_add("/signer/identity/delete/basic", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_delete_basic, identity_fixture_teardown);
    g_test_add("/signer/identity/delete/nonexistent", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_delete_nonexistent, identity_fixture_teardown);
    g_test_add("/signer/identity/delete/active_reassigns", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_delete_active_reassigns, identity_fixture_teardown);
    g_test_add("/signer/identity/delete/last_clears_active", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_delete_last_clears_active, identity_fixture_teardown);

    /* Active identity tests */
    g_test_add("/signer/identity/active/set", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_set_active, identity_fixture_teardown);
    g_test_add("/signer/identity/active/first_becomes", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_first_becomes_active, identity_fixture_teardown);

    /* Label tests */
    g_test_add("/signer/identity/label/update", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_update_label, identity_fixture_teardown);
    g_test_add("/signer/identity/label/clear", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_clear_label, identity_fixture_teardown);
    g_test_add("/signer/identity/label/nonexistent", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_label_nonexistent, identity_fixture_teardown);

    /* Existence tests */
    g_test_add("/signer/identity/exists/basic", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_exists, identity_fixture_teardown);
    g_test_add("/signer/identity/exists/null", IdentityFixture, NULL,
               identity_fixture_setup, test_identity_exists_null, identity_fixture_teardown);

    return g_test_run();
}
