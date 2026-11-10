/* Deterministic relay frames drive the real account controller, SQLCipher
 * storage, Groundhog adapter and list service. No helper substitutes for OK
 * or EOSE. The group-list suite separately exercises the real wire transport. */
#include "gh-social-lists.h"
#include "gh-social-lists-backend.h"
#include "gh-preferences-dialog.h"
#include "gh-relay-deny-policy.h"
#include "gh-test-signer.h"
#include "nostrc-test-gdk-frame.h"
void groundhog_register_resource(void);
#include <glib/gstdio.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <jansson.h>

static GhTestBus bus;
static gchar *account, *npub, *other_npub;
static const gchar *writes[] = { "wss://write-a.test.invalid", "wss://write-b.test.invalid", "wss://write-c.test.invalid", NULL };
typedef struct { GhRelayScope *scope; gchar *url; gint kind; gboolean closed; gchar *filter; } Read;
typedef struct { GhRelayPublish *publish; gchar *url, *json, *id, *auth_id, *auth_pubkey; gint kind; gboolean closed; guint resends; } Write;
typedef struct {
  GhTestSigner signer;
  GSettings *settings;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhSocialLists *social;
  GhStore *store;
  gchar *directory;
  GPtrArray *reads, *sent;
} Fixture;
static void drain(void) { while (g_main_context_iteration(NULL, FALSE)); }
static void remove_tree(const gchar *path)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *name;
    while (dir && (name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      remove_tree(child);
    }
    if (dir) g_dir_close(dir);
    g_rmdir(path);
  } else g_unlink(path);
}
static void read_free(gpointer p) { Read *r = p; g_free(r->url); g_free(r->filter); g_free(r); }
static void write_free(gpointer p) { Write *w = p; g_free(w->url); g_free(w->json); g_free(w->id); g_free(w->auth_id); g_free(w->auth_pubkey); g_free(w); }
static gpointer read_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data, GError **error)
{
  (void)error;
  Fixture *f = data;
  g_assert_cmpuint(filters->count, ==, 1);
  Read *r = g_new0(Read, 1);
  r->scope = scope; r->url = g_strdup(url); r->kind = filters->filters[0].kinds.data[0];
  char *raw = nostr_filter_serialize_compact(&filters->filters[0]);
  r->filter = g_strdup(raw); free(raw);
  g_ptr_array_add(f->reads, r);
  return r;
}
static void read_close(gpointer handle, gpointer data) { (void)data; ((Read *)handle)->closed = TRUE; }
static gpointer write_open(GhRelayPublish *publish, const gchar *url, const gchar *json, gpointer data, GError **error)
{
  (void)error;
  Fixture *f = data;
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  Write *w = g_new0(Write, 1);
  w->publish = publish; w->url = g_strdup(url); w->json = g_strdup(json);
  w->id = nostr_event_get_id(event); w->kind = nostr_event_get_kind(event);
  nostr_event_free(event);
  g_ptr_array_add(f->sent, w);
  return w;
}
static void write_close(gpointer handle, gpointer data) { (void)data; ((Write *)handle)->closed = TRUE; }
static gboolean write_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  (void)data; (void)error;
  Write *w = handle;
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(event), ==, NOSTR_KIND_CLIENT_AUTHENTICATION);
  g_free(w->auth_id); w->auth_id = nostr_event_get_id(event);
  g_free(w->auth_pubkey); w->auth_pubkey = g_strdup(nostr_event_get_pubkey(event));
  nostr_event_free(event);
  return TRUE;
}
static gboolean write_resend(gpointer handle, gpointer data, GError **error)
{ (void)data; (void)error; ((Write *)handle)->resends++; return TRUE; }
static const GhRelayPublishAuthTransport write_auth_transport = { write_auth, write_resend };
static const GhRelayTransport read_transport = { read_open, read_close };
static const GhRelayPublishTransport write_transport = { write_open, write_close };
static gchar *event_json(gint kind, gint64 at, NostrTags *tags, const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind); nostr_event_set_created_at(event, at);
  nostr_event_set_pubkey(event, account); nostr_event_set_tags(event, tags);
  nostr_event_set_content(event, content ? content : "");
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[1]), ==, 0);
  char *raw = nostr_event_serialize_compact(event);
  gchar *json = g_strdup(raw); free(raw); nostr_event_free(event);
  return json;
}
static gchar *encrypt_private(const gchar *plain)
{
  guint8 secret[32], pubkey[32]; char *cipher = NULL;
  g_assert_true(nostr_hex2bin(secret, gh_test_secret[1], 32));
  g_assert_true(nostr_hex2bin(pubkey, account, 32));
  g_assert_cmpint(nostr_nip44_encrypt_v2(secret, pubkey, (const guint8 *)plain, strlen(plain), &cipher), ==, 0);
  gchar *result = g_strdup(cipher); free(cipher); return result;
}
static GPtrArray *identities(gpointer data, GError **error)
{
  (void)data; (void)error;
  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  const gchar *keys[] = { npub, other_npub };
  for (guint i = 0; i < 2; i++) {
    GhIdentityInfo *id = g_new0(GhIdentityInfo, 1); id->npub = g_strdup(keys[i]);
    g_ptr_array_add(result, id);
  }
  return result;
}
static gboolean active(gpointer data) { return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE; }
static gboolean discovered(gpointer data) { return gh_account_relays_get_state(data) == GH_ACCOUNT_RELAYS_COMPLETE; }
static Read *read_for(Fixture *f, gint kind, const gchar *url)
{
  for (guint i = f->reads->len; i > 0; i--) {
    Read *r = f->reads->pdata[i - 1];
    if (!r->closed && r->kind == kind && !g_strcmp0(r->url, url)) return r;
  }
  g_error("Missing read for kind %d", kind);
  return NULL;
}
static void bind(Fixture *f)
{
  f->social = gh_social_lists_new(NULL);
  GhListAdapterNetwork network = { .accounts = f->accounts, .scope_transport = &read_transport,
    .scope_data = f, .publish_transport = &write_transport, .publish_auth_transport = &write_auth_transport,
    .publish_data = f, .publish_deadline = 10 };
  gh_social_lists_set_network(f->social, &network, f->relays);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_social_lists_bind_store(f->social, f->store,
    gh_account_controller_get_generation(f->accounts), &error));
  g_assert_no_error(error);
  drain();
}
static void open_store(Fixture *f)
{
  guint8 key[GH_STORE_KEY_SIZE] = { 1 };
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { f->directory, account, NULL, NULL, NULL };
  g_autoptr(GError) error = NULL;
  f->store = gh_store_open_with_key(&config, bytes, "7ddaf388-5abc-49e6-ae17-07ca5777dbf0", GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
}
static void setup(Fixture *f)
{
  memset(f, 0, sizeof *f);
  f->reads = g_ptr_array_new_with_free_func(read_free); f->sent = g_ptr_array_new_with_free_func(write_free);
  gh_test_signer_up(&bus, &f->signer);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub);
  const gchar *discovery[] = { "wss://discovery.test.invalid", NULL };
  g_settings_set_strv(f->settings, "discovery-relays", discovery);
  f->accounts = gh_account_controller_new_full(f->settings, bus.client, identities, NULL);
  gh_test_spin_until(active, f->accounts);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &read_transport, f);
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; writes[i]; i++) nostr_tags_append(tags, nostr_tag_new("r", writes[i], "write", NULL));
  g_autofree gchar *list = event_json(NOSTR_KIND_RELAY_LIST_METADATA, g_get_real_time()/G_USEC_PER_SEC - 60, tags, "");
  Read *disc = read_for(f, NOSTR_KIND_RELAY_LIST_METADATA, discovery[0]);
  gh_relay_scope_event(disc->scope, disc->url, list); gh_relay_scope_eose(disc->scope, disc->url);
  gh_test_spin_until(discovered, f->relays);
  f->directory = g_dir_make_tmp("groundhog-social-sync-XXXXXX", NULL);
  open_store(f); bind(f);
}
static void teardown(Fixture *f)
{
  gh_social_lists_close(f->social); gh_test_release(g_steal_pointer(&f->social));
  drain(); if (f->store) gh_store_close(f->store);
  gh_test_release(g_steal_pointer(&f->relays)); gh_test_release(g_steal_pointer(&f->accounts));
  GhTestSenders senders = { &bus, &f->signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &senders);
  gh_test_signer_down(&bus, &f->signer);
  g_object_unref(f->settings); g_ptr_array_unref(f->reads); g_ptr_array_unref(f->sent);
  remove_tree(f->directory); g_free(f->directory);
}
static void enabled(Fixture *f, gboolean value)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_social_lists_set_sync_enabled(f->social, value, &error)); g_assert_no_error(error); drain();
}
static void eose(Fixture *f, gint kind, guint count)
{
  for (guint i = 0; i < count; i++) { Read *r = read_for(f, kind, writes[i]); gh_relay_scope_eose(r->scope, r->url); }
  drain();
}
static void append(Fixture *f, gint kind, gboolean priv, const gchar *name, const gchar *value)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GNostrListDocument) snapshot = gh_social_lists_ensure(f->social, kind, "", &error);
  g_assert_no_error(error);
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  const gchar *fields[] = { name, value };
  g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, priv, fields, 2, 0, 0));
  g_autoptr(GNostrListDocument) saved = gh_social_lists_apply(f->social, kind, "",
    gnostr_list_document_get_local_revision(snapshot), ops, &error);
  g_assert_no_error(error); g_assert_nonnull(saved); drain();
}
static gboolean any_sent(gpointer data) { return ((Fixture *)data)->sent->len > 0; }
static gboolean sent(gpointer data) { return ((Fixture *)data)->sent->len >= 3; }
static void accept_writes(Fixture *f)
{
  for (guint i = 0; i < f->sent->len; i++) {
    Write *w = f->sent->pdata[i];
    if (!w->closed) gh_relay_publish_ok(w->publish, w->url, w->id, TRUE, "");
  }
  drain();
}
static void test_default_majority(void)
{
  Fixture f; setup(&f);
  guint reads = f.reads->len, calls = f.signer.calls;
  append(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, TRUE, "relay", "wss://blocked.test.invalid");
  g_assert_cmpuint(f.reads->len, ==, reads); g_assert_cmpuint(f.sent->len, ==, 0);
  gh_test_signer_assert_no_calls_since(&f.signer, calls);
  enabled(&f, TRUE);
  Read *r = read_for(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, writes[0]);
  g_assert_nonnull(strstr(r->filter, account)); g_assert_nonnull(strstr(r->filter, "\"limit\":1"));
  eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 1); g_assert_cmpuint(f.sent->len, ==, 0);
  eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 2); gh_test_spin_until(sent, &f);
  for (guint i = 0; i < f.sent->len; i++) {
    Write *w = f.sent->pdata[i]; g_assert_cmpint(w->kind, ==, NOSTR_KIND_BLOCKED_RELAY_LIST);
    g_assert_true(g_strv_contains(writes, w->url));
    g_assert_null(strstr(w->json, "blocked.test.invalid"));
  }
  accept_writes(&f);
  enabled(&f, FALSE);
  calls = f.signer.calls;
  append(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, TRUE, "relay", "wss://another-blocked.test.invalid");
  g_assert_cmpuint(f.sent->len, ==, 3);
  gh_test_signer_assert_no_calls_since(&f.signer, calls);
  teardown(&f);
}
static void test_own_hide_echo(void)
{
  Fixture f; setup(&f); enabled(&f, TRUE);
  append(&f, NOSTR_KIND_MUTE_LIST, TRUE, "p", account);
  eose(&f, NOSTR_KIND_MUTE_LIST, 2); gh_test_spin_until(sent, &f);
  Write *w = f.sent->pdata[0];
  /* Relay serialization differs from the signer's exact JSON, but not its ID. */
  json_t *event = json_loads(w->json, 0, NULL);
  gchar *echo = json_dumps(event, JSON_INDENT(2)); json_decref(event);
  Read *r = read_for(&f, NOSTR_KIND_MUTE_LIST, writes[0]);
  gh_relay_scope_event(r->scope, r->url, echo); free(echo); drain();
  accept_writes(&f);
  g_assert_false(gh_social_lists_get_pending_import(f.social, NULL, NULL));
  g_autoptr(GNostrListDocument) snapshot = gnostr_list_service_lookup(gh_social_lists_get_service(f.social), NOSTR_KIND_MUTE_LIST, "");
  g_assert_false(gnostr_list_document_get_dirty(snapshot));
  teardown(&f);
}
static gchar *remote_hide(void)
{
  g_autofree gchar *plain = g_strdup_printf("[[\"p\",\"%s\"],[\"word\",\"test\"]]", account);
  g_autofree gchar *cipher = encrypt_private(plain);
  return event_json(NOSTR_KIND_MUTE_LIST, g_get_real_time()/G_USEC_PER_SEC - 10, nostr_tags_new(0), cipher);
}
static gboolean import_pending(gpointer data) { return gh_social_lists_get_pending_import(((Fixture *)data)->social, NULL, NULL); }
static gboolean hide_applied(gpointer data)
{
  Fixture *f = data;
  g_autoptr(GNostrListDocument) snapshot = gnostr_list_service_lookup(gh_social_lists_get_service(f->social), NOSTR_KIND_MUTE_LIST, "");
  return nostr_nip51_document_tag_count(gnostr_list_document_get_codec(snapshot), TRUE) == 2;
}
static void test_first_import(gconstpointer data)
{
  gboolean accept = GPOINTER_TO_INT(data);
  Fixture f; setup(&f); enabled(&f, TRUE);
  g_autofree gchar *wire = remote_hide();
  Read *r = read_for(&f, NOSTR_KIND_MUTE_LIST, writes[0]);
  gh_relay_scope_event(r->scope, r->url, wire); eose(&f, NOSTR_KIND_MUTE_LIST, 3);
  gh_test_spin_until(import_pending, &f);
  guint accounts = 0, items = 0;
  g_assert_true(gh_social_lists_get_pending_import(f.social, &accounts, &items));
  g_assert_cmpuint(accounts, ==, 1); g_assert_cmpuint(items, ==, 1); g_assert_false(hide_applied(&f));
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_social_lists_confirm_import(f.social, accept, &error)); g_assert_no_error(error);
  if (accept) gh_test_spin_until(hide_applied, &f); else drain();
  g_assert_true(gh_social_lists_get_sync_enabled(f.social));
  g_assert_cmpint(hide_applied(&f), ==, accept);
  gh_social_lists_close(f.social);
  for (guint i = 0; i < f.sent->len; i++) g_assert_true(((Write *)f.sent->pdata[i])->closed);
  gh_test_release(g_steal_pointer(&f.social)); drain();
  gh_store_close(f.store); f.store = NULL; open_store(&f); bind(&f);
  g_assert_cmpint(hide_applied(&f), ==, accept);
  g_assert_false(import_pending(&f));
  if (!accept) {
    append(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, TRUE, "relay", "wss://blocked.test.invalid");
    eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 2); gh_test_spin_until(sent, &f); accept_writes(&f);
  }
  teardown(&f);
}
static void test_public_import_blocks_local_publish(void)
{
  Fixture f; setup(&f); enabled(&f, TRUE);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", account, NULL));
  g_autofree gchar *wire = event_json(NOSTR_KIND_MUTE_LIST, g_get_real_time()/G_USEC_PER_SEC - 10, tags, "");
  Read *r = read_for(&f, NOSTR_KIND_MUTE_LIST, writes[0]);
  gh_relay_scope_event(r->scope, r->url, wire); eose(&f, NOSTR_KIND_MUTE_LIST, 2);
  gh_test_spin_until(import_pending, &f);
  guint calls = f.signer.calls;
  append(&f, NOSTR_KIND_MUTE_LIST, TRUE, "word", "local-change");
  g_assert_cmpuint(f.sent->len, ==, 0);
  gh_test_signer_assert_no_calls_since(&f.signer, calls);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_social_lists_confirm_import(f.social, TRUE, &error)); g_assert_no_error(error);
  gh_test_spin_until(sent, &f); accept_writes(&f);
  g_assert_false(import_pending(&f)); teardown(&f);
}
typedef struct { gboolean done; GError *error; GVariant *outcomes; } Result;
static void published(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *r = data;
  r->outcomes = gnostr_list_service_publish_finish(GNOSTR_LIST_SERVICE(source), result, &r->error); r->done = TRUE;
}
static gboolean finished(gpointer data) { return ((Result *)data)->done; }
static void test_public_and_partial(void)
{
  Fixture f; setup(&f); enabled(&f, TRUE);
  append(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, FALSE, "relay", "wss://public.test.invalid");
  eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 3); drain();
  g_assert_cmpuint(f.sent->len, ==, 0); /* sync opt-in never approves public edits */
  enabled(&f, FALSE); enabled(&f, TRUE);
  eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 1);
  Result r = {0};
  GNostrListService *service = gh_social_lists_get_service(f.social);
  gnostr_list_service_publish_async(service, NOSTR_KIND_BLOCKED_RELAY_LIST, "", GNOSTR_LIST_APPROVE_PUBLIC, NULL, published, &r);
  gh_test_spin_until(finished, &r); g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&r.error);
  r.done = FALSE;
  gnostr_list_service_publish_async(service, NOSTR_KIND_BLOCKED_RELAY_LIST, "",
    GNOSTR_LIST_APPROVE_PUBLIC | GNOSTR_LIST_APPROVE_PARTIAL, NULL, published, &r);
  gh_test_spin_until(sent, &f); accept_writes(&f); gh_test_spin_until(finished, &r);
  g_assert_no_error(r.error); g_assert_nonnull(r.outcomes); g_variant_unref(r.outcomes);
  teardown(&f);
}
static gboolean auth_sent(gpointer data) { return ((Write *)data)->auth_pubkey != NULL; }
static void test_server_list_explicit(void)
{
  Fixture f; setup(&f);
  const gchar *servers[] = { "https://first.test.invalid", "https://second.test.invalid", NULL };
  guint calls = f.signer.calls;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_social_lists_set_servers(f.social, servers, &error)); g_assert_no_error(error);
  enabled(&f, TRUE); eose(&f, NOSTR_KIND_BLOSSOM_SERVER_LIST, 3);
  gh_test_signer_assert_no_calls_since(&f.signer, calls);
  g_assert_cmpuint(f.sent->len, ==, 0); /* the sync switch never authorizes 10063 */
  enabled(&f, FALSE);
  Result result = {0};
  GNostrListService *service = gh_social_lists_get_service(f.social);
  gnostr_list_service_publish_async(service, NOSTR_KIND_BLOSSOM_SERVER_LIST, "", GNOSTR_LIST_APPROVE_PARTIAL, NULL, published, &result);
  gh_test_spin_until(finished, &result);
  g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&result.error);
  g_assert_cmpuint(f.sent->len, ==, 0);
  result.done = FALSE;
  gnostr_list_service_publish_async(service, NOSTR_KIND_BLOSSOM_SERVER_LIST, "",
    GNOSTR_LIST_APPROVE_PUBLIC | GNOSTR_LIST_APPROVE_PARTIAL, NULL, published, &result);
  gh_test_spin_until(sent, &f);
  for (guint i = 0; i < f.sent->len; i++) {
    Write *w = f.sent->pdata[i];
    g_assert_cmpint(w->kind, ==, NOSTR_KIND_BLOSSOM_SERVER_LIST);
    g_assert_cmpstr(w->url, ==, writes[i]);
    json_t *event = json_loads(w->json, 0, NULL);
    g_assert_cmpstr(json_string_value(json_object_get(event, "content")), ==, "");
    json_t *tags = json_object_get(event, "tags");
    g_assert_cmpstr(json_string_value(json_array_get(json_array_get(tags, 0), 1)), ==, servers[0]);
    g_assert_cmpstr(json_string_value(json_array_get(json_array_get(tags, 1), 1)), ==, servers[1]);
    json_decref(event);
  }
  Write *w = f.sent->pdata[0];
  calls = f.signer.calls;
  gh_relay_publish_auth_challenge(w->publish, w->url, "public-server-list");
  gh_relay_publish_ok(w->publish, w->url, w->id, FALSE, "auth-required: sign in");
  gh_test_spin_until(auth_sent, w);
  g_assert_cmpstr(w->auth_pubkey, !=, account); /* PUBLIC_POST, never OWN_LIST_PUBLISH */
  gh_test_signer_assert_no_calls_since(&f.signer, calls);
  gh_relay_publish_ok(w->publish, w->url, w->auth_id, TRUE, "");
  g_assert_cmpuint(w->resends, ==, 1);
  accept_writes(&f); gh_test_spin_until(finished, &result);
  g_assert_no_error(result.error); g_assert_nonnull(result.outcomes); g_variant_unref(result.outcomes);
  teardown(&f);
}

static gboolean unreadable(gpointer data)
{
  return gnostr_list_service_get_state(gh_social_lists_get_service(((Fixture *)data)->social), NOSTR_KIND_BLOCKED_RELAY_LIST, "") == GNOSTR_LIST_UNREADABLE;
}
static void test_unreadable(void)
{
  Fixture f; setup(&f); enabled(&f, TRUE);
  append(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, TRUE, "relay", "wss://retained.test.invalid");
  g_autofree gchar *wire = event_json(NOSTR_KIND_BLOCKED_RELAY_LIST, g_get_real_time()/G_USEC_PER_SEC,
                                     nostr_tags_new(0), "invalid-ciphertext");
  Read *r = read_for(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, writes[0]);
  gh_relay_scope_event(r->scope, r->url, wire); eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 3);
  gh_test_spin_until(unreadable, &f);
  g_autoptr(GNostrListDocument) snapshot = gnostr_list_service_lookup(gh_social_lists_get_service(f.social), NOSTR_KIND_BLOCKED_RELAY_LIST, "");
  g_assert_cmpuint(nostr_nip51_document_tag_count(gnostr_list_document_get_codec(snapshot), TRUE), ==, 1);
  g_assert_cmpuint(f.sent->len, ==, 0); teardown(&f);
}
static gboolean held(gpointer data) { return ((Fixture *)data)->signer.held->len > 0; }
static void test_account_teardown(void)
{
  Fixture f; setup(&f); enabled(&f, TRUE); f.signer.hold = TRUE;
  append(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, TRUE, "relay", "wss://blocked.test.invalid");
  eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 2); gh_test_spin_until(held, &f);
  g_settings_set_string(f.settings, "current-npub", other_npub); drain();
  gh_social_lists_close(f.social); gh_store_close(f.store); f.store = NULL;
  f.signer.hold = FALSE; gh_test_signer_release_all(&f.signer); drain();
  g_assert_cmpuint(f.sent->len, ==, 0);
  teardown(&f);
}
static void test_disable_pending_publish(void)
{
  Fixture f; setup(&f); enabled(&f, TRUE);
  append(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, TRUE, "relay", "wss://blocked.test.invalid");
  eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 2); gh_test_spin_until(sent, &f);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_social_lists_set_sync_enabled(f.social, FALSE, &error)); g_assert_no_error(error);
  /* Before iterating again: old sockets are already gone, not queued for close. */
  for (guint i = 0; i < f.sent->len; i++) g_assert_true(((Write *)f.sent->pdata[i])->closed);
  g_autofree gchar *state = gh_store_social_load(f.store, NOSTR_KIND_BLOCKED_RELAY_LIST, "", &error);
  g_assert_no_error(error);
  json_t *root = json_loads(state, 0, NULL);
  g_assert_cmpstr(json_string_value(json_object_get(root, "signed")), ==, ""); json_decref(root);
  g_autoptr(GNostrListDocument) snapshot = gnostr_list_service_lookup(gh_social_lists_get_service(f.social), NOSTR_KIND_BLOCKED_RELAY_LIST, "");
  g_assert_true(gnostr_list_document_get_dirty(snapshot));
  drain(); teardown(&f);
}
static gboolean resumed(gpointer data) { return ((Fixture *)data)->sent->len >= 6; }
static void test_restart_resume(void)
{
  Fixture f; setup(&f); enabled(&f, TRUE);
  append(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, TRUE, "relay", "wss://blocked.test.invalid");
  eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 2); gh_test_spin_until(sent, &f);
  g_autofree gchar *wire = g_strdup(((Write *)f.sent->pdata[0])->json);
  guint calls = f.signer.calls;
  gh_social_lists_close(f.social);
  for (guint i = 0; i < f.sent->len; i++) g_assert_true(((Write *)f.sent->pdata[i])->closed);
  gh_test_release(g_steal_pointer(&f.social)); drain();
  gh_store_close(f.store); f.store = NULL; open_store(&f); bind(&f);
  eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 2); gh_test_spin_until(resumed, &f);
  gh_test_signer_assert_no_calls_since(&f.signer, calls);
  for (guint i = 3; i < f.sent->len; i++) g_assert_cmpstr(((Write *)f.sent->pdata[i])->json, ==, wire);
  accept_writes(&f); teardown(&f);
}
static GPtrArray *group_projection(gpointer data, GNostrListDocument *snapshot, GError **error)
{
  (void)data; (void)error;
  if (gnostr_list_document_get_kind(snapshot) != NOSTR_KIND_SIMPLE_GROUP_LIST) return NULL;
  GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  if (!nostr_nip51_document_tag_count(gnostr_list_document_get_codec(snapshot), TRUE)) {
    const gchar *fields[] = { "group", "fixture", "wss://group.test.invalid" };
    g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, TRUE, fields, 3, 0, 0));
  }
  return ops;
}
static void test_groups_all_sources(void)
{
  Fixture f; setup(&f);
  gh_social_lists_configure_groups(f.social, writes, FALSE, group_projection, NULL);
  drain();
  eose(&f, NOSTR_KIND_SIMPLE_GROUP_LIST, 2);
  g_assert_cmpuint(f.sent->len, ==, 0);
  eose(&f, NOSTR_KIND_SIMPLE_GROUP_LIST, 3); gh_test_spin_until(sent, &f);
  g_assert_false(gh_social_lists_get_sync_enabled(f.social));
  guint reads = f.reads->len;
  enabled(&f, TRUE);
  guint after_on = f.reads->len;
  enabled(&f, FALSE);
  g_assert_cmpuint(f.reads->len, ==, after_on); /* no group re-REQ on either toggle */
  g_assert_cmpuint(after_on, >, reads); /* ordinary sync did open its own scopes */
  for (guint i = 0; i < f.sent->len; i++) {
    g_assert_cmpint(((Write *)f.sent->pdata[i])->kind, ==, NOSTR_KIND_SIMPLE_GROUP_LIST);
    g_assert_false(((Write *)f.sent->pdata[i])->closed); /* in-flight group publish survives */
  }
  accept_writes(&f); teardown(&f);
}
typedef struct { GhListAdapter *adapter; Result result; } AdapterResult;
static void adapter_published(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  AdapterResult *r = data;
  r->result.outcomes = gh_list_adapter_get_callbacks()->publish_finish(r->adapter, result, &r->result.error);
  r->result.done = TRUE;
}
static void test_private_auth_destinations(void)
{
  Fixture f; setup(&f); enabled(&f, TRUE);
  const gchar *private[] = { "wss://private.test.invalid", NULL };
  append(&f, NOSTR_KIND_PRIVATE_RELAY_LIST, TRUE, "relay", private[0]);
  g_autofree gchar *cipher = encrypt_private("{\"kind\":14,\"content\":\"draft\"}");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("d", "opaque-fixture", NULL));
  nostr_tags_append(tags, nostr_tag_new("k", "14", NULL));
  g_autofree gchar *wire = event_json(NOSTR_KIND_DRAFT_WRAP, g_get_real_time()/G_USEC_PER_SEC, tags, cipher);
  GhListAdapter *adapter = gh_social_lists_get_adapter(f.social);
  const GNostrListAdapter *callbacks = gh_list_adapter_get_callbacks();
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  AdapterResult r = { .adapter = adapter };
  callbacks->publish_async(adapter, wire, writes, "own-private-sync-publish", cancel, adapter_published, &r);
  gh_test_spin_until(finished, &r.result);
  g_assert_error(r.result.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&r.result.error);
  g_assert_cmpuint(f.sent->len, ==, 0);
  GNostrListReadPlanEntry plan = { .kind = NOSTR_KIND_DRAFT_WRAP, .identifier = "", .sources = writes,
    .read_purpose = "own-private-sync-read" };
  g_autoptr(GError) error = NULL;
  g_assert_null(callbacks->open_subscription(adapter, gh_social_lists_get_service(f.social), account,
    gh_social_lists_get_generation(f.social), gnostr_list_service_get_read_serial(gh_social_lists_get_service(f.social)), &plan, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&error);
  plan.sources = private;
  gpointer subscription = callbacks->open_subscription(adapter, gh_social_lists_get_service(f.social), account,
    gh_social_lists_get_generation(f.social), gnostr_list_service_get_read_serial(gh_social_lists_get_service(f.social)), &plan, &error);
  g_assert_no_error(error); g_assert_nonnull(subscription); callbacks->close_subscription(adapter, subscription);
  r.result.done = FALSE;
  callbacks->publish_async(adapter, wire, private, "own-private-sync-publish", cancel, adapter_published, &r);
  /* A single private target, rather than the three write-list targets. */
  gh_test_spin_until(any_sent, &f);
  g_assert_cmpstr(((Write *)f.sent->pdata[0])->url, ==, private[0]);
  accept_writes(&f); gh_test_spin_until(finished, &r.result);
  g_assert_no_error(r.result.error); g_variant_unref(r.result.outcomes);
  teardown(&f);
}

static GtkWidget *find_alert(GtkWidget *widget)
{
  if (ADW_IS_ALERT_DIALOG(widget)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_alert(child);
    if (found) return found;
  }
  return NULL;
}
static gboolean alert_shown(gpointer data) { return find_alert(data) != NULL; }
static void set_write_relays(Fixture *f, const gchar *const *urls)
{
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; urls && urls[i]; i++) nostr_tags_append(tags, nostr_tag_new("r", urls[i], "write", NULL));
  g_autofree gchar *wire = event_json(NOSTR_KIND_RELAY_LIST_METADATA, g_get_real_time()/G_USEC_PER_SEC, tags, "");
  Read *r = read_for(f, NOSTR_KIND_RELAY_LIST_METADATA, "wss://discovery.test.invalid");
  gh_relay_scope_event(r->scope, r->url, wire);
  drain();
}
static void test_preferences_server_publish(gconstpointer data)
{
  gint mode = GPOINTER_TO_INT(data); /* cancel, publish, edit, target change, account close, no targets */
  Fixture f; setup(&f);
  const gchar *servers[] = { "https://first.test.invalid", "https://second.test.invalid", NULL };
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_social_lists_set_servers(f.social, servers, &error)); g_assert_no_error(error);
  if (mode == 5) set_write_relays(&f, NULL);
  GhListsBackend backend = gh_social_lists_backend(f.social);
  GhPreferencesDialog *prefs = g_object_ref_sink(gh_preferences_dialog_new(f.settings, GH_PREFERENCES_FEATURE_ATTACHMENTS));
  gh_preferences_dialog_set_social_backend(prefs, &backend);
  GtkWindow *window = GTK_WINDOW(adw_window_new());
  gtk_window_set_default_size(window, 920, 640); gtk_window_present(window);
  adw_dialog_present(ADW_DIALOG(prefs), GTK_WIDGET(window));
  adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(prefs), "messages");
  GtkWidget *button = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "server_publish_button"));
  GtkWidget *row = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "server_publish_row"));
  guint calls = f.signer.calls;
  g_assert_cmpint(gtk_widget_get_sensitive(button), ==, mode != 5);
  g_signal_emit_by_name(button, "clicked");
  if (mode != 5) {
    gh_test_spin_until(alert_shown, window);
    AdwAlertDialog *dialog = g_object_ref(ADW_ALERT_DIALOG(find_alert(GTK_WIDGET(window))));
    g_assert_cmpstr(adw_alert_dialog_get_heading(dialog), ==, "Publish Server List?");
    g_assert_cmpstr(adw_alert_dialog_get_default_response(dialog), ==, "cancel");
    const gchar *body = adw_alert_dialog_get_body(dialog);
    g_assert_nonnull(strstr(body, "may replace relay changes not checked"));
    g_assert_nonnull(strstr(body, servers[0])); g_assert_nonnull(strstr(body, servers[1]));
    g_assert_true(strstr(body, servers[0]) < strstr(body, servers[1]));
    for (guint i = 0; writes[i]; i++) g_assert_nonnull(strstr(body, writes[i]));
    gh_test_signer_assert_no_calls_since(&f.signer, calls);
    g_assert_cmpuint(f.sent->len, ==, 0); /* opening the preview is not permission */
    if (mode == 2) {
      const gchar *changed[] = { "https://changed.test.invalid", NULL };
      g_assert_true(gh_social_lists_set_servers(f.social, changed, &error)); g_assert_no_error(error);
    } else if (mode == 3) {
      const gchar *changed[] = { "wss://changed-write.test.invalid", NULL };
      set_write_relays(&f, changed);
    } else if (mode == 4) gh_social_lists_close(f.social);
    g_signal_emit_by_name(dialog, "response", mode == 0 ? "cancel" : "publish");
    adw_dialog_force_close(ADW_DIALOG(dialog)); g_object_unref(dialog);
    if (mode == 1) {
      gh_test_spin_until(sent, &f);
      g_assert_false(gh_social_lists_get_sync_enabled(f.social));
      for (guint i = 0; i < f.sent->len; i++) g_assert_cmpint(((Write *)f.sent->pdata[i])->kind, ==, NOSTR_KIND_BLOSSOM_SERVER_LIST);
      accept_writes(&f);
      g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), "Published —"));
    } else if (mode == 2 || mode == 3) {
      g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), "changed"));
    }
  } else g_assert_null(find_alert(GTK_WIDGET(window)));
  if (mode != 1) {
    drain(); g_assert_cmpuint(f.sent->len, ==, 0);
    gh_test_signer_assert_no_calls_since(&f.signer, calls);
  }
  adw_dialog_force_close(ADW_DIALOG(prefs)); gtk_window_destroy(window); g_object_unref(prefs); drain();
  teardown(&f);
}
static void test_preferences_blocked_sync(void)
{
  Fixture f; setup(&f);
  GhListsBackend backend = gh_social_lists_backend(f.social);
  GhPreferencesDialog *prefs = g_object_ref_sink(gh_preferences_dialog_new(f.settings, 0));
  gh_preferences_dialog_set_social_backend(prefs, &backend);
  GtkWindow *window = GTK_WINDOW(adw_window_new());
  gtk_window_present(window); adw_dialog_present(ADW_DIALOG(prefs), GTK_WIDGET(window));
  GtkWidget *entry = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "blocked_relays_entry"));
  gtk_editable_set_text(GTK_EDITABLE(entry), "wss://blocked-editor.test.invalid/path");
  g_signal_emit_by_name(entry, "apply"); drain();
  g_assert_cmpuint(f.sent->len, ==, 0);
  g_assert_false(gh_relay_deny_policy_check(gh_social_lists_get_generation(f.social), "wss://blocked-editor.test.invalid/path", FALSE, NULL));
  GtkWidget *sync = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "social_sync_row"));
  adw_switch_row_set_active(ADW_SWITCH_ROW(sync), TRUE);
  eose(&f, NOSTR_KIND_BLOCKED_RELAY_LIST, 2); gh_test_spin_until(sent, &f);
  for (guint i = 0; i < f.sent->len; i++) {
    Write *w = f.sent->pdata[i];
    g_assert_cmpint(w->kind, ==, NOSTR_KIND_BLOCKED_RELAY_LIST);
    g_assert_true(g_strv_contains(writes, w->url));
    g_assert_null(strstr(w->json, "blocked-editor.test.invalid"));
  }
  accept_writes(&f);
  adw_dialog_force_close(ADW_DIALOG(prefs)); gtk_window_destroy(window); g_object_unref(prefs); drain();
  teardown(&f);
}

static void test_preferences_import(gconstpointer data)
{
  gboolean accept = GPOINTER_TO_INT(data);
  Fixture f; setup(&f);
  GhListsBackend backend = gh_social_lists_backend(f.social);
  GhPreferencesDialog *prefs = g_object_ref_sink(gh_preferences_dialog_new(f.settings, 0));
  gh_preferences_dialog_set_social_backend(prefs, &backend);
  GtkWindow *window = GTK_WINDOW(adw_window_new());
  gtk_window_set_default_size(window, 920, 640); gtk_window_present(window);
  adw_dialog_present(ADW_DIALOG(prefs), GTK_WIDGET(window));
  adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(prefs), "account");
  GtkWidget *sync = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "social_sync_row"));
  adw_switch_row_set_active(ADW_SWITCH_ROW(sync), TRUE);
  g_assert_true(gh_social_lists_get_sync_enabled(f.social));
  g_autofree gchar *wire = remote_hide();
  Read *r = read_for(&f, NOSTR_KIND_MUTE_LIST, writes[0]);
  gh_relay_scope_event(r->scope, r->url, wire); eose(&f, NOSTR_KIND_MUTE_LIST, 3);
  gh_test_spin_until(import_pending, &f);
  GtkWidget *review = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "social_import_row"));
  g_assert_true(gtk_widget_get_visible(review));
  g_signal_emit_by_name(review, "activated");
  gh_test_spin_until(alert_shown, window);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(find_alert(GTK_WIDGET(window)));
  g_assert_cmpstr(adw_alert_dialog_get_heading(dialog), ==, "Apply Hidden Items?");
  g_signal_emit_by_name(dialog, "response", accept ? "apply" : "cancel");
  adw_dialog_force_close(ADW_DIALOG(dialog));
  if (accept) gh_test_spin_until(hide_applied, &f); else drain();
  g_assert_cmpint(hide_applied(&f), ==, accept);
  g_assert_false(import_pending(&f));
  g_assert_true(gh_social_lists_get_sync_enabled(f.social));
  adw_dialog_force_close(ADW_DIALOG(prefs)); gtk_window_destroy(window); g_object_unref(prefs); drain();
  teardown(&f);
}

int main(int argc, char **argv)
{
  gboolean gui = argc > 1 && !g_strcmp0(argv[1], "--gui");
  if (gui) { argv[1] = NULL; argc = 1; }
  g_test_init(&argc, &argv, NULL);
  account = gh_test_pub(1); npub = gh_test_npub(1); other_npub = gh_test_npub(2);
  if (gui) {
    nostrc_test_tolerate_gdk_frame_warning();
    if (!gtk_init_check()) {
      g_free(account); g_free(npub); g_free(other_npub);
      return 77;
    }
    adw_init();
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    groundhog_register_resource();
    gh_test_bus_up_beside_gtk(&bus);
  } else gh_test_bus_up(&bus);
  if (gui) {
    g_test_add_data_func("/groundhog/social-sync-gui/server-publish-cancel", GINT_TO_POINTER(0), test_preferences_server_publish);
    g_test_add_data_func("/groundhog/social-sync-gui/server-publish-approved", GINT_TO_POINTER(1), test_preferences_server_publish);
    g_test_add_data_func("/groundhog/social-sync-gui/server-publish-stale-list", GINT_TO_POINTER(2), test_preferences_server_publish);
    g_test_add_data_func("/groundhog/social-sync-gui/server-publish-stale-targets", GINT_TO_POINTER(3), test_preferences_server_publish);
    g_test_add_data_func("/groundhog/social-sync-gui/server-publish-account-close", GINT_TO_POINTER(4), test_preferences_server_publish);
    g_test_add_data_func("/groundhog/social-sync-gui/server-publish-no-targets", GINT_TO_POINTER(5), test_preferences_server_publish);
    g_test_add_func("/groundhog/social-sync-gui/blocked-editor-sync", test_preferences_blocked_sync);
    g_test_add_data_func("/groundhog/social-sync-gui/import-accept", GINT_TO_POINTER(TRUE), test_preferences_import);
    g_test_add_data_func("/groundhog/social-sync-gui/import-decline", GINT_TO_POINTER(FALSE), test_preferences_import);
  } else {
  g_test_add_func("/privacy/social/default-no-sync", test_default_majority);
  g_test_add_func("/privacy/social/publication-needs-confirmation", test_public_and_partial);
  g_test_add_func("/privacy/social/server-list-explicit-public-post", test_server_list_explicit);
  g_test_add_func("/groundhog/social-sync/own-hide-echo", test_own_hide_echo);
  g_test_add_func("/groundhog/social-sync/public-import-blocks-local-publish", test_public_import_blocks_local_publish);
  g_test_add_data_func("/groundhog/social-sync/first-import-accept", GINT_TO_POINTER(TRUE), test_first_import);
  g_test_add_data_func("/groundhog/social-sync/first-import-decline", GINT_TO_POINTER(FALSE), test_first_import);
  g_test_add_func("/groundhog/social-sync/unreadable", test_unreadable);
  g_test_add_func("/privacy/social/account-teardown", test_account_teardown);
  g_test_add_func("/groundhog/social-sync/restart-resume", test_restart_resume);
  g_test_add_func("/groundhog/social-sync/disable-pending-publish", test_disable_pending_publish);
  g_test_add_func("/groundhog/social-sync/groups-all-sources-switch-off", test_groups_all_sources);
  g_test_add_func("/privacy/drafts/private-auth-purpose", test_private_auth_destinations);
  }
  gint result = g_test_run();
  gh_test_bus_down(&bus); g_free(account); g_free(npub); g_free(other_npub);
  return result;
}
