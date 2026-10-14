#include "test-control-common.h"
#include <nostr-kinds.h>
#include <nostr/nip55l/signer_ops.h>
#include <nostr/nip19/nip19.h>
#include <secure_buf.h>
#ifdef GROTTO_HAVE_NIP49_EXPORT
#include <nostr/nip49/nip49_g.h>
#endif

static GVariant *operation_rows(const gchar *principal, const gchar *identity, const gchar *operation) {
  GVariantBuilder list, row; g_variant_builder_init(&list, G_VARIANT_TYPE("aa{sv}"));
  g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&row, "{sv}", "principal", g_variant_new_string(principal));
  g_variant_builder_add(&row, "{sv}", "identity", g_variant_new_string(identity));
  g_variant_builder_add(&row, "{sv}", "operation", g_variant_new_string(operation));
  g_variant_builder_add(&row, "{sv}", "decision", g_variant_new_string("allow"));
  g_variant_builder_add(&row, "{sv}", "event-kind", g_variant_new_int32(-1));
  g_variant_builder_add(&row, "{sv}", "expires-at", g_variant_new_uint64(0));
  g_variant_builder_add_value(&list, g_variant_builder_end(&row));
  return g_variant_ref_sink(g_variant_builder_end(&list));
}
static GVariant *permission_rows(const gchar *principal, const gchar *identity) {
  return operation_rows(principal, identity, "get_relays");
}
static GVariant *client_row(ControlFixture *f, const gchar *principal) {
  g_autoptr(GVariant) rows = get_rows(f, "ListClients", NULL);
  for (gsize i = 0; i < g_variant_n_children(rows); i++) {
    GVariant *row = g_variant_get_child_value(rows, i); const gchar *p;
    if (g_variant_lookup(row, "principal", "&s", &p) && !strcmp(p, principal)) return row;
    g_variant_unref(row);
  }
  g_assert_not_reached();
}
static void test_contract(ControlFixture *f, gconstpointer data) {
  (void)data; g_autoptr(GVariant) status = get_status(f); guint32 version; const gchar *epoch;
  g_assert_true(g_variant_lookup(status, "control_version", "u", &version)); g_assert_cmpuint(version, ==, 1);
  g_assert_true(g_variant_lookup(status, "control_epoch", "&s", &epoch)); g_assert_true(g_uuid_string_is_valid(epoch));
  g_autoptr(GVariant) caps = g_variant_lookup_value(status, "capabilities", G_VARIANT_TYPE("a{sb}")); gboolean capable;
  g_assert_true(g_variant_lookup(caps, "pairing", "b", &capable)); g_assert_false(capable);
  g_assert_true(g_variant_lookup(caps, "scoped-permissions", "b", &capable)); g_assert_true(capable);
  g_autoptr(GVariant) rows = get_rows(f, "ListClients", NULL); g_assert_cmpuint(g_variant_n_children(rows), ==, 0);
  g_autoptr(GVariant) reqs = get_rows(f, "ListRequests", NULL); g_assert_cmpuint(g_variant_n_children(reqs), ==, 0);
  expect_control_error(f, "ConfigureBunker", g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)), GROTTO_CONTROL_ERROR "NotReady");
  expect_control_error(f, "CreatePairing", g_variant_new("(ss)", "", ""), GROTTO_CONTROL_ERROR "NotReady");
  expect_control_error(f, "AcceptConnectURI", g_variant_new("(ss)", "nostrconnect://invalid", ""), GROTTO_CONTROL_ERROR "NotReady");
  expect_control_error(f, "CancelPairing", g_variant_new("(s)", "opaque-id"), GROTTO_CONTROL_ERROR "NotReady");
  expect_control_error(f, "ResetBunkerEndpoint", NULL, GROTTO_CONTROL_ERROR "NotReady");
  g_autoptr(GVariant) paused = control_call(f, "SetPaused", g_variant_new("(b)", TRUE));
  g_assert_true(signer_approval_service_is_paused(f->approval));
  g_autoptr(GVariant) resumed = control_call(f, "SetPaused", g_variant_new("(b)", FALSE));
  g_assert_false(signer_approval_service_is_paused(f->approval));
}
static void test_conflict(ControlFixture *f, gconstpointer data) {
  (void)data; const gchar *p = "https://one.example"; guint64 revision = get_revision(f);
  g_autoptr(GVariant) rows = permission_rows(p, "*");
  g_autoptr(GVariant) ok = control_call(f, "SetPermissions", g_variant_new("(ss@aa{sv}t)", p, "*", rows, revision));
  expect_control_error(f, "SetPermissions", g_variant_new("(ss@aa{sv}t)", p, "*", rows, revision), GROTTO_CONTROL_ERROR "Conflict");
  g_autoptr(GVariant) listed = get_rows(f, "ListPermissions", g_variant_new("(s)", p));
  g_assert_cmpuint(g_variant_n_children(listed), ==, 1);
  g_autoptr(GVariant) other = get_rows(f, "ListPermissions", g_variant_new("(s)", "https://two.example"));
  g_assert_cmpuint(g_variant_n_children(other), ==, 0);
  guint64 current = get_revision(f);
  /* Legacy ApproveRequest/remember mutations also invalidate Control1 revisions. */
  g_assert_true(signer_permissions_remember(signer_approval_service_permissions(f->approval), p, "*", "ping", -1, FALSE, 600, NULL));
  expect_control_error(f, "SetPermissions", g_variant_new("(ss@aa{sv}t)", p, "*", rows, current), GROTTO_CONTROL_ERROR "Conflict");
  guint64 old = get_revision(f);
  grotto_control_free(f->control);
  f->control = grotto_control_new(f->server, f->approval, NULL, NULL); g_assert_nonnull(f->control);
  g_assert_cmpuint(get_revision(f), !=, old);
  expect_control_error(f, "SetPermissions", g_variant_new("(ss@aa{sv}t)", p, "*", rows, old), GROTTO_CONTROL_ERROR "Conflict");
  g_autoptr(GVariant) revoked = control_call(f, "RevokeClient", g_variant_new("(s)", p));
  g_autoptr(GVariant) cleared = get_rows(f, "ListPermissions", g_variant_new("(s)", p)); g_assert_cmpuint(g_variant_n_children(cleared), ==, 0);
}
static void test_transports(ControlFixture *f, gconstpointer data) {
  (void)data;
  const gchar *principals[] = {"exe:/test/local", "exe:/test/socket", "https://first.example", "https://second.example", "nip46:79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"};
  SignerTransport transports[] = {SIGNER_TRANSPORT_DBUS, SIGNER_TRANSPORT_NIP5F, SIGNER_TRANSPORT_DBUS, SIGNER_TRANSPORT_DBUS, SIGNER_TRANSPORT_NIP46};
  for (guint i = 0; i < G_N_ELEMENTS(principals); i++) {
    SignerCaller who = {.kind=SIGNER_CALLER_EXE, .principal=(gchar *)principals[i], .sender=(gchar *)principals[i], .same_uid=TRUE};
    TestReply reply = {0};
    g_autoptr(SignerGateHandle) h = submit_request(f, &who, transports[i], SIGNER_GATE_GET_PUBLIC_KEY, NULL, 0, &reply);
    const gchar *id = pending_id(f, i); g_autoptr(GVariant) pending = get_rows(f, "ListRequests", NULL);
    g_assert_cmpuint(g_variant_n_children(pending), ==, 1);
    decision(f, id, TRUE, FALSE); wait_count(&reply.count, 1); g_assert_null(reply.error); g_assert_nonnull(reply.value);
    clear_reply(&reply);
  }
  g_autoptr(GVariant) rows = get_rows(f, "ListClients", NULL); g_assert_cmpuint(g_variant_n_children(rows), ==, 5);
  for (gsize i = 0; i < g_variant_n_children(rows); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(rows, i); const gchar *tier; guint64 first;
    g_assert_true(g_variant_lookup(row, "trust-tier", "&s", &tier)); g_assert_cmpstr(tier, ==, "low");
    g_assert_true(g_variant_lookup(row, "first-consented-at", "t", &first)); g_assert_cmpuint(first, >, 0);
  }
  g_autoptr(GVariant) label = control_call(f, "SetClientLabel", g_variant_new("(ss)", principals[2], "First website"));
  expect_control_error(f, "SetClientLabel", g_variant_new("(ss)", "https://unknown.example", "Not admitted"), GROTTO_CONTROL_ERROR "NotFound");
  g_autoptr(GVariant) changed = control_call(f, "SetClientTrust", g_variant_new("(ss)", principals[2], "full"));
  gboolean consented;
  /* Inspect the durable live store, not a test-double lookup. */
  g_autoptr(GrottoClients) store = grotto_clients_new(NULL, NULL); g_assert_nonnull(store);
  g_assert_cmpint(grotto_clients_trust(principals[2], &consented, store), ==, SIGNER_TRUST_FULL); g_assert_true(consented);
  g_assert_cmpint(grotto_clients_trust(principals[3], &consented, store), ==, SIGNER_TRUST_LOW);
  /* Explicit identity selection updates only this client's metadata. */
  g_assert_true(grotto_clients_record(store, principals[2], "DBUS", "claimed", "npub-second-identity", TRUE, TRUE, NULL));
  g_autoptr(GVariant) stored = grotto_clients_list(store);
  for (gsize i = 0; i < g_variant_n_children(stored); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(stored, i); const gchar *p, *identity;
    g_variant_lookup(row, "principal", "&s", &p); g_variant_lookup(row, "identity", "&s", &identity);
    if (!strcmp(p, principals[2])) g_assert_cmpstr(identity, ==, "npub-second-identity");
    if (!strcmp(p, principals[3])) g_assert_cmpstr(identity, !=, "npub-second-identity");
  }
  const gchar *relays[] = {"wss://nos.lol", NULL};
  g_assert_true(grotto_clients_set_pairing(store, principals[4], TRUE, relays, NULL));
  g_clear_pointer(&store, grotto_clients_free); store = grotto_clients_new(NULL, NULL); g_assert_nonnull(store);
  g_autoptr(GVariant) reloaded = grotto_clients_list(store); gboolean found = FALSE;
  for (gsize i = 0; i < g_variant_n_children(reloaded); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(reloaded, i); const gchar *p; g_variant_lookup(row, "principal", "&s", &p);
    if (!strcmp(p, principals[4])) { gboolean paired; g_variant_lookup(row, "paired", "b", &paired); g_assert_true(paired);
      g_autoptr(GVariant) urls = g_variant_lookup_value(row, "relays", G_VARIANT_TYPE_STRING_ARRAY); g_assert_cmpuint(g_variant_n_children(urls), ==, 1); found = TRUE; }
  }
  g_assert_true(found);
}
/* Review F2: a client created by a management permission is a known record
 * without Low admission. Its automatic (permission-allowed) successes are not
 * consent decisions, yet they must still refresh the observed metadata. */
static void test_permission_created_client(ControlFixture *f, gconstpointer data) {
  (void)data; const gchar *p = "exe:/permission-created"; char *npub = NULL;
  g_assert_cmpint(nostr_nip55l_get_public_key(&npub), ==, 0);
  g_autoptr(GVariant) rows = operation_rows(p, npub, "get_public_key");
  g_autoptr(GVariant) set = control_call(f, "SetPermissions",
      g_variant_new("(ss@aa{sv}t)", p, npub, rows, get_revision(f)));
  g_autoptr(GVariant) created = client_row(f, p); const gchar *identity, *transport, *tier; guint64 first, used;
  g_assert_true(g_variant_lookup(created, "identity", "&s", &identity)); g_assert_cmpstr(identity, ==, "");
  g_assert_true(g_variant_lookup(created, "last-used-at", "t", &used)); g_assert_cmpuint(used, ==, 0);
  SignerCaller who = {.kind=SIGNER_CALLER_EXE, .principal=(gchar *)p, .sender="permission-created", .same_uid=TRUE};
  TestReply reply = {0};
  g_autoptr(SignerGateHandle) h = submit_request(f, &who, SIGNER_TRANSPORT_NIP5F, SIGNER_GATE_GET_PUBLIC_KEY, NULL, 0, &reply);
  wait_count(&reply.count, 1); g_assert_null(reply.error); g_assert_cmpstr(reply.value, ==, npub);
  wait_count(&f->finished, 1); g_assert_cmpuint(f->pending->len, ==, 0); /* allowed without a prompt */
  g_autoptr(GVariant) row = client_row(f, p);
  g_assert_true(g_variant_lookup(row, "identity", "&s", &identity)); g_assert_cmpstr(identity, ==, npub);
  g_assert_true(g_variant_lookup(row, "transport", "&s", &transport)); g_assert_cmpstr(transport, ==, "NIP5F");
  g_assert_true(g_variant_lookup(row, "last-used-at", "t", &used)); g_assert_cmpuint(used, >, 0);
  /* Metadata only: no consent timestamp, no tier change, no Low admission. */
  g_assert_true(g_variant_lookup(row, "first-consented-at", "t", &first)); g_assert_cmpuint(first, ==, 0);
  g_assert_true(g_variant_lookup(row, "trust-tier", "&s", &tier)); g_assert_cmpstr(tier, ==, "low");
  g_autoptr(GrottoClients) store = grotto_clients_new(NULL, NULL); gboolean consented = TRUE;
  g_assert_nonnull(store); g_assert_cmpint(grotto_clients_trust(p, &consented, store), ==, SIGNER_TRUST_LOW);
  g_assert_false(consented); clear_reply(&reply); free(npub);
}
static void test_failed_not_client(ControlFixture *f, gconstpointer data) {
  (void)data; SignerCaller who = {.kind=SIGNER_CALLER_EXE, .principal="exe:/never-consented", .sender="bad", .same_uid=TRUE};
  TestReply reply = {0}; g_autoptr(SignerGateHandle) h = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_SIGN_EVENT, "{malformed", 0, &reply);
  wait_count(&reply.count, 1); g_assert_nonnull(reply.error); clear_reply(&reply);
  g_autoptr(GVariant) rows = get_rows(f, "ListClients", NULL); g_assert_cmpuint(g_variant_n_children(rows), ==, 0);
}
#ifdef NIP55L_TEST_TRUST_ENV
typedef struct { GMutex mutex; GCond cond; gboolean entered, released; } Barrier;
static void block_worker(gpointer data) {
  Barrier *b = data; g_mutex_lock(&b->mutex); b->entered = TRUE; g_cond_broadcast(&b->cond);
  while (!b->released) g_cond_wait(&b->cond, &b->mutex); g_mutex_unlock(&b->mutex);
}
static guint64 request_revision(ControlFixture *f) {
  g_autoptr(GVariant) s = get_status(f); guint64 r;
  g_assert_true(g_variant_lookup(s, "requests_revision", "t", &r)); return r;
}
static void test_request_revisions(ControlFixture *f, gconstpointer data) {
  (void)data; Barrier b = {0}; g_mutex_init(&b.mutex); g_cond_init(&b.cond);
  signer_approval_service_test_after_backend(f->approval, block_worker, &b);
  SignerCaller who = {.kind=SIGNER_CALLER_EXE, .principal="exe:/joined", .sender="join-a", .same_uid=TRUE};
  TestReply a = {0}, joined = {0};
  g_autoptr(SignerGateHandle) ha = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_PING, NULL, 0, &a);
  const gchar *id = pending_id(f, 0); guint64 before = request_revision(f);
  who.sender = "join-b";
  g_autoptr(SignerGateHandle) hb = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_PING, NULL,
      g_get_monotonic_time() + 120 * G_USEC_PER_SEC, &joined);
  g_assert_cmpuint(f->pending->len, ==, 1); g_assert_cmpuint(request_revision(f), >, before);
  g_autoptr(GVariant) rows = get_rows(f, "ListRequests", NULL); g_assert_cmpuint(g_variant_n_children(rows), ==, 1);
  g_autoptr(GVariant) row = g_variant_get_child_value(rows, 0); guint calls, seconds;
  g_variant_lookup(row, "calls", "u", &calls); g_assert_cmpuint(calls, ==, 2);
  g_variant_lookup(row, "remaining_seconds", "u", &seconds); g_assert_cmpuint(seconds, <=, 120);
  before = request_revision(f); signer_approval_service_connection_closed(f->approval, "join-a");
  wait_count(&a.count, 1); g_assert_cmpuint(request_revision(f), >, before);
  g_autoptr(GVariant) activity = get_rows(f, "ListActivity", g_variant_new("(tu)", (guint64)0, (guint)100));
  g_assert_cmpuint(g_variant_n_children(activity), ==, 0);
  before = request_revision(f); guint done = 0;
  signer_approval_service_decide(f->approval, id, TRUE, FALSE, 0, decided, &done);
  g_mutex_lock(&b.mutex); gint64 bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!b.entered) g_assert_true(g_cond_wait_until(&b.cond, &b.mutex, bound)); g_mutex_unlock(&b.mutex);
  g_clear_pointer(&rows, g_variant_unref); rows = get_rows(f, "ListRequests", NULL);
  g_assert_cmpuint(g_variant_n_children(rows), ==, 0); g_assert_cmpuint(f->finished, ==, 0);
  g_assert_cmpuint(request_revision(f), >, before);
  g_mutex_lock(&b.mutex); b.released = TRUE; g_cond_broadcast(&b.cond); g_mutex_unlock(&b.mutex);
  wait_count(&joined.count, 1); wait_count(&done, 1); g_assert_null(joined.error);
  clear_reply(&a); clear_reply(&joined);
  signer_approval_service_test_after_backend(f->approval, NULL, NULL); g_mutex_clear(&b.mutex); g_cond_clear(&b.cond);
}
static void test_live_tier_lowering(ControlFixture *f, gconstpointer data) {
  (void)data; SignerCaller who = {.kind=SIGNER_CALLER_EXE, .principal="exe:/tier-test", .sender="first", .same_uid=TRUE};
  TestReply consent = {0}; g_autoptr(SignerGateHandle) hc = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_GET_PUBLIC_KEY, NULL, 0, &consent);
  decision(f, pending_id(f, 0), TRUE, FALSE); wait_count(&consent.count, 1); clear_reply(&consent);
  g_autoptr(GVariant) full = control_call(f, "SetClientTrust", g_variant_new("(ss)", who.principal, "full"));
  Barrier b = {0}; g_mutex_init(&b.mutex); g_cond_init(&b.cond);
  signer_approval_service_test_after_backend(f->approval, block_worker, &b);
  TestReply blocker = {0}; who.sender = "blocker";
  g_autoptr(SignerGateHandle) hb = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_PING, NULL, 0, &blocker);
  g_mutex_lock(&b.mutex); gint64 bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!b.entered) g_assert_true(g_cond_wait_until(&b.cond, &b.mutex, bound)); g_mutex_unlock(&b.mutex);
  TestReply queued = {0}; who.sender = "queued";
  g_autoptr(SignerGateHandle) hq = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_PING, NULL, 0, &queued);
  g_autoptr(GVariant) low = control_call(f, "SetClientTrust", g_variant_new("(ss)", who.principal, "low"));
  g_mutex_lock(&b.mutex); b.released = TRUE; g_cond_broadcast(&b.cond); g_mutex_unlock(&b.mutex);
  wait_count(&blocker.count, 1); wait_count(&queued.count, 1);
  g_assert_nonnull(queued.error); g_assert_null(queued.value); clear_reply(&queued); clear_reply(&blocker);
  signer_approval_service_test_after_backend(f->approval, NULL, NULL);
  g_mutex_clear(&b.mutex); g_cond_clear(&b.cond);
}
#endif
static void test_store_failures(ControlFixture *f, gconstpointer data) {
  (void)f; (void)data; g_autofree gchar *dir = g_dir_make_tmp("g04-clients-XXXXXX", NULL);
  g_autoptr(GError) error = NULL; g_autoptr(GrottoClients) c = grotto_clients_new(dir, &error); g_assert_no_error(error);
  g_assert_true(grotto_clients_record(c, "https://store.example", "DBUS", "untrusted", "identity", TRUE, FALSE, &error));
  g_assert_no_error(error); g_autofree gchar *path = g_build_filename(dir, "grotto", "clients.ini", NULL);
  GStatBuf st; g_assert_cmpint(g_stat(path, &st), ==, 0); g_assert_cmpint(st.st_mode & 0777, ==, 0600);
  /* Replacing the file by a directory deterministically breaks atomic persistence. */
  g_assert_cmpint(g_unlink(path), ==, 0); g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  g_assert_false(grotto_clients_set_tier(c, "https://store.example", SIGNER_TRUST_FULL, &error)); g_clear_error(&error);
  gboolean consented; g_assert_cmpint(grotto_clients_trust("https://store.example", &consented, c), ==, SIGNER_TRUST_LOW);
  g_assert_cmpint(g_rmdir(path), ==, 0);
  g_assert_true(g_file_set_contents(path, "[clients]\nversion=2\n", -1, NULL));
  g_autoptr(GrottoClients) newer = grotto_clients_new(dir, &error); g_assert_null(newer); g_assert_nonnull(error);
  g_unlink(path); g_autofree gchar *parent = g_path_get_dirname(path); g_rmdir(parent); g_rmdir(dir);
}
static void test_export(ControlFixture *f, gconstpointer data) {
  (void)data; char *identity = NULL; g_assert_cmpint(nostr_nip55l_get_public_key(&identity), ==, 0);
  g_autoptr(GVariant) raw = control_call(f, "ExportRawIdentity", g_variant_new("(s)", identity));
  const gchar *nsec; g_variant_get(raw, "(&s)", &nsec); guint8 key[32] = {0};
  g_assert_cmpint(nostr_nip19_decode_nsec(nsec, key), ==, 0);
  for (guint i = 0; i < 31; i++) g_assert_cmpuint(key[i], ==, 0); g_assert_cmpuint(key[31], ==, 1); secure_wipe(key, sizeof key);
  expect_control_error(f, "ExportIdentity", g_variant_new("(sss)", identity, "nsec", "ignored"), GROTTO_CONTROL_ERROR "NotSupported");
  expect_control_error(f, "ExportIdentity", g_variant_new("(sss)", identity, "mnemonic", ""), GROTTO_CONTROL_ERROR "NotSupported");
  expect_control_error(f, "RotateIdentity", g_variant_new("(s)", identity), GROTTO_CONTROL_ERROR "NotSupported");
  expect_control_error(f, "ExportRawIdentity", g_variant_new("(s)", ""), GROTTO_CONTROL_ERROR "InvalidInput");
  expect_control_error(f, "ExportRawIdentity", g_variant_new("(s)", nsec), GROTTO_CONTROL_ERROR "InvalidInput");
#ifdef GROTTO_HAVE_NIP49_EXPORT
  expect_control_error(f, "ExportIdentity", g_variant_new("(sss)", identity, "ncryptsec", ""), GROTTO_CONTROL_ERROR "InvalidInput");
  /* Unicode NFKC and the default format travel the real authenticated API. */
  g_autoptr(GVariant) encrypted = control_call(f, "ExportIdentity", g_variant_new("(sss)", identity, "", "caf\303\251"));
  const gchar *backup; g_variant_get(encrypted, "(&s)", &backup); g_assert_true(g_str_has_prefix(backup, "ncryptsec1"));
  g_assert_true(nostr_nip49_decrypt_g(backup, "cafe\314\201", key, NULL, NULL, NULL));
  for (guint i = 0; i < 31; i++) g_assert_cmpuint(key[i], ==, 0); g_assert_cmpuint(key[31], ==, 1); secure_wipe(key, sizeof key);
#endif
  g_autoptr(GVariant) activity = get_rows(f, "ListActivity", g_variant_new("(tu)", (guint64)0, (guint)100));
  g_assert_cmpuint(g_variant_n_children(activity), ==, 0); free(identity);
}
static void process_done(GObject *object, GAsyncResult *result, gpointer data) {
  guint *done = data; g_autoptr(GError) error = NULL;
  g_assert_true(g_subprocess_wait_check_finish(G_SUBPROCESS(object), result, &error)); g_assert_no_error(error); (*done)++;
}
static void test_auth(ControlFixture *f, gconstpointer data) {
  (void)data;
#ifdef __linux__
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) peer = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
      CONTROL_UNTRUSTED_BIN, g_dbus_connection_get_unique_name(f->server), NULL);
  g_assert_no_error(error); guint done = 0;
  g_subprocess_wait_check_async(peer, NULL, process_done, &done); wait_count(&done, 1);
  g_assert_true(signer_approval_service_is_paused(f->approval));
  g_autoptr(GVariant) resume = control_call(f, "SetPaused", g_variant_new("(b)", FALSE));
#else
  (void)f; g_test_skip("UID-only bus cannot distinguish the trusted executable; Linux attestation gate required");
#endif
}
static void name_acquired(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *signal, GVariant *args, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal;
  const gchar *name, *old, *owner; g_variant_get(args, "(&s&s&s)", &name, &old, &owner);
  if (!strcmp(name, "org.nostr.Signer") && *owner) (*(guint *)data)++;
}
static void test_daemon_export(ControlFixture *f, gconstpointer data) {
  (void)data; guint ready = 0;
  guint sub = g_dbus_connection_signal_subscribe(f->client, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", "org.nostr.Signer", 0, name_acquired, &ready, NULL);
  const gchar *argv[] = {GROTTO_DAEMON_BIN, NULL};
  nostrc_test_bus_spawn_supervised(f->bus, "g04-daemon.log", NULL, argv); wait_count(&ready, 1);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) status = control_call_to(f, "org.nostr.Signer", "GetStatus", NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(status);
  g_dbus_connection_signal_unsubscribe(f->client, sub);
}
int main(int argc, char **argv) {
  test_environment(argv[0]); g_test_init(&argc, &argv, NULL);
  if (!nostrc_test_bus_available()) return 77;
#define ADD(name, fn) g_test_add(name, ControlFixture, NULL, fixture_setup, fn, fixture_teardown)
  ADD("/control/contract", test_contract); ADD("/control/conflict-epoch", test_conflict);
  ADD("/control/auth-pause-asymmetry", test_auth); ADD("/control/real-daemon-export", test_daemon_export);
  ADD("/control/custody-export", test_export);
  ADD("/clients/transports-origins-identity", test_transports); ADD("/clients/failed-not-admitted", test_failed_not_client);
  ADD("/clients/permission-created-observed-use", test_permission_created_client);
  ADD("/clients/persistence-failure-version", test_store_failures);
#ifdef NIP55L_TEST_TRUST_ENV
  ADD("/clients/live-tier-lowering", test_live_tier_lowering);
  ADD("/control/join-detach-execution-revisions", test_request_revisions);
#endif
  return g_test_run();
}
