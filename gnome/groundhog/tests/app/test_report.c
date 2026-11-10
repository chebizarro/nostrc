#include "gh-report-service.h"
#include "gh-nip29-group.h"
#include "gh-identity.h"
#include "gh-relay-deny-policy.h"
#include <nostr/nip19/nip19.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <nostr-kinds.h>
#include <json.h>
#include <sqlite3.h>
#include <string.h>
#define KEY "0000000000000000000000000000000000000000000000000000000000000001"
#define ACCOUNT "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define TARGET "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static gchar *sign_json(const gchar *json, gboolean mutate)
{
  NostrEvent *e = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_unsigned(e, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  if (mutate) nostr_event_set_content(e, "Signer changed the approved explanation");
  g_assert_cmpint(nostr_event_sign(e, KEY), ==, 0);
  gchar *signed_json = nostr_event_serialize_compact(e);
  nostr_event_free(e); return signed_json;
}
typedef struct {
  GhStore *store;
  GhSocialLists *lists;
  GhReportService *service;
  GSettings *settings;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhRelayScope *scope;
  gchar *directory;
  guint64 generation;
  GhRelayPublish *publish;
  guint signs, opens, closes;
  gboolean mutate, hold;
  GTask *pending;
  gchar *wire, *url, *auth_id;
  guint auths, resends;
} Fixture;
static void sign_async(gpointer data, const gchar *json, GCancellable *cancel,
                       GAsyncReadyCallback callback, gpointer user_data)
{
  Fixture *f = data; f->signs++;
  /* The real persisted approval must exist before the signer is asked. */
  g_autoptr(GPtrArray) rows = gh_store_report_list(f->store, NULL);
  g_assert_cmpuint(rows->len, >, 0);
  GhStoreReport *r = g_ptr_array_index(rows, 0);
  g_assert_cmpstr(r->unsigned_json, ==, json);
  GTask *task = g_task_new(NULL, cancel, callback, user_data);
  if (f->hold) { f->pending = task; return; }
  g_task_return_pointer(task, sign_json(json, f->mutate), g_free);
  g_object_unref(task);
}
static gchar *sign_finish(GAsyncResult *r, GError **e) { return g_task_propagate_pointer(G_TASK(r), e); }
static gpointer open_wire(GhRelayPublish *p, const gchar *url, const gchar *json, gpointer data, GError **error)
{
  (void)error;
  Fixture *f = data; f->opens++; f->publish = p;
  g_free(f->wire); f->wire = g_strdup(json); g_free(f->url); f->url = g_strdup(url);
  return f;
}
static void close_wire(gpointer handle, gpointer data) { (void)handle; ((Fixture *)data)->closes++; }
static const GhRelayPublishTransport transport = { open_wire, close_wire };
static gboolean send_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  (void)handle; (void)error;
  Fixture *f = data; NostrEvent *e = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(e, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_validate(e, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(e), ==, NOSTR_KIND_CLIENT_AUTHENTICATION);
  g_assert_cmpstr(nostr_event_get_pubkey(e), !=, ACCOUNT);
  g_free(f->auth_id); f->auth_id = nostr_event_get_id(e); f->auths++;
  nostr_event_free(e); return TRUE;
}
static gboolean resend(gpointer handle, gpointer data, GError **error)
{
  (void)handle; (void)error; ((Fixture *)data)->resends++; return TRUE;
}
static const GhRelayPublishAuthTransport auth_transport = {send_auth, resend};
static gchar *npub;
static GPtrArray *identities(gpointer data, GError **error)
{
  (void)data; (void)error;
  GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *id = g_new0(GhIdentityInfo, 1); id->npub = g_strdup(npub); id->label = g_strdup("Disposable report account");
  g_ptr_array_add(rows, id); return rows;
}
static gpointer read_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
                           gpointer data, GError **error)
{
  (void)url; (void)filters; (void)error;
  Fixture *f = data; f->scope = scope; return f;
}
static void read_close(gpointer handle, gpointer data) { (void)handle; (void)data; }
static const GhRelayTransport read_transport = {read_open, read_close};
static void configure_targets(Fixture *f)
{
  g_autofree gchar *json = sign_json("{\"pubkey\":\"" ACCOUNT "\",\"kind\":10002,\"created_at\":100,\"content\":\"\",\"tags\":[[\"r\",\"wss://write.test.invalid\",\"write\"],[\"r\",\"wss://blocked.test.invalid\",\"write\"],[\"r\",\"wss://read.test.invalid\",\"read\"]]}", FALSE);
  gh_relay_scope_event(f->scope, "wss://discovery.test.invalid", json);
  gh_relay_scope_eose(f->scope, "wss://discovery.test.invalid");
  const gchar *denied[] = {"wss://blocked.test.invalid", NULL};
  gh_relay_deny_policy_bind(gh_relay_deny_policy_get_default(), f->generation, denied);
}
static void setup(Fixture *f, gconstpointer unused)
{
  (void)unused;
  f->directory = g_dir_make_tmp("groundhog-report-XXXXXX", NULL);
  f->settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = {"wss://discovery.test.invalid", NULL};
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "current-npub", npub);
  f->accounts = gh_account_controller_new_full_with_remote_list(f->settings, NULL,
      identities, NULL, identities, NULL);
  while (gh_account_controller_is_listing(f->accounts)) g_main_context_iteration(NULL, TRUE);
  f->generation = gh_account_controller_get_generation(f->accounts);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &read_transport, f);
  GhStoreConfig config = {.data_dir = f->directory, .account_pubkey = ACCOUNT};
  guint8 bytes[GH_STORE_KEY_SIZE] = {1};
  g_autoptr(GBytes) key = g_bytes_new(bytes, sizeof(bytes));
  g_autoptr(GError) error = NULL;
  f->store = gh_store_open_with_key(&config, key, "735a19fb-e328-4573-ae6c-f7c65a3636f1", GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error); g_assert_nonnull(f->store);
  f->lists = gh_social_lists_new(NULL);
  GhReportSigner signer = {sign_async, sign_finish, f};
  GhListAdapterNetwork network = {.accounts = f->accounts, .scope_transport = &read_transport,
      .scope_data = f, .publish_transport = &transport, .publish_auth_transport = &auth_transport, .publish_data = f};
  gh_social_lists_set_network(f->lists, &network, f->relays);
  g_assert_true(gh_social_lists_bind_store(f->lists, f->store, f->generation, NULL));
  f->service = gh_report_service_new(f->lists, NULL, &network, &signer);
  gh_report_service_bind(f->service, f->store, f->generation);
  configure_targets(f);
}
static void remove_tree(const gchar *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (!dir) { g_unlink(path); return; }
  const gchar *name;
  while ((name = g_dir_read_name(dir))) {
    g_autofree gchar *child = g_build_filename(path, name, NULL); remove_tree(child);
  }
  g_dir_close(dir); g_rmdir(path);
}
static void teardown(Fixture *f, gconstpointer unused)
{
  (void)unused;
  gh_report_service_close(f->service);
  g_clear_object(&f->service);
  gh_social_lists_close(f->lists); g_clear_object(&f->lists);
  gh_store_close(f->store); g_free(f->wire); g_free(f->url); g_free(f->auth_id);
  g_clear_object(&f->relays); g_clear_object(&f->accounts); g_clear_object(&f->settings);
  remove_tree(f->directory); g_free(f->directory);
  while (g_main_context_iteration(NULL, FALSE));
}
static void no_private_evidence(void)
{
  g_autoptr(GError) error = NULL;
  const gchar *origins[] = {"NIP-17", "NIP-04", "MLS", "private NIP-29", "unknown NIP-29"};
  for (guint i = 0; i < G_N_ELEMENTS(origins); i++) {
    g_autofree gchar *json = gh_report_build_account(ACCOUNT, TARGET, NOSTR_NIP56_SPAM,
                                                    "My own explanation \"only\"", 100, &error);
    g_assert_no_error(error);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
    JsonObject *o = json_node_get_object(json_parser_get_root(parser));
    JsonArray *tags = json_object_get_array_member(o, "tags");
    g_assert_cmpuint(json_array_get_length(tags), ==, 1);
    JsonArray *p = json_array_get_array_element(tags, 0);
    g_assert_cmpuint(json_array_get_length(p), ==, 3);
    g_assert_cmpstr(json_array_get_string_element(p, 0), ==, "p");
    g_assert_cmpstr(json_array_get_string_element(p, 1), ==, TARGET);
    g_assert_cmpstr(json_array_get_string_element(p, 2), ==, "spam");
    g_assert_cmpstr(json_object_get_string_member(o, "content"), ==, "My own explanation \"only\"");
    g_autofree gchar *signed_json = sign_json(json, FALSE);
    g_assert_true(gh_report_matches_signed(json, signed_json, &error));
    g_autofree gchar *mutated = sign_json(json, TRUE);
    g_assert_false(gh_report_matches_signed(json, mutated, &error)); g_clear_error(&error);
    g_test_message("%s serializes typed p only", origins[i]);
  }
}
static void metadata(void)
{
  GhNip29GroupKey *key = gh_nip29_group_key_new("wss://nos.lol", "test", NULL);
  GhNip29Group *group = gh_nip29_group_new(key, ACCOUNT, NULL);
  g_assert_false(gh_nip29_group_is_known_public(group));
  g_assert_false(gh_nip29_group_is_known_public(NULL));
  const gchar *json = "{\"pubkey\":\"" ACCOUNT "\",\"kind\":39000,\"created_at\":100,\"tags\":[[\"d\",\"test\"]],\"content\":\"\"}";
  g_autofree gchar *signed_json = sign_json(json, FALSE);
  NostrEvent *e = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(e, signed_json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(gh_nip29_group_admit_at(group, e, 100), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_assert_true(gh_nip29_group_is_known_public(group));
  nostr_event_free(e);
  const gchar *private_json = "{\"pubkey\":\"" ACCOUNT "\",\"kind\":39000,\"created_at\":101,\"tags\":[[\"d\",\"test\"],[\"private\"]],\"content\":\"\"}";
  g_autofree gchar *private_signed = sign_json(private_json, FALSE);
  e = nostr_event_new(); nostr_event_deserialize_signed(e, private_signed, NULL);
  g_assert_cmpint(gh_nip29_group_admit_at(group, e, 101), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_assert_false(gh_nip29_group_is_known_public(group));
  nostr_event_free(e); gh_nip29_group_free(group); gh_nip29_group_key_free(key);
}
static void storage(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree gchar *json = gh_report_build_account(ACCOUNT, TARGET, NOSTR_NIP56_OTHER, "", 100, NULL);
  const gchar *destinations = "[\"wss://nos.lol\"]";
  gint64 id = gh_store_report_approve(f->store, json, destinations, 100, NULL);
  g_assert_cmpint(id, >, 0);
  g_autoptr(GhStoreReport) r = gh_store_report_load(f->store, id, NULL);
  g_assert_cmpstr(r->unsigned_json, ==, json); g_assert_null(r->signed_json);
  g_assert_cmpstr(r->targets_json, ==, destinations);
  /* The persisted approval is not silently retargeted to the current write relay. */
  g_assert_true(gh_report_service_retry(f->service, id, NULL, NULL));
  while (gh_report_service_get_busy(f->service)) g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(f->opens, ==, 0);
}
static gint64 submit(Fixture *f, gboolean hide)
{
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) targets = gh_report_service_dup_targets(f->service);
  g_assert_nonnull(targets); g_assert_cmpstr(targets[0], ==, "wss://write.test.invalid");
  g_assert_null(targets[1]); /* no blocked, read-only, discovery, inbox or group relay */
  g_autofree gchar *json = gh_report_build_account(ACCOUNT, TARGET, NOSTR_NIP56_SPAM,
      "User explanation, not a transcript", 100, &error);
  gint64 id = gh_report_service_submit(f->service, json, (const gchar *const *)targets, hide, &error);
  g_assert_no_error(error); g_assert_cmpint(id, >, 0);
  return id;
}
static void signed_ready(Fixture *f)
{
  while (f->opens == 0 && gh_report_service_get_busy(f->service)) g_main_context_iteration(NULL, TRUE);
}
static void delivery(Fixture *f, gconstpointer unused)
{
  (void)unused;
  gint64 id = submit(f, TRUE);
  g_assert_true(gh_social_lists_is_hidden(f->lists, TARGET));
  signed_ready(f);
  g_assert_cmpuint(f->opens, ==, 1); g_assert_cmpuint(f->signs, ==, 1);
  g_assert_cmpstr(f->url, ==, "wss://write.test.invalid");
  g_autoptr(GhStoreReport) r = gh_store_report_load(f->store, id, NULL);
  g_assert_true(gh_report_matches_signed(r->unsigned_json, f->wire, NULL));
  g_autofree gchar *immutable = g_strdup(f->wire);
  g_assert_null(strstr(f->wire, "\"e\""));
  g_assert_null(strstr(f->wire, "\"h\""));
  gh_relay_publish_ok(f->publish, f->url, gh_relay_publish_get_event_id(f->publish), FALSE, "blocked: test refusal");
  g_assert_false(gh_report_service_get_busy(f->service));
  g_assert_true(gh_social_lists_is_hidden(f->lists, TARGET));
  g_assert_true(gh_report_service_retry(f->service, id, "wss://write.test.invalid", NULL));
  g_assert_cmpuint(f->opens, ==, 2); g_assert_cmpuint(f->signs, ==, 1);
  g_assert_cmpstr(f->wire, ==, immutable);
  gh_relay_publish_ok(f->publish, f->url, gh_relay_publish_get_event_id(f->publish), FALSE, "duplicate: already stored");
  g_autoptr(GhStoreReport) accepted = gh_store_report_load(f->store, id, NULL);
  g_assert_cmpstr(accepted->state, ==, "accepted");
  g_assert_true(gh_report_service_retry(f->service, id, NULL, NULL));
  g_assert_cmpuint(f->opens, ==, 2); /* accepted outcomes are not erased or resent */
}
static void bad_signature(Fixture *f, gconstpointer unused)
{
  (void)unused; f->mutate = TRUE; gint64 id = submit(f, FALSE);
  while (gh_report_service_get_busy(f->service)) g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(f->opens, ==, 0);
  g_autoptr(GhStoreReport) r = gh_store_report_load(f->store, id, NULL);
  g_assert_cmpstr(r->state, ==, "failed"); g_assert_null(r->signed_json);
}
static void hide_failure(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_assert_true(gh_store_exec(f->store,
      "CREATE TRIGGER refuse_hide BEFORE INSERT ON social_documents BEGIN SELECT RAISE(ABORT,'fixture'); END", NULL));
  g_auto(GStrv) targets = gh_report_service_dup_targets(f->service);
  g_autofree gchar *json = gh_report_build_account(ACCOUNT, TARGET, NOSTR_NIP56_SPAM, "", 100, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_report_service_submit(f->service, json, (const gchar *const *)targets, TRUE, &error), ==, 0);
  g_assert_nonnull(error); g_assert_cmpuint(f->signs, ==, 0); g_assert_cmpuint(f->opens, ==, 0);
  g_autoptr(GPtrArray) rows = gh_store_report_list(f->store, NULL);
  g_assert_cmpuint(rows->len, ==, 0);
}
static void cancellation(Fixture *f, gconstpointer unused)
{
  (void)unused; f->hold = TRUE; gint64 id = submit(f, TRUE);
  g_assert_nonnull(f->pending);
  gh_report_service_close(f->service);
  g_task_return_new_error(f->pending, G_IO_ERROR, G_IO_ERROR_CANCELLED, "test cancellation");
  g_clear_object(&f->pending);
  while (g_main_context_iteration(NULL, FALSE));
  g_assert_cmpuint(f->opens, ==, 0);
  gh_report_service_bind(f->service, f->store, f->generation);
  g_assert_cmpuint(f->signs, ==, 1); /* reopening does not retry */
  f->hold = FALSE;
  g_assert_true(gh_report_service_retry(f->service, id, NULL, NULL)); signed_ready(f);
  g_assert_cmpuint(f->opens, ==, 1);
  gh_report_service_cancel(f->service);
  g_assert_cmpuint(f->closes, ==, 1);
  g_assert_true(gh_social_lists_is_hidden(f->lists, TARGET));
}
static void blocked_after_sign(Fixture *f, gconstpointer unused)
{
  (void)unused; f->hold = TRUE; submit(f, FALSE);
  g_autoptr(GPtrArray) rows = gh_store_report_list(f->store, NULL);
  GhStoreReport *r = g_ptr_array_index(rows, 0);
  const gchar *denied[] = {"wss://write.test.invalid", "wss://blocked.test.invalid", NULL};
  gh_relay_deny_policy_bind(gh_relay_deny_policy_get_default(), f->generation, denied);
  g_task_return_pointer(f->pending, sign_json(r->unsigned_json, FALSE), g_free); g_clear_object(&f->pending);
  while (gh_report_service_get_busy(f->service)) g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(f->opens, ==, 0);
  g_autoptr(GhStoreReport) saved = gh_store_report_load(f->store, r->id, NULL);
  g_assert_cmpstr(saved->state, ==, "failed"); g_assert_nonnull(saved->signed_json);
  g_assert_nonnull(strstr(saved->outcomes_json, "wss://write.test.invalid"));
}
static void authentication(Fixture *f, gconstpointer unused)
{
  (void)unused; gint64 id = submit(f, FALSE); signed_ready(f);
  gh_relay_publish_ok(f->publish, f->url, gh_relay_publish_get_event_id(f->publish), FALSE, "auth-required: challenge needed");
  gh_relay_publish_auth_challenge(f->publish, f->url, "report-ephemeral-test");
  while (!f->auths) g_main_context_iteration(NULL, TRUE);
  gh_relay_publish_ok(f->publish, f->url, f->auth_id, TRUE, "");
  g_assert_cmpuint(f->resends, ==, 1);
  gh_relay_publish_ok(f->publish, f->url, gh_relay_publish_get_event_id(f->publish), TRUE, "");
  g_autoptr(GhStoreReport) report = gh_store_report_load(f->store, id, NULL);
  g_assert_cmpstr(report->state, ==, "accepted");
}
static void partial(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree gchar *relay_list = sign_json("{\"pubkey\":\"" ACCOUNT "\",\"kind\":10002,\"created_at\":101,\"content\":\"\",\"tags\":[[\"r\",\"wss://write.test.invalid\",\"write\"],[\"r\",\"wss://write-two.test.invalid\",\"write\"]]}", FALSE);
  gh_relay_scope_event(f->scope, "wss://discovery.test.invalid", relay_list);
  g_auto(GStrv) targets = gh_report_service_dup_targets(f->service);
  g_assert_nonnull(targets[1]);
  g_autofree gchar *json = gh_report_build_account(ACCOUNT, TARGET, NOSTR_NIP56_OTHER, "", 100, NULL);
  gint64 id = gh_report_service_submit(f->service, json, (const gchar *const *)targets, FALSE, NULL);
  signed_ready(f); g_assert_cmpuint(f->opens, ==, 2);
  gh_relay_publish_ok(f->publish, targets[0], gh_relay_publish_get_event_id(f->publish), TRUE, "");
  gh_relay_publish_ok(f->publish, targets[1], gh_relay_publish_get_event_id(f->publish), FALSE, "blocked: refused");
  g_autoptr(GhStoreReport) report = gh_store_report_load(f->store, id, NULL);
  g_assert_cmpstr(report->state, ==, "accepted");
  g_autofree gchar *wire = g_strdup(f->wire);
  g_assert_true(gh_report_service_retry(f->service, id, targets[1], NULL));
  g_assert_cmpuint(f->opens, ==, 3); g_assert_cmpstr(f->wire, ==, wire);
  g_assert_cmpuint(f->signs, ==, 1);
  gh_relay_publish_ok(f->publish, targets[1], gh_relay_publish_get_event_id(f->publish), FALSE, "blocked: still refused");
  g_autoptr(GhStoreReport) retained = gh_store_report_load(f->store, id, NULL);
  g_assert_cmpstr(retained->state, ==, "accepted");
  g_assert_nonnull(strstr(retained->outcomes_json, targets[0]));
  g_assert_nonnull(strstr(retained->outcomes_json, targets[1]));
}
int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL); nostr_json_init();
  guint8 public_bytes[32];
  for (guint i = 0; i < 32; i++) {
    gchar byte[] = {ACCOUNT[2*i], ACCOUNT[2*i+1], 0};
    public_bytes[i] = strtoul(byte, NULL, 16);
  }
  g_assert_cmpint(nostr_nip19_encode_npub(public_bytes, &npub), ==, 0);
  g_test_add_func("/privacy/reports/no-private-evidence", no_private_evidence);
  g_test_add_func("/reports/known-public-metadata", metadata);
  g_test_add("/reports/storage-no-fallback", Fixture, NULL, setup, storage, teardown);
  g_test_add("/reports/persist-sign-deliver-retry", Fixture, NULL, setup, delivery, teardown);
  g_test_add("/reports/reject-signer-mutation", Fixture, NULL, setup, bad_signature, teardown);
  g_test_add("/reports/hide-failure-prevents-publication", Fixture, NULL, setup, hide_failure, teardown);
  g_test_add("/reports/cancellation-explicit-resume", Fixture, NULL, setup, cancellation, teardown);
  g_test_add("/reports/denied-after-sign", Fixture, NULL, setup, blocked_after_sign, teardown);
  g_test_add("/reports/public-post-ephemeral-auth", Fixture, NULL, setup, authentication, teardown);
  g_test_add("/reports/partial-outcomes-retained", Fixture, NULL, setup, partial, teardown);
  gint result = g_test_run(); g_free(npub); return result;
}
