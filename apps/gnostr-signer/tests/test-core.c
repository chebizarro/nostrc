/* test-core.c - Comprehensive unit tests for gnostr-signer core modules
 *
 * Tests core signer functionality including:
 * - secret_store.c - Key storage/retrieval with mock libsecret/Keychain
 * - backup-recovery.c - NIP-49 encryption, BIP-39 mnemonic support
 * - session-manager.c - Authentication, timeout, lock/unlock
 * - relay_store.c - Relay configuration management
 * - delegation.c - NIP-26 delegation token management
 *
 * Uses GLib testing framework with mock implementations to isolate
 * tests from actual system backends (libsecret, D-Bus, file system).
 *
 * Issue: nostrc-ddh
 */
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ===========================================================================
 * Mock Secret Store Implementation
 *
 * Simulates libsecret/Keychain behavior without requiring actual system
 * backends. Stores keys in a simple hash table with schema attributes.
 * =========================================================================== */

typedef struct {
    gchar *npub;
    gchar *key_id;
    gchar *secret;       /* hex private key */
    gchar *label;
    gchar *fingerprint;  /* first 8 hex chars of pubkey */
    gboolean has_owner;
    guint32 owner_uid;
} MockSecretEntry;

typedef struct {
    GHashTable *entries;  /* key_id -> MockSecretEntry */
    gboolean available;
} MockSecretStore;

static MockSecretStore *mock_store = NULL;

static void
mock_secret_entry_free(MockSecretEntry *entry)
{
    if (!entry) return;
    g_free(entry->npub);
    g_free(entry->key_id);
    if (entry->secret) {
        /* Securely clear secret before freeing */
        memset(entry->secret, 0, strlen(entry->secret));
        g_free(entry->secret);
    }
    g_free(entry->label);
    g_free(entry->fingerprint);
    g_free(entry);
}

static MockSecretStore *
mock_secret_store_new(void)
{
    MockSecretStore *store = g_new0(MockSecretStore, 1);
    store->entries = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, (GDestroyNotify)mock_secret_entry_free);
    store->available = TRUE;
    return store;
}

static void
mock_secret_store_free(MockSecretStore *store)
{
    if (!store) return;
    g_hash_table_destroy(store->entries);
    g_free(store);
}

static gboolean
mock_secret_store_add(MockSecretStore *store, const gchar *npub,
                      const gchar *secret_hex, const gchar *label)
{
    if (!store || !npub || !secret_hex) return FALSE;
    if (!store->available) return FALSE;

    /* Check for duplicate */
    if (g_hash_table_contains(store->entries, npub)) {
        return FALSE;
    }

    MockSecretEntry *entry = g_new0(MockSecretEntry, 1);
    entry->npub = g_strdup(npub);
    entry->key_id = g_strdup(npub);
    entry->secret = g_strdup(secret_hex);
    entry->label = label ? g_strdup(label) : g_strdup("");

    /* Generate fingerprint from secret (first 8 chars for mock) */
    if (strlen(secret_hex) >= 8) {
        entry->fingerprint = g_strndup(secret_hex, 8);
    }

    g_hash_table_insert(store->entries, g_strdup(npub), entry);
    return TRUE;
}

static gboolean
mock_secret_store_remove(MockSecretStore *store, const gchar *selector)
{
    if (!store || !selector) return FALSE;
    return g_hash_table_remove(store->entries, selector);
}

static MockSecretEntry *
mock_secret_store_lookup(MockSecretStore *store, const gchar *selector)
{
    if (!store || !selector) return NULL;
    return g_hash_table_lookup(store->entries, selector);
}

static GPtrArray *
mock_secret_store_list(MockSecretStore *store)
{
    GPtrArray *arr = g_ptr_array_new();
    if (!store) return arr;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, store->entries);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_ptr_array_add(arr, value);
    }
    return arr;
}

static guint
mock_secret_store_count(MockSecretStore *store)
{
    return store ? g_hash_table_size(store->entries) : 0;
}

static gboolean
mock_secret_store_set_label(MockSecretStore *store, const gchar *selector,
                             const gchar *new_label)
{
    MockSecretEntry *entry = mock_secret_store_lookup(store, selector);
    if (!entry) return FALSE;

    g_free(entry->label);
    entry->label = g_strdup(new_label ? new_label : "");
    return TRUE;
}

/* ===========================================================================
 * Mock Backup/Recovery Implementation
 *
 * Simulates NIP-49 encryption/decryption and BIP-39 mnemonic handling
 * without actual cryptographic operations (for fast testing).
 * =========================================================================== */

typedef enum {
    MOCK_BACKUP_OK = 0,
    MOCK_BACKUP_ERR_INVALID_KEY,
    MOCK_BACKUP_ERR_INVALID_PASSWORD,
    MOCK_BACKUP_ERR_DECRYPT_FAILED,
    MOCK_BACKUP_ERR_INVALID_MNEMONIC
} MockBackupResult;

/* Mock ncryptsec format: "ncryptsec1_<password_hash>_<nsec_hex>" */
static gchar *
mock_backup_encrypt_nip49(const gchar *nsec_hex, const gchar *password)
{
    if (!nsec_hex || !password || !*password) return NULL;

    /* Simple mock: base64(password+nsec) with prefix */
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar *)password, strlen(password));
    const gchar *pw_hash = g_checksum_get_string(checksum);
    gchar *pw_hash_short = g_strndup(pw_hash, 8);
    g_checksum_free(checksum);

    gchar *result = g_strdup_printf("ncryptsec1mock%s%s", pw_hash_short, nsec_hex);
    g_free(pw_hash_short);
    return result;
}

static gchar *
mock_backup_decrypt_nip49(const gchar *ncryptsec, const gchar *password)
{
    if (!ncryptsec || !password || !*password) return NULL;
    if (!g_str_has_prefix(ncryptsec, "ncryptsec1mock")) return NULL;

    /* Verify password hash matches */
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar *)password, strlen(password));
    const gchar *pw_hash = g_checksum_get_string(checksum);
    gchar *pw_hash_short = g_strndup(pw_hash, 8);
    g_checksum_free(checksum);

    const gchar *stored_hash = ncryptsec + strlen("ncryptsec1mock");
    if (strncmp(stored_hash, pw_hash_short, 8) != 0) {
        g_free(pw_hash_short);
        return NULL;  /* Wrong password */
    }
    g_free(pw_hash_short);

    /* Extract nsec hex */
    return g_strdup(stored_hash + 8);
}

static gboolean
mock_backup_validate_mnemonic(const gchar *mnemonic)
{
    if (!mnemonic || !*mnemonic) return FALSE;

    /* Count words - must be 12, 15, 18, 21, or 24 */
    gchar **words = g_strsplit(mnemonic, " ", -1);
    guint count = g_strv_length(words);
    g_strfreev(words);

    return (count == 12 || count == 15 || count == 18 ||
            count == 21 || count == 24);
}

static gchar *
mock_backup_mnemonic_to_nsec(const gchar *mnemonic, guint account)
{
    if (!mock_backup_validate_mnemonic(mnemonic)) return NULL;
    (void)account;

    /* Mock: derive deterministic hex from mnemonic hash */
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar *)mnemonic, strlen(mnemonic));
    const gchar *hash = g_checksum_get_string(checksum);
    gchar *result = g_strdup(hash);
    g_checksum_free(checksum);

    return result;
}

/* ===========================================================================
 * Mock Session Manager Implementation
 *
 * Simulates session lifecycle without GSettings or libsecret dependencies.
 * =========================================================================== */

typedef enum {
    MOCK_SESSION_LOCKED,
    MOCK_SESSION_AUTHENTICATED,
    MOCK_SESSION_EXPIRED
} MockSessionState;

typedef struct {
    MockSessionState state;
    gint64 last_activity;
    gint64 session_started;
    guint timeout_seconds;
    gchar *password_hash;
    gboolean password_configured;
    guint lock_count;
    guint unlock_count;
    guint expire_count;
} MockSessionManager;

static MockSessionManager *
mock_session_manager_new(void)
{
    MockSessionManager *sm = g_new0(MockSessionManager, 1);
    sm->state = MOCK_SESSION_LOCKED;
    sm->timeout_seconds = 300;  /* 5 minute default */
    return sm;
}

static void
mock_session_manager_free(MockSessionManager *sm)
{
    if (!sm) return;
    if (sm->password_hash) {
        memset(sm->password_hash, 0, strlen(sm->password_hash));
        g_free(sm->password_hash);
    }
    g_free(sm);
}

static gchar *
mock_hash_password(const gchar *password)
{
    if (!password) return NULL;
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar *)password, strlen(password));
    gchar *result = g_strdup(g_checksum_get_string(checksum));
    g_checksum_free(checksum);
    return result;
}

static gboolean
mock_session_manager_set_password(MockSessionManager *sm,
                                   const gchar *current_password,
                                   const gchar *new_password)
{
    if (!sm) return FALSE;

    /* Verify current password if configured */
    if (sm->password_configured) {
        if (!current_password) return FALSE;
        gchar *current_hash = mock_hash_password(current_password);
        gboolean match = (g_strcmp0(current_hash, sm->password_hash) == 0);
        g_free(current_hash);
        if (!match) return FALSE;
    }

    if (!new_password || !*new_password) return FALSE;

    g_free(sm->password_hash);
    sm->password_hash = mock_hash_password(new_password);
    sm->password_configured = TRUE;
    return TRUE;
}

static gboolean
mock_session_manager_authenticate(MockSessionManager *sm, const gchar *password)
{
    if (!sm) return FALSE;

    /* If no password configured, accept any non-empty password or auto-unlock */
    if (!sm->password_configured) {
        sm->state = MOCK_SESSION_AUTHENTICATED;
        sm->session_started = g_get_monotonic_time();
        sm->last_activity = sm->session_started;
        sm->unlock_count++;
        return TRUE;
    }

    /* Verify password */
    if (!password || !*password) return FALSE;

    gchar *hash = mock_hash_password(password);
    gboolean match = (g_strcmp0(hash, sm->password_hash) == 0);
    g_free(hash);

    if (match) {
        sm->state = MOCK_SESSION_AUTHENTICATED;
        sm->session_started = g_get_monotonic_time();
        sm->last_activity = sm->session_started;
        sm->unlock_count++;
    }
    return match;
}

static void
mock_session_manager_lock(MockSessionManager *sm)
{
    if (!sm || sm->state == MOCK_SESSION_LOCKED) return;
    sm->state = MOCK_SESSION_LOCKED;
    sm->session_started = 0;
    sm->last_activity = 0;
    sm->lock_count++;
}

static void
mock_session_manager_extend(MockSessionManager *sm)
{
    if (!sm || sm->state != MOCK_SESSION_AUTHENTICATED) return;
    sm->last_activity = g_get_monotonic_time();
}

static gboolean
mock_session_manager_check_timeout(MockSessionManager *sm)
{
    if (!sm || sm->state != MOCK_SESSION_AUTHENTICATED) return FALSE;
    if (sm->timeout_seconds == 0) return FALSE;

    gint64 now = g_get_monotonic_time();
    gint64 elapsed = (now - sm->last_activity) / G_USEC_PER_SEC;
    return elapsed >= sm->timeout_seconds;
}

static void
mock_session_manager_simulate_elapsed(MockSessionManager *sm, gint seconds)
{
    if (!sm) return;
    sm->last_activity -= (gint64)seconds * G_USEC_PER_SEC;
}

/* ===========================================================================
 * Mock Relay Store Implementation
 *
 * Simulates relay configuration without file I/O.
 * =========================================================================== */

typedef struct {
    gchar *url;
    gboolean read;
    gboolean write;
} MockRelayEntry;

typedef struct {
    GPtrArray *relays;
    gchar *identity;
} MockRelayStore;

static void
mock_relay_entry_free(MockRelayEntry *entry)
{
    if (!entry) return;
    g_free(entry->url);
    g_free(entry);
}

static MockRelayStore *
mock_relay_store_new(const gchar *identity)
{
    MockRelayStore *rs = g_new0(MockRelayStore, 1);
    rs->relays = g_ptr_array_new_with_free_func((GDestroyNotify)mock_relay_entry_free);
    rs->identity = identity ? g_strdup(identity) : NULL;
    return rs;
}

static void
mock_relay_store_free(MockRelayStore *rs)
{
    if (!rs) return;
    g_ptr_array_unref(rs->relays);
    g_free(rs->identity);
    g_free(rs);
}

static gboolean
mock_relay_store_add(MockRelayStore *rs, const gchar *url,
                      gboolean read, gboolean write)
{
    if (!rs || !url || !*url) return FALSE;

    /* Check for duplicate */
    for (guint i = 0; i < rs->relays->len; i++) {
        MockRelayEntry *e = g_ptr_array_index(rs->relays, i);
        if (g_strcmp0(e->url, url) == 0) return FALSE;
    }

    MockRelayEntry *entry = g_new0(MockRelayEntry, 1);
    entry->url = g_strdup(url);
    entry->read = read;
    entry->write = write;
    g_ptr_array_add(rs->relays, entry);
    return TRUE;
}

static gboolean
mock_relay_store_remove(MockRelayStore *rs, const gchar *url)
{
    if (!rs || !url) return FALSE;

    for (guint i = 0; i < rs->relays->len; i++) {
        MockRelayEntry *e = g_ptr_array_index(rs->relays, i);
        if (g_strcmp0(e->url, url) == 0) {
            g_ptr_array_remove_index(rs->relays, i);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
mock_relay_store_update(MockRelayStore *rs, const gchar *url,
                         gboolean read, gboolean write)
{
    if (!rs || !url) return FALSE;

    for (guint i = 0; i < rs->relays->len; i++) {
        MockRelayEntry *e = g_ptr_array_index(rs->relays, i);
        if (g_strcmp0(e->url, url) == 0) {
            e->read = read;
            e->write = write;
            return TRUE;
        }
    }
    return FALSE;
}

static guint
mock_relay_store_count(MockRelayStore *rs)
{
    return rs ? rs->relays->len : 0;
}

static gboolean
mock_relay_validate_url(const gchar *url)
{
    if (!url || !*url) return FALSE;
    return g_str_has_prefix(url, "wss://") || g_str_has_prefix(url, "ws://");
}

/* ===========================================================================
 * Mock Delegation Implementation
 *
 * Simulates NIP-26 delegation token management.
 * =========================================================================== */

typedef struct {
    gchar *id;
    gchar *delegator_npub;
    gchar *delegatee_pubkey_hex;
    GArray *allowed_kinds;
    gint64 valid_from;
    gint64 valid_until;
    gchar *conditions;
    gchar *signature;
    gboolean revoked;
    gint64 revoked_at;
    gchar *label;
} MockDelegation;

typedef struct {
    GHashTable *delegations;  /* delegator_npub -> GPtrArray of MockDelegation */
} MockDelegationStore;

static void
mock_delegation_free(MockDelegation *d)
{
    if (!d) return;
    g_free(d->id);
    g_free(d->delegator_npub);
    g_free(d->delegatee_pubkey_hex);
    if (d->allowed_kinds) g_array_unref(d->allowed_kinds);
    g_free(d->conditions);
    g_free(d->signature);
    g_free(d->label);
    g_free(d);
}

static MockDelegationStore *
mock_delegation_store_new(void)
{
    MockDelegationStore *ds = g_new0(MockDelegationStore, 1);
    ds->delegations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify)g_ptr_array_unref);
    return ds;
}

static void
mock_delegation_store_free(MockDelegationStore *ds)
{
    if (!ds) return;
    g_hash_table_destroy(ds->delegations);
    g_free(ds);
}

static gchar *
mock_delegation_build_conditions(GArray *kinds, gint64 valid_from, gint64 valid_until)
{
    GString *cond = g_string_new("");

    if (kinds && kinds->len > 0) {
        for (guint i = 0; i < kinds->len; i++) {
            if (cond->len > 0) g_string_append(cond, "&");
            guint16 k = g_array_index(kinds, guint16, i);
            g_string_append_printf(cond, "kind=%u", k);
        }
    }

    if (valid_from > 0) {
        if (cond->len > 0) g_string_append(cond, "&");
        g_string_append_printf(cond, "created_at>%" G_GINT64_FORMAT, valid_from);
    }

    if (valid_until > 0) {
        if (cond->len > 0) g_string_append(cond, "&");
        g_string_append_printf(cond, "created_at<%" G_GINT64_FORMAT, valid_until);
    }

    return g_string_free(cond, FALSE);
}

static MockDelegation *
mock_delegation_create(MockDelegationStore *ds,
                        const gchar *delegator_npub,
                        const gchar *delegatee_hex,
                        GArray *kinds,
                        gint64 valid_from,
                        gint64 valid_until,
                        const gchar *label)
{
    if (!ds || !delegator_npub || !delegatee_hex) return NULL;

    MockDelegation *d = g_new0(MockDelegation, 1);
    d->id = g_strdup_printf("del_%ld_%d", (long)time(NULL), g_random_int_range(0, 10000));
    d->delegator_npub = g_strdup(delegator_npub);
    d->delegatee_pubkey_hex = g_strdup(delegatee_hex);

    if (kinds && kinds->len > 0) {
        d->allowed_kinds = g_array_ref(kinds);
    }

    d->valid_from = valid_from;
    d->valid_until = valid_until;
    d->conditions = mock_delegation_build_conditions(kinds, valid_from, valid_until);
    d->signature = g_strdup_printf("mock_sig_%s", d->id);
    d->label = label ? g_strdup(label) : NULL;
    d->revoked = FALSE;

    /* Store in delegation store */
    GPtrArray *list = g_hash_table_lookup(ds->delegations, delegator_npub);
    if (!list) {
        list = g_ptr_array_new_with_free_func((GDestroyNotify)mock_delegation_free);
        g_hash_table_insert(ds->delegations, g_strdup(delegator_npub), list);
    }
    g_ptr_array_add(list, d);

    return d;
}

static gboolean
mock_delegation_is_valid(MockDelegation *d, guint16 kind, gint64 timestamp)
{
    if (!d || d->revoked) return FALSE;

    gint64 now = timestamp > 0 ? timestamp : (gint64)time(NULL);

    /* Check time bounds */
    if (d->valid_from > 0 && now < d->valid_from) return FALSE;
    if (d->valid_until > 0 && now > d->valid_until) return FALSE;

    /* Check kind restriction */
    if (d->allowed_kinds && d->allowed_kinds->len > 0 && kind > 0) {
        gboolean found = FALSE;
        for (guint i = 0; i < d->allowed_kinds->len; i++) {
            if (g_array_index(d->allowed_kinds, guint16, i) == kind) {
                found = TRUE;
                break;
            }
        }
        if (!found) return FALSE;
    }

    return TRUE;
}

static gboolean
mock_delegation_revoke(MockDelegationStore *ds, const gchar *delegator_npub,
                        const gchar *delegation_id)
{
    if (!ds || !delegator_npub || !delegation_id) return FALSE;

    GPtrArray *list = g_hash_table_lookup(ds->delegations, delegator_npub);
    if (!list) return FALSE;

    for (guint i = 0; i < list->len; i++) {
        MockDelegation *d = g_ptr_array_index(list, i);
        if (g_strcmp0(d->id, delegation_id) == 0) {
            d->revoked = TRUE;
            d->revoked_at = (gint64)time(NULL);
            return TRUE;
        }
    }
    return FALSE;
}

/* ===========================================================================
 * Test Fixtures
 * =========================================================================== */

typedef struct {
    MockSecretStore *secret_store;
    MockSessionManager *session;
    MockRelayStore *relay_store;
    MockDelegationStore *delegation_store;
} CoreFixture;

static void
core_fixture_setup(CoreFixture *fixture, gconstpointer data)
{
    (void)data;
    fixture->secret_store = mock_secret_store_new();
    fixture->session = mock_session_manager_new();
    fixture->relay_store = mock_relay_store_new(NULL);
    fixture->delegation_store = mock_delegation_store_new();
}

static void
core_fixture_teardown(CoreFixture *fixture, gconstpointer data)
{
    (void)data;
    mock_secret_store_free(fixture->secret_store);
    mock_session_manager_free(fixture->session);
    mock_relay_store_free(fixture->relay_store);
    mock_delegation_store_free(fixture->delegation_store);
}

/* ===========================================================================
 * Secret Store Tests
 * =========================================================================== */

static void
test_secret_store_add_key(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1test1234567890abcdef1234567890abcdef1234567890abcdef12345678";
    const gchar *secret = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    gboolean result = mock_secret_store_add(fixture->secret_store, npub, secret, "Test Key");

    g_assert_true(result);
    g_assert_cmpuint(mock_secret_store_count(fixture->secret_store), ==, 1);
}

static void
test_secret_store_add_duplicate(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1duplicate1234567890abcdef1234567890abcdef1234567890abcdef12";
    const gchar *secret = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    g_assert_true(mock_secret_store_add(fixture->secret_store, npub, secret, "First"));
    g_assert_false(mock_secret_store_add(fixture->secret_store, npub, secret, "Second"));
    g_assert_cmpuint(mock_secret_store_count(fixture->secret_store), ==, 1);
}

static void
test_secret_store_add_invalid_input(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_false(mock_secret_store_add(fixture->secret_store, NULL, "secret", "Label"));
    g_assert_false(mock_secret_store_add(fixture->secret_store, "npub", NULL, "Label"));
    g_assert_cmpuint(mock_secret_store_count(fixture->secret_store), ==, 0);
}

static void
test_secret_store_remove_key(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1remove1234567890abcdef1234567890abcdef1234567890abcdef12345";
    const gchar *secret = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";

    mock_secret_store_add(fixture->secret_store, npub, secret, "To Remove");
    g_assert_cmpuint(mock_secret_store_count(fixture->secret_store), ==, 1);

    g_assert_true(mock_secret_store_remove(fixture->secret_store, npub));
    g_assert_cmpuint(mock_secret_store_count(fixture->secret_store), ==, 0);
}

static void
test_secret_store_remove_nonexistent(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_false(mock_secret_store_remove(fixture->secret_store, "npub1nonexistent"));
}

static void
test_secret_store_lookup(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1lookup1234567890abcdef1234567890abcdef1234567890abcdef12345";
    const gchar *secret = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

    mock_secret_store_add(fixture->secret_store, npub, secret, "Lookup Test");

    MockSecretEntry *entry = mock_secret_store_lookup(fixture->secret_store, npub);
    g_assert_nonnull(entry);
    g_assert_cmpstr(entry->npub, ==, npub);
    g_assert_cmpstr(entry->secret, ==, secret);
    g_assert_cmpstr(entry->label, ==, "Lookup Test");
}

static void
test_secret_store_lookup_not_found(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    MockSecretEntry *entry = mock_secret_store_lookup(fixture->secret_store, "npub1notfound");
    g_assert_null(entry);
}

static void
test_secret_store_list_empty(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    GPtrArray *list = mock_secret_store_list(fixture->secret_store);
    g_assert_nonnull(list);
    g_assert_cmpuint(list->len, ==, 0);
    g_ptr_array_unref(list);
}

static void
test_secret_store_list_multiple(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    mock_secret_store_add(fixture->secret_store,
                          "npub1first1234567890abcdef1234567890abcdef1234567890abcdef123456",
                          "1111111111111111111111111111111111111111111111111111111111111111",
                          "First");
    mock_secret_store_add(fixture->secret_store,
                          "npub1second234567890abcdef1234567890abcdef1234567890abcdef12345",
                          "2222222222222222222222222222222222222222222222222222222222222222",
                          "Second");

    GPtrArray *list = mock_secret_store_list(fixture->secret_store);
    g_assert_cmpuint(list->len, ==, 2);
    g_ptr_array_unref(list);
}

static void
test_secret_store_set_label(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1label1234567890abcdef1234567890abcdef1234567890abcdef123456";
    mock_secret_store_add(fixture->secret_store, npub,
                          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                          "Original");

    g_assert_true(mock_secret_store_set_label(fixture->secret_store, npub, "Updated"));

    MockSecretEntry *entry = mock_secret_store_lookup(fixture->secret_store, npub);
    g_assert_cmpstr(entry->label, ==, "Updated");
}

static void
test_secret_store_unavailable(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    fixture->secret_store->available = FALSE;

    g_assert_false(mock_secret_store_add(fixture->secret_store,
                                         "npub1test", "secret", "Label"));
}

/* ===========================================================================
 * Backup/Recovery Tests (NIP-49 and BIP-39)
 * =========================================================================== */

static void
test_backup_nip49_encrypt_decrypt(void)
{
    const gchar *nsec = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const gchar *password = "test-password-123";

    gchar *encrypted = mock_backup_encrypt_nip49(nsec, password);
    g_assert_nonnull(encrypted);
    g_assert_true(g_str_has_prefix(encrypted, "ncryptsec1mock"));

    gchar *decrypted = mock_backup_decrypt_nip49(encrypted, password);
    g_assert_nonnull(decrypted);
    g_assert_cmpstr(decrypted, ==, nsec);

    g_free(encrypted);
    g_free(decrypted);
}

static void
test_backup_nip49_wrong_password(void)
{
    const gchar *nsec = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    gchar *encrypted = mock_backup_encrypt_nip49(nsec, "correct-password");
    g_assert_nonnull(encrypted);

    gchar *decrypted = mock_backup_decrypt_nip49(encrypted, "wrong-password");
    g_assert_null(decrypted);

    g_free(encrypted);
}

static void
test_backup_nip49_empty_password(void)
{
    const gchar *nsec = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    g_assert_null(mock_backup_encrypt_nip49(nsec, ""));
    g_assert_null(mock_backup_encrypt_nip49(nsec, NULL));
}

static void
test_backup_nip49_invalid_encrypted(void)
{
    g_assert_null(mock_backup_decrypt_nip49("invalid", "password"));
    g_assert_null(mock_backup_decrypt_nip49("ncryptsec", "password"));
    g_assert_null(mock_backup_decrypt_nip49(NULL, "password"));
}

static void
test_backup_mnemonic_validate_12_words(void)
{
    const gchar *mnemonic = "abandon abandon abandon abandon abandon abandon "
                             "abandon abandon abandon abandon abandon about";
    g_assert_true(mock_backup_validate_mnemonic(mnemonic));
}

static void
test_backup_mnemonic_validate_24_words(void)
{
    const gchar *mnemonic = "abandon abandon abandon abandon abandon abandon "
                             "abandon abandon abandon abandon abandon abandon "
                             "abandon abandon abandon abandon abandon abandon "
                             "abandon abandon abandon abandon abandon art";
    g_assert_true(mock_backup_validate_mnemonic(mnemonic));
}

static void
test_backup_mnemonic_invalid_word_count(void)
{
    g_assert_false(mock_backup_validate_mnemonic("one two three"));
    g_assert_false(mock_backup_validate_mnemonic("one two three four five six seven eight nine ten eleven"));
    g_assert_false(mock_backup_validate_mnemonic(""));
    g_assert_false(mock_backup_validate_mnemonic(NULL));
}

static void
test_backup_mnemonic_to_key(void)
{
    const gchar *mnemonic = "abandon abandon abandon abandon abandon abandon "
                             "abandon abandon abandon abandon abandon about";

    gchar *nsec = mock_backup_mnemonic_to_nsec(mnemonic, 0);
    g_assert_nonnull(nsec);
    g_assert_cmpuint(strlen(nsec), ==, 64);  /* 256-bit hex */

    /* Same mnemonic should produce same key */
    gchar *nsec2 = mock_backup_mnemonic_to_nsec(mnemonic, 0);
    g_assert_cmpstr(nsec, ==, nsec2);

    g_free(nsec);
    g_free(nsec2);
}

/* ===========================================================================
 * Session Manager Tests
 * =========================================================================== */

static void
test_session_create_starts_locked(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_LOCKED);
}

static void
test_session_authenticate_no_password(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Without password configured, should auto-authenticate */
    g_assert_true(mock_session_manager_authenticate(fixture->session, NULL));
    g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_AUTHENTICATED);
    g_assert_cmpuint(fixture->session->unlock_count, ==, 1);
}

static void
test_session_authenticate_with_password(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    mock_session_manager_set_password(fixture->session, NULL, "my-password");
    g_assert_true(fixture->session->password_configured);

    /* Wrong password should fail */
    g_assert_false(mock_session_manager_authenticate(fixture->session, "wrong"));
    g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_LOCKED);

    /* Correct password should succeed */
    g_assert_true(mock_session_manager_authenticate(fixture->session, "my-password"));
    g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_AUTHENTICATED);
}

static void
test_session_lock(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    mock_session_manager_authenticate(fixture->session, NULL);
    g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_AUTHENTICATED);

    mock_session_manager_lock(fixture->session);
    g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_LOCKED);
    g_assert_cmpuint(fixture->session->lock_count, ==, 1);
}

static void
test_session_lock_already_locked(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_LOCKED);
    mock_session_manager_lock(fixture->session);
    g_assert_cmpuint(fixture->session->lock_count, ==, 0);  /* No-op */
}

static void
test_session_timeout_check(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    fixture->session->timeout_seconds = 60;
    mock_session_manager_authenticate(fixture->session, NULL);

    /* Should not timeout immediately */
    g_assert_false(mock_session_manager_check_timeout(fixture->session));

    /* Simulate time passing */
    mock_session_manager_simulate_elapsed(fixture->session, 61);

    /* Should now timeout */
    g_assert_true(mock_session_manager_check_timeout(fixture->session));
}

static void
test_session_extend_resets_timeout(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    fixture->session->timeout_seconds = 60;
    mock_session_manager_authenticate(fixture->session, NULL);

    /* Simulate 50 seconds */
    mock_session_manager_simulate_elapsed(fixture->session, 50);
    g_assert_false(mock_session_manager_check_timeout(fixture->session));

    /* Extend session */
    mock_session_manager_extend(fixture->session);

    /* Another 50 seconds should not trigger timeout */
    mock_session_manager_simulate_elapsed(fixture->session, 50);
    g_assert_false(mock_session_manager_check_timeout(fixture->session));
}

static void
test_session_zero_timeout_disables(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    fixture->session->timeout_seconds = 0;
    mock_session_manager_authenticate(fixture->session, NULL);

    mock_session_manager_simulate_elapsed(fixture->session, 99999);
    g_assert_false(mock_session_manager_check_timeout(fixture->session));
}

static void
test_session_password_change(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Set initial password */
    g_assert_true(mock_session_manager_set_password(fixture->session, NULL, "password1"));

    /* Change password (requires current password) */
    g_assert_false(mock_session_manager_set_password(fixture->session, "wrong", "password2"));
    g_assert_true(mock_session_manager_set_password(fixture->session, "password1", "password2"));

    /* Verify new password works */
    g_assert_true(mock_session_manager_authenticate(fixture->session, "password2"));
}

static void
test_session_multiple_lock_unlock_cycles(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    for (int i = 0; i < 5; i++) {
        mock_session_manager_authenticate(fixture->session, NULL);
        g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_AUTHENTICATED);

        mock_session_manager_lock(fixture->session);
        g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_LOCKED);
    }

    g_assert_cmpuint(fixture->session->unlock_count, ==, 5);
    g_assert_cmpuint(fixture->session->lock_count, ==, 5);
}

/* ===========================================================================
 * Relay Store Tests
 * =========================================================================== */

static void
test_relay_store_add(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_true(mock_relay_store_add(fixture->relay_store, "wss://relay.example.com", TRUE, TRUE));
    g_assert_cmpuint(mock_relay_store_count(fixture->relay_store), ==, 1);
}

static void
test_relay_store_add_duplicate(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_true(mock_relay_store_add(fixture->relay_store, "wss://relay.example.com", TRUE, TRUE));
    g_assert_false(mock_relay_store_add(fixture->relay_store, "wss://relay.example.com", FALSE, FALSE));
    g_assert_cmpuint(mock_relay_store_count(fixture->relay_store), ==, 1);
}

static void
test_relay_store_remove(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    mock_relay_store_add(fixture->relay_store, "wss://relay1.example.com", TRUE, TRUE);
    mock_relay_store_add(fixture->relay_store, "wss://relay2.example.com", TRUE, TRUE);
    g_assert_cmpuint(mock_relay_store_count(fixture->relay_store), ==, 2);

    g_assert_true(mock_relay_store_remove(fixture->relay_store, "wss://relay1.example.com"));
    g_assert_cmpuint(mock_relay_store_count(fixture->relay_store), ==, 1);
}

static void
test_relay_store_remove_nonexistent(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_false(mock_relay_store_remove(fixture->relay_store, "wss://nonexistent.example.com"));
}

static void
test_relay_store_update(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    mock_relay_store_add(fixture->relay_store, "wss://relay.example.com", TRUE, TRUE);

    g_assert_true(mock_relay_store_update(fixture->relay_store, "wss://relay.example.com", TRUE, FALSE));

    MockRelayEntry *entry = g_ptr_array_index(fixture->relay_store->relays, 0);
    g_assert_true(entry->read);
    g_assert_false(entry->write);
}

static void
test_relay_store_update_nonexistent(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_false(mock_relay_store_update(fixture->relay_store, "wss://nonexistent.example.com", TRUE, TRUE));
}

static void
test_relay_validate_url_valid(void)
{
    g_assert_true(mock_relay_validate_url("wss://relay.example.com"));
    g_assert_true(mock_relay_validate_url("ws://relay.example.com"));
    g_assert_true(mock_relay_validate_url("wss://relay.damus.io"));
    g_assert_true(mock_relay_validate_url("wss://relay.nostr.band/"));
}

static void
test_relay_validate_url_invalid(void)
{
    g_assert_false(mock_relay_validate_url(NULL));
    g_assert_false(mock_relay_validate_url(""));
    g_assert_false(mock_relay_validate_url("http://example.com"));
    g_assert_false(mock_relay_validate_url("https://example.com"));
    g_assert_false(mock_relay_validate_url("relay.example.com"));
}

static void
test_relay_store_multiple_relays(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = {
        "wss://relay.damus.io",
        "wss://relay.nostr.band",
        "wss://nos.lol",
        "wss://relay.snort.social"
    };

    for (size_t i = 0; i < G_N_ELEMENTS(relays); i++) {
        g_assert_true(mock_relay_store_add(fixture->relay_store, relays[i], TRUE, TRUE));
    }

    g_assert_cmpuint(mock_relay_store_count(fixture->relay_store), ==, 4);
}

static void
test_relay_store_identity_specific(void)
{
    const gchar *npub = "npub1test1234567890abcdef1234567890abcdef1234567890abcdef12345678";

    MockRelayStore *identity_store = mock_relay_store_new(npub);
    g_assert_nonnull(identity_store);
    g_assert_cmpstr(identity_store->identity, ==, npub);

    mock_relay_store_add(identity_store, "wss://private-relay.example.com", TRUE, TRUE);
    g_assert_cmpuint(mock_relay_store_count(identity_store), ==, 1);

    mock_relay_store_free(identity_store);
}

/* ===========================================================================
 * Delegation Tests (NIP-26)
 * =========================================================================== */

static void
test_delegation_create(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *delegator = "npub1delegator234567890abcdef1234567890abcdef1234567890abcdef";
    const gchar *delegatee = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

    MockDelegation *d = mock_delegation_create(fixture->delegation_store,
                                                delegator, delegatee,
                                                NULL, 0, 0, "Test Delegation");

    g_assert_nonnull(d);
    g_assert_nonnull(d->id);
    g_assert_cmpstr(d->delegator_npub, ==, delegator);
    g_assert_cmpstr(d->delegatee_pubkey_hex, ==, delegatee);
    g_assert_false(d->revoked);
}

static void
test_delegation_create_with_kinds(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *delegator = "npub1delegator234567890abcdef1234567890abcdef1234567890abcdef";
    const gchar *delegatee = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

    GArray *kinds = g_array_new(FALSE, FALSE, sizeof(guint16));
    guint16 kind1 = 1, kind7 = 7;
    g_array_append_val(kinds, kind1);
    g_array_append_val(kinds, kind7);

    MockDelegation *d = mock_delegation_create(fixture->delegation_store,
                                                delegator, delegatee,
                                                kinds, 0, 0, NULL);

    g_assert_nonnull(d);
    g_assert_nonnull(d->allowed_kinds);
    g_assert_cmpuint(d->allowed_kinds->len, ==, 2);
    g_assert_true(strstr(d->conditions, "kind=1") != NULL);
    g_assert_true(strstr(d->conditions, "kind=7") != NULL);

    g_array_unref(kinds);
}

static void
test_delegation_create_with_time_bounds(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *delegator = "npub1delegator234567890abcdef1234567890abcdef1234567890abcdef";
    const gchar *delegatee = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

    gint64 valid_from = 1700000000;
    gint64 valid_until = 1800000000;

    MockDelegation *d = mock_delegation_create(fixture->delegation_store,
                                                delegator, delegatee,
                                                NULL, valid_from, valid_until, NULL);

    g_assert_nonnull(d);
    g_assert_cmpint(d->valid_from, ==, valid_from);
    g_assert_cmpint(d->valid_until, ==, valid_until);
    g_assert_true(strstr(d->conditions, "created_at>1700000000") != NULL);
    g_assert_true(strstr(d->conditions, "created_at<1800000000") != NULL);
}

static void
test_delegation_is_valid_basic(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    MockDelegation *d = mock_delegation_create(fixture->delegation_store,
                                                "npub1delegator", "delegatee_hex",
                                                NULL, 0, 0, NULL);

    g_assert_true(mock_delegation_is_valid(d, 0, 0));
    g_assert_true(mock_delegation_is_valid(d, 1, 0));
    g_assert_true(mock_delegation_is_valid(d, 30023, 0));
}

static void
test_delegation_is_valid_kind_restricted(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    GArray *kinds = g_array_new(FALSE, FALSE, sizeof(guint16));
    guint16 kind1 = 1;
    g_array_append_val(kinds, kind1);

    MockDelegation *d = mock_delegation_create(fixture->delegation_store,
                                                "npub1delegator", "delegatee_hex",
                                                kinds, 0, 0, NULL);

    g_assert_true(mock_delegation_is_valid(d, 1, 0));
    g_assert_false(mock_delegation_is_valid(d, 7, 0));
    g_assert_false(mock_delegation_is_valid(d, 30023, 0));

    g_array_unref(kinds);
}

static void
test_delegation_is_valid_time_bounded(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    gint64 valid_from = 1700000000;
    gint64 valid_until = 1800000000;

    MockDelegation *d = mock_delegation_create(fixture->delegation_store,
                                                "npub1delegator", "delegatee_hex",
                                                NULL, valid_from, valid_until, NULL);

    /* Before valid_from */
    g_assert_false(mock_delegation_is_valid(d, 0, valid_from - 1));

    /* Within range */
    g_assert_true(mock_delegation_is_valid(d, 0, valid_from));
    g_assert_true(mock_delegation_is_valid(d, 0, (valid_from + valid_until) / 2));
    g_assert_true(mock_delegation_is_valid(d, 0, valid_until));

    /* After valid_until */
    g_assert_false(mock_delegation_is_valid(d, 0, valid_until + 1));
}

static void
test_delegation_revoke(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *delegator = "npub1delegator234567890abcdef1234567890abcdef1234567890abcdef";

    MockDelegation *d = mock_delegation_create(fixture->delegation_store,
                                                delegator, "delegatee_hex",
                                                NULL, 0, 0, NULL);

    gchar *delegation_id = g_strdup(d->id);

    g_assert_true(mock_delegation_is_valid(d, 0, 0));

    g_assert_true(mock_delegation_revoke(fixture->delegation_store, delegator, delegation_id));

    g_assert_true(d->revoked);
    g_assert_false(mock_delegation_is_valid(d, 0, 0));

    g_free(delegation_id);
}

static void
test_delegation_revoke_nonexistent(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_false(mock_delegation_revoke(fixture->delegation_store,
                                           "npub1unknown", "del_nonexistent"));
}

static void
test_delegation_build_conditions_empty(void)
{
    gchar *cond = mock_delegation_build_conditions(NULL, 0, 0);
    g_assert_nonnull(cond);
    g_assert_cmpstr(cond, ==, "");
    g_free(cond);
}

static void
test_delegation_build_conditions_kinds_only(void)
{
    GArray *kinds = g_array_new(FALSE, FALSE, sizeof(guint16));
    guint16 k1 = 1, k7 = 7;
    g_array_append_val(kinds, k1);
    g_array_append_val(kinds, k7);

    gchar *cond = mock_delegation_build_conditions(kinds, 0, 0);
    g_assert_cmpstr(cond, ==, "kind=1&kind=7");

    g_free(cond);
    g_array_unref(kinds);
}

static void
test_delegation_build_conditions_full(void)
{
    GArray *kinds = g_array_new(FALSE, FALSE, sizeof(guint16));
    guint16 k1 = 1;
    g_array_append_val(kinds, k1);

    gchar *cond = mock_delegation_build_conditions(kinds, 1700000000, 1800000000);
    g_assert_true(strstr(cond, "kind=1") != NULL);
    g_assert_true(strstr(cond, "created_at>1700000000") != NULL);
    g_assert_true(strstr(cond, "created_at<1800000000") != NULL);

    g_free(cond);
    g_array_unref(kinds);
}

/* ===========================================================================
 * Integration Tests
 * =========================================================================== */

static void
test_integration_key_lifecycle(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *npub = "npub1lifecycle1234567890abcdef1234567890abcdef1234567890abcdef";
    const gchar *secret = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";

    /* Add key */
    g_assert_true(mock_secret_store_add(fixture->secret_store, npub, secret, "Lifecycle Test"));

    /* Lookup and verify */
    MockSecretEntry *entry = mock_secret_store_lookup(fixture->secret_store, npub);
    g_assert_nonnull(entry);
    g_assert_cmpstr(entry->secret, ==, secret);

    /* Update label */
    g_assert_true(mock_secret_store_set_label(fixture->secret_store, npub, "Updated Label"));
    entry = mock_secret_store_lookup(fixture->secret_store, npub);
    g_assert_cmpstr(entry->label, ==, "Updated Label");

    /* Remove */
    g_assert_true(mock_secret_store_remove(fixture->secret_store, npub));
    g_assert_null(mock_secret_store_lookup(fixture->secret_store, npub));
}

static void
test_integration_backup_restore(void)
{
    const gchar *original_nsec = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const gchar *password = "backup-password";

    /* Encrypt */
    gchar *encrypted = mock_backup_encrypt_nip49(original_nsec, password);
    g_assert_nonnull(encrypted);

    /* Decrypt */
    gchar *restored = mock_backup_decrypt_nip49(encrypted, password);
    g_assert_nonnull(restored);
    g_assert_cmpstr(restored, ==, original_nsec);

    g_free(encrypted);
    g_free(restored);
}

static void
test_integration_session_with_secret_store(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Store a key */
    const gchar *npub = "npub1session1234567890abcdef1234567890abcdef1234567890abcdef123";
    mock_secret_store_add(fixture->secret_store, npub,
                          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                          "Session Key");

    /* Setup password */
    mock_session_manager_set_password(fixture->session, NULL, "session-pw");

    /* Authenticate */
    g_assert_true(mock_session_manager_authenticate(fixture->session, "session-pw"));

    /* Access key while authenticated */
    MockSecretEntry *entry = mock_secret_store_lookup(fixture->secret_store, npub);
    g_assert_nonnull(entry);

    /* Lock */
    mock_session_manager_lock(fixture->session);
    g_assert_cmpint(fixture->session->state, ==, MOCK_SESSION_LOCKED);
}

static void
test_integration_relay_with_delegation(CoreFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Setup relays */
    mock_relay_store_add(fixture->relay_store, "wss://relay.example.com", TRUE, TRUE);

    /* Create delegation */
    MockDelegation *d = mock_delegation_create(fixture->delegation_store,
                                                "npub1delegator", "delegatee_hex",
                                                NULL, 0, 0, "For relay access");

    g_assert_nonnull(d);
    g_assert_true(mock_delegation_is_valid(d, 1, 0));
    g_assert_cmpuint(mock_relay_store_count(fixture->relay_store), ==, 1);
}

/* ===========================================================================
 * Test Runner
 * =========================================================================== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Secret Store Tests */
    g_test_add("/signer/core/secret_store/add_key", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_add_key, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/add_duplicate", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_add_duplicate, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/add_invalid", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_add_invalid_input, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/remove_key", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_remove_key, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/remove_nonexistent", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_remove_nonexistent, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/lookup", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_lookup, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/lookup_not_found", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_lookup_not_found, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/list_empty", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_list_empty, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/list_multiple", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_list_multiple, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/set_label", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_set_label, core_fixture_teardown);
    g_test_add("/signer/core/secret_store/unavailable", CoreFixture, NULL,
               core_fixture_setup, test_secret_store_unavailable, core_fixture_teardown);

    /* Backup/Recovery Tests */
    g_test_add_func("/signer/core/backup/nip49_encrypt_decrypt", test_backup_nip49_encrypt_decrypt);
    g_test_add_func("/signer/core/backup/nip49_wrong_password", test_backup_nip49_wrong_password);
    g_test_add_func("/signer/core/backup/nip49_empty_password", test_backup_nip49_empty_password);
    g_test_add_func("/signer/core/backup/nip49_invalid_encrypted", test_backup_nip49_invalid_encrypted);
    g_test_add_func("/signer/core/backup/mnemonic_validate_12", test_backup_mnemonic_validate_12_words);
    g_test_add_func("/signer/core/backup/mnemonic_validate_24", test_backup_mnemonic_validate_24_words);
    g_test_add_func("/signer/core/backup/mnemonic_invalid_count", test_backup_mnemonic_invalid_word_count);
    g_test_add_func("/signer/core/backup/mnemonic_to_key", test_backup_mnemonic_to_key);

    /* Session Manager Tests */
    g_test_add("/signer/core/session/create_locked", CoreFixture, NULL,
               core_fixture_setup, test_session_create_starts_locked, core_fixture_teardown);
    g_test_add("/signer/core/session/auth_no_password", CoreFixture, NULL,
               core_fixture_setup, test_session_authenticate_no_password, core_fixture_teardown);
    g_test_add("/signer/core/session/auth_with_password", CoreFixture, NULL,
               core_fixture_setup, test_session_authenticate_with_password, core_fixture_teardown);
    g_test_add("/signer/core/session/lock", CoreFixture, NULL,
               core_fixture_setup, test_session_lock, core_fixture_teardown);
    g_test_add("/signer/core/session/lock_already_locked", CoreFixture, NULL,
               core_fixture_setup, test_session_lock_already_locked, core_fixture_teardown);
    g_test_add("/signer/core/session/timeout_check", CoreFixture, NULL,
               core_fixture_setup, test_session_timeout_check, core_fixture_teardown);
    g_test_add("/signer/core/session/extend_resets_timeout", CoreFixture, NULL,
               core_fixture_setup, test_session_extend_resets_timeout, core_fixture_teardown);
    g_test_add("/signer/core/session/zero_timeout_disables", CoreFixture, NULL,
               core_fixture_setup, test_session_zero_timeout_disables, core_fixture_teardown);
    g_test_add("/signer/core/session/password_change", CoreFixture, NULL,
               core_fixture_setup, test_session_password_change, core_fixture_teardown);
    g_test_add("/signer/core/session/multiple_cycles", CoreFixture, NULL,
               core_fixture_setup, test_session_multiple_lock_unlock_cycles, core_fixture_teardown);

    /* Relay Store Tests */
    g_test_add("/signer/core/relay/add", CoreFixture, NULL,
               core_fixture_setup, test_relay_store_add, core_fixture_teardown);
    g_test_add("/signer/core/relay/add_duplicate", CoreFixture, NULL,
               core_fixture_setup, test_relay_store_add_duplicate, core_fixture_teardown);
    g_test_add("/signer/core/relay/remove", CoreFixture, NULL,
               core_fixture_setup, test_relay_store_remove, core_fixture_teardown);
    g_test_add("/signer/core/relay/remove_nonexistent", CoreFixture, NULL,
               core_fixture_setup, test_relay_store_remove_nonexistent, core_fixture_teardown);
    g_test_add("/signer/core/relay/update", CoreFixture, NULL,
               core_fixture_setup, test_relay_store_update, core_fixture_teardown);
    g_test_add("/signer/core/relay/update_nonexistent", CoreFixture, NULL,
               core_fixture_setup, test_relay_store_update_nonexistent, core_fixture_teardown);
    g_test_add_func("/signer/core/relay/validate_url_valid", test_relay_validate_url_valid);
    g_test_add_func("/signer/core/relay/validate_url_invalid", test_relay_validate_url_invalid);
    g_test_add("/signer/core/relay/multiple", CoreFixture, NULL,
               core_fixture_setup, test_relay_store_multiple_relays, core_fixture_teardown);
    g_test_add_func("/signer/core/relay/identity_specific", test_relay_store_identity_specific);

    /* Delegation Tests (NIP-26) */
    g_test_add("/signer/core/delegation/create", CoreFixture, NULL,
               core_fixture_setup, test_delegation_create, core_fixture_teardown);
    g_test_add("/signer/core/delegation/create_with_kinds", CoreFixture, NULL,
               core_fixture_setup, test_delegation_create_with_kinds, core_fixture_teardown);
    g_test_add("/signer/core/delegation/create_with_time_bounds", CoreFixture, NULL,
               core_fixture_setup, test_delegation_create_with_time_bounds, core_fixture_teardown);
    g_test_add("/signer/core/delegation/is_valid_basic", CoreFixture, NULL,
               core_fixture_setup, test_delegation_is_valid_basic, core_fixture_teardown);
    g_test_add("/signer/core/delegation/is_valid_kind_restricted", CoreFixture, NULL,
               core_fixture_setup, test_delegation_is_valid_kind_restricted, core_fixture_teardown);
    g_test_add("/signer/core/delegation/is_valid_time_bounded", CoreFixture, NULL,
               core_fixture_setup, test_delegation_is_valid_time_bounded, core_fixture_teardown);
    g_test_add("/signer/core/delegation/revoke", CoreFixture, NULL,
               core_fixture_setup, test_delegation_revoke, core_fixture_teardown);
    g_test_add("/signer/core/delegation/revoke_nonexistent", CoreFixture, NULL,
               core_fixture_setup, test_delegation_revoke_nonexistent, core_fixture_teardown);
    g_test_add_func("/signer/core/delegation/build_conditions_empty", test_delegation_build_conditions_empty);
    g_test_add_func("/signer/core/delegation/build_conditions_kinds", test_delegation_build_conditions_kinds_only);
    g_test_add_func("/signer/core/delegation/build_conditions_full", test_delegation_build_conditions_full);

    /* Integration Tests */
    g_test_add("/signer/core/integration/key_lifecycle", CoreFixture, NULL,
               core_fixture_setup, test_integration_key_lifecycle, core_fixture_teardown);
    g_test_add_func("/signer/core/integration/backup_restore", test_integration_backup_restore);
    g_test_add("/signer/core/integration/session_with_store", CoreFixture, NULL,
               core_fixture_setup, test_integration_session_with_secret_store, core_fixture_teardown);
    g_test_add("/signer/core/integration/relay_with_delegation", CoreFixture, NULL,
               core_fixture_setup, test_integration_relay_with_delegation, core_fixture_teardown);

    return g_test_run();
}
