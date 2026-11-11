/* Real account-controller -> private D-Bus signer -> NIP-44 + signed 21059,
 * on recording relay transports. No external relay or developer identities. */
#include "gh-call-service.h"
#include "gh-inbox-lookup.h"
#include "gh-relay-deny-policy.h"
#include "gh-relay-publish.h"
#include "fake-call-media.h"
#include "gh-test-signer.h"
#include <nostr/nip59/nip59.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <json-glib/json-glib.h>

static const gchar *GOOD = "wss://inbox.test.invalid";
static const gchar *BAD = "wss://denied.test.invalid";
typedef struct {
  GhTestBus bus;
  GhTestSigner signer;
  GSettings *settings;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhInboxLookup *resolver;
  GhCallSignalling *signalling;
  GhCallService *service;
  GhNetSession *network;
  GhFakeCallMedia *media;
  GPtrArray *scopes, *published;
  guint received, failed, closed, eose;
  gboolean no_inbox, reject;
} Network;
typedef struct {
  Network *net;
  GhRelayScope *scope;
  gchar *url, *filters;
  guint idle;
} Read;
typedef struct { GhRelayPublish *publish; gchar *url; guint idle; Network *net; } Write;
static gchar *list_event(guint key, int kind, const gchar *tags)
{
  g_autofree gchar *pub = gh_test_pub(key);
  g_autofree gchar *json = g_strdup_printf("{\"pubkey\":\"%s\",\"kind\":%d,\"created_at\":%" G_GINT64_FORMAT ",\"content\":\"\",\"tags\":%s}", pub, kind, g_get_real_time()/G_USEC_PER_SEC, tags);
  NostrEvent *e = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize_unsigned(e, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_sign(e, gh_test_secret[key]), ==, 0);
  gchar *out = nostr_event_serialize_compact(e); nostr_event_free(e); return out;
}
static gboolean read_ready(gpointer data)
{
  Read *r = data; r->idle = 0;
  GhRelayScope *scope = gh_relay_scope_ref(r->scope);
  g_autofree gchar *url = g_strdup(r->url);
  g_autoptr(JsonParser) p = json_parser_new(); g_assert_true(json_parser_load_from_data(p, r->filters, -1, NULL));
  JsonObject *f = json_node_get_object(json_parser_get_root(p));
  JsonArray *kinds = json_object_get_array_member(f, "kinds");
  int kind = json_array_get_int_element(kinds, 0);
  g_autofree gchar *json = NULL;
  if (kind != NOSTR_KIND_EPHEMERAL_GIFT_WRAP) {
    JsonArray *authors = json_object_get_array_member(f, "authors");
    g_autofree gchar *account = gh_test_pub(2);
    guint key = !g_strcmp0(json_array_get_string_element(authors, 0), account) ? 2 : 1;
    if (kind == NOSTR_KIND_RELAY_LIST_METADATA && key == 1)
      json = list_event(1, kind, "[[\"r\",\"wss://inbox.test.invalid\",\"read\"],[\"r\",\"wss://denied.test.invalid\",\"read\"]]");
    else if (!r->net->no_inbox || key == 2)
      json = list_event(key, NOSTR_KIND_DM_RELAY_LIST, "[[\"relay\",\"wss://inbox.test.invalid\"],[\"relay\",\"wss://denied.test.invalid\"]]");
  }
  if (json) gh_relay_scope_event(scope, url, json);
  gh_relay_scope_eose(scope, url); gh_relay_scope_unref(scope); return G_SOURCE_REMOVE;
}
static gpointer read_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data, GError **error)
{
  (void)error; Network *n = data;
  Read *r = g_new0(Read, 1); r->net = n; r->scope = gh_relay_scope_ref(scope);
  r->url = g_strdup(url); r->filters = nostr_filter_serialize_compact(&filters->filters[0]);
  if (strstr(r->filters, "21059")) g_assert_cmpstr(url, !=, BAD);
  g_ptr_array_add(n->scopes, r); r->idle = g_idle_add(read_ready, r); return r;
}
static void read_close(gpointer data, gpointer transport_data)
{
  (void)transport_data; Read *r = data;
  if (r->idle) g_source_remove(r->idle);
  g_ptr_array_remove(r->net->scopes, r); gh_relay_scope_unref(r->scope);
  g_free(r->url); g_free(r->filters); g_free(r);
}
static gboolean write_ready(gpointer data)
{
  Write *w = data; w->idle = 0;
  GhRelayPublish *p = gh_relay_publish_ref(w->publish);
  g_autofree gchar *url = g_strdup(w->url); gboolean reject = w->net->reject;
  gh_relay_publish_ok(p, url, gh_relay_publish_get_event_id(p), !reject, reject ? "blocked: fixture" : "");
  gh_relay_publish_unref(p); return G_SOURCE_REMOVE;
}
static gpointer write_open(GhRelayPublish *publish, const gchar *url, const gchar *json, gpointer data, GError **error)
{
  (void)error; Network *n = data; g_assert_cmpstr(url, ==, GOOD);
  g_ptr_array_add(n->published, g_strdup(json));
  Write *w = g_new0(Write, 1); w->publish = gh_relay_publish_ref(publish); w->url = g_strdup(url); w->net = n;
  w->idle = g_idle_add(write_ready, w); return w;
}
static void write_close(gpointer data, gpointer transport_data)
{
  (void)transport_data; Write *w = data;
  if (w->idle) g_source_remove(w->idle);
  gh_relay_publish_unref(w->publish); g_free(w->url); g_free(w);
}
static const GhRelayTransport reads = {read_open, read_close};
static const GhRelayPublishTransport writes = {write_open, write_close};
static GPtrArray *identities(gpointer data, GError **error)
{
  (void)data; (void)error; GPtrArray *a = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  for (guint i = 1; i <= 3; i++) { GhIdentityInfo *info = g_new0(GhIdentityInfo, 1); info->npub = gh_test_npub(i); info->label = g_strdup("Call fixture"); g_ptr_array_add(a, info); }
  return a;
}
static GPtrArray *empty(gpointer data, GError **error) { (void)data; (void)error; return g_ptr_array_new(); }
static gboolean account_ready(gpointer data) { return gh_account_controller_can_call_signer(((Network *)data)->accounts); }
static gboolean inbox_ready(gpointer data) { return gh_account_relays_get_inbox_relays(((Network *)data)->relays) != NULL; }
static gboolean published_one(gpointer data) { return ((Network *)data)->published->len > 0; }
static gboolean published_two(gpointer data) { return ((Network *)data)->published->len >= 2; }
static gboolean received_one(gpointer data) { return ((Network *)data)->received > 0; }
static gboolean held_one(gpointer data) { return ((Network *)data)->signer.held->len > 0; }
static gboolean failed_one(gpointer data) { return ((Network *)data)->failed > 0; }
static void got_event(GhCallSignalling *s, const gchar *json, gpointer data)
{ (void)s; (void)json; ((Network *)data)->received++; }
static void failed(GhCallSignalling *s, const gchar *id, gpointer data)
{ (void)s; (void)id; ((Network *)data)->failed++; }
static void relay_status(GhCallSignalling *s, int notice, const gchar *url, const gchar *detail, gpointer data)
{
  (void)s; (void)url; (void)detail; Network *n = data;
  if (notice == GH_RELAY_NOTICE_CLOSED) n->closed++;
  if (notice == GH_RELAY_NOTICE_EOSE) n->eose++;
}
static void setup(Network *n, gconstpointer data)
{
  (void)data; gh_test_bus_up(&n->bus); gh_test_signer_up(&n->bus, &n->signer);
  n->scopes = g_ptr_array_new(); n->published = g_ptr_array_new_with_free_func(g_free);
  gh_relay_scope_set_default_transport(&reads, NULL, n);
  gh_relay_publish_set_default_transport(&writes, NULL, n);
  n->settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { "wss://discovery.test.invalid", NULL };
  g_settings_set_strv(n->settings, "discovery-relays", sources);
  g_settings_set_string(n->settings, "current-backend", "grotto");
  g_autofree gchar *npub = gh_test_npub(2); g_settings_set_string(n->settings, "current-npub", npub);
  n->accounts = gh_account_controller_new_full_with_remote_list(n->settings, n->bus.client, identities, NULL, empty, NULL);
  gh_test_spin_until(account_ready, n);
  const gchar *denied[] = { BAD, NULL };
  gh_relay_deny_policy_bind(gh_relay_deny_policy_get_default(), gh_account_controller_get_generation(n->accounts), denied);
  n->relays = gh_account_relays_new(n->accounts, n->settings, NULL, NULL); gh_test_spin_until(inbox_ready, n);
  n->resolver = gh_inbox_lookup_new(n->accounts, n->settings, NULL, NULL);
  n->signalling = gh_call_signalling_new(n->accounts, n->relays, GH_INBOX_RESOLVER(n->resolver), n->settings);
  n->network = gh_net_session_new(NULL);
  n->service = gh_call_service_new(n->signalling, NULL, n->settings, n->network);
  gh_call_service_set_media_factory(n->service, gh_fake_call_media_factory, &n->media, NULL);
  g_signal_connect(n->signalling, "event", G_CALLBACK(got_event), n);
  g_signal_connect(n->signalling, "failed", G_CALLBACK(failed), n);
  g_signal_connect(n->signalling, "relay-status", G_CALLBACK(relay_status), n);
}
static void teardown(Network *n, gconstpointer data)
{
  (void)data; gh_call_service_stop(n->service); gh_test_release(n->service);
  gh_test_release(n->signalling); gh_test_release(n->resolver); gh_test_release(n->relays);
  gh_test_release(n->accounts); g_clear_object(&n->media); g_object_unref(n->network);
  g_assert_cmpuint(n->scopes->len, ==, 0); g_ptr_array_unref(n->scopes); g_ptr_array_unref(n->published);
  gh_relay_scope_set_default_transport(NULL, NULL, NULL); gh_relay_publish_set_default_transport(NULL, NULL, NULL);
  gh_relay_deny_policy_bind(gh_relay_deny_policy_get_default(), 0, NULL);
  g_object_unref(n->settings); gh_test_signer_down(&n->bus, &n->signer); gh_test_bus_down(&n->bus);
}
static void roundtrip(Network *n, gconstpointer data)
{
  (void)data; g_autofree gchar *peer = gh_test_pub(1), *me = gh_test_pub(2);
  GhCall *call = gh_call_service_place(n->service, peer, GH_CALL_VOICE, NULL); g_assert_nonnull(call);
  gh_test_spin_until(published_one, n);
  NostrEvent *outer = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(outer, n->published->pdata[0], NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_true(nostr_nip59_validate_gift_wrap_kind(outer, NOSTR_KIND_EPHEMERAL_GIFT_WRAP));
  NostrEvent *offer = nostr_nip59_unwrap_kind(outer, gh_test_secret[1], NOSTR_KIND_EPHEMERAL_GIFT_WRAP);
  g_assert_nonnull(offer); g_assert_cmpstr(nostr_event_get_pubkey(offer), ==, me);
  g_assert_cmpint(nostr_event_get_kind(offer), ==, NOSTR_KIND_CALL_OFFER);
  nostr_event_free(offer); nostr_event_free(outer);
  NostrEvent *answer = gh_call_event_new(peer, me, gh_call_get_id(call), NOSTR_KIND_CALL_ANSWER, GH_CALL_VOICE, "v=0\r\n", g_get_real_time()/G_USEC_PER_SEC);
  g_assert_cmpint(nostr_event_sign(answer, gh_test_secret[1]), ==, 0);
  outer = nostr_nip59_wrap_kind(answer, me, NOSTR_KIND_EPHEMERAL_GIFT_WRAP, NULL);
  g_autofree gchar *json = nostr_event_serialize_compact(outer); nostr_event_free(outer); nostr_event_free(answer);
  GhRelayScope *scope = NULL;
  for (guint i = 0; i < n->scopes->len; i++) { Read *r = n->scopes->pdata[i]; if (strstr(r->filters, "21059")) scope = r->scope; }
  g_assert_nonnull(scope); gh_relay_scope_event(scope, GOOD, json); gh_test_spin_until(received_one, n);
  g_assert_cmpint(gh_call_get_state(call), ==, GH_CALL_CONNECTING);
  gh_relay_scope_event(scope, GOOD, json);
  g_assert_cmpuint(n->received, ==, 1);
}
static void switch_pending(Network *n, gconstpointer data)
{
  (void)data; n->signer.hold = TRUE;
  g_autofree gchar *peer = gh_test_pub(1), *other = gh_test_npub(3);
  GhCall *call = gh_call_service_place(n->service, peer, GH_CALL_VOICE, NULL); g_assert_nonnull(call); g_object_ref(call);
  gh_test_spin_until(held_one, n);
  g_assert_true(gh_account_controller_select(n->accounts, other, NULL));
  gh_test_signer_release_all(&n->signer);
  g_assert_cmpint(gh_call_get_end_reason(call), ==, GH_CALL_END_ACCOUNT_CHANGED);
  g_assert_cmpuint(n->published->len, ==, 0); g_object_unref(call);
}
static void deny_pending(Network *n, gconstpointer data)
{
  (void)data; n->signer.hold = TRUE; g_autofree gchar *peer = gh_test_pub(1);
  GhCall *call = gh_call_service_place(n->service, peer, GH_CALL_VOICE, NULL); g_assert_nonnull(call);
  gh_test_spin_until(held_one, n);
  const gchar *denied[] = {GOOD, BAD, NULL};
  gh_relay_deny_policy_bind(gh_relay_deny_policy_get_default(), gh_account_controller_get_generation(n->accounts), denied);
  gh_test_signer_release_all(&n->signer);
  g_assert_cmpint(gh_call_get_end_reason(call), ==, GH_CALL_END_SIGNALLING_FAILED);
  g_assert_cmpuint(n->published->len, ==, 0);
  for (guint i = 0; i < n->scopes->len; i++) {
    Read *r = n->scopes->pdata[i]; g_assert_null(strstr(r->filters, "21059"));
  }
}
static void closed_status(Network *n, gconstpointer data)
{
  (void)data; GhRelayScope *scope = NULL;
  for (guint i = 0; i < n->scopes->len; i++) { Read *r = n->scopes->pdata[i]; if (strstr(r->filters, "21059")) scope = r->scope; }
  g_assert_nonnull(scope);
  gh_relay_scope_notice(scope, GOOD, GH_RELAY_NOTICE_CLOSED, NULL, FALSE, "restricted: fixture");
  g_assert_cmpuint(n->closed, ==, 1);
}
static void self_wrap(Network *n, gconstpointer data)
{
  (void)data;
  g_settings_set_boolean(n->settings, "allow-calls-from-unaccepted", TRUE);
  g_autofree gchar *peer = gh_test_pub(1), *me = gh_test_pub(2);
  const gchar *id = "123e4567-e89b-12d3-a456-426614174000";
  NostrEvent *offer = gh_call_event_new(peer, me, id, NOSTR_KIND_CALL_OFFER, GH_CALL_VOICE,
                                      "v=0\r\n", g_get_real_time()/G_USEC_PER_SEC);
  g_assert_cmpint(nostr_event_sign(offer, gh_test_secret[1]), ==, 0);
  g_autofree gchar *offer_json = nostr_event_serialize_compact(offer); nostr_event_free(offer);
  gh_call_service_receive(n->service, offer_json, g_get_real_time()/G_USEC_PER_SEC);
  g_autoptr(GhCallService) other_device = gh_call_service_new(NULL, NULL, n->settings, n->network);
  gh_call_service_bind(other_device, me);
  gh_call_service_receive(other_device, offer_json, g_get_real_time()/G_USEC_PER_SEC);
  guint signer_calls = n->signer.calls;
  gh_call_service_decline(n->service); gh_test_spin_until(published_two, n);
  g_assert_cmpuint(n->signer.calls, ==, signer_calls + 1);
  gboolean peer_seen = FALSE, self_seen = FALSE;
  g_autofree gchar *first_id = NULL;
  for (guint i = 0; i < n->published->len; i++) {
    NostrEvent *wrap = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_signed(wrap, n->published->pdata[i], NULL), ==, NOSTR_EVENT_VALIDATION_OK);
    gboolean own = !g_strcmp0(gh_call_event_tag(wrap, "p"), me);
    NostrEvent *inner = nostr_nip59_unwrap_kind(wrap, gh_test_secret[own ? 2 : 1], NOSTR_KIND_EPHEMERAL_GIFT_WRAP);
    g_assert_nonnull(inner); g_assert_cmpstr(gh_call_event_tag(inner, "p"), ==, peer);
    g_assert_cmpint(nostr_event_get_kind(inner), ==, NOSTR_KIND_CALL_REJECT);
    g_autofree gchar *event_id = nostr_event_get_id(inner);
    if (!first_id) first_id = g_strdup(event_id); else g_assert_cmpstr(first_id, ==, event_id);
    if (own) {
      self_seen = TRUE; g_autofree gchar *json = nostr_event_serialize_compact(inner);
      gh_call_service_receive(other_device, json, g_get_real_time()/G_USEC_PER_SEC);
    } else peer_seen = TRUE;
    nostr_event_free(inner); nostr_event_free(wrap);
  }
  g_assert_true(peer_seen); g_assert_true(self_seen);
  g_assert_cmpint(gh_call_get_end_reason(gh_call_service_get_current(other_device)), ==, GH_CALL_END_ELSEWHERE);
  gh_call_service_stop(other_device);
}
static void fallback_and_reject(Network *n, gconstpointer data)
{
  (void)data; n->no_inbox = TRUE; n->reject = TRUE;
  g_autofree gchar *peer = gh_test_pub(1);
  g_assert_nonnull(gh_call_service_place(n->service, peer, GH_CALL_VOICE, NULL));
  gh_test_spin_until(failed_one, n);
  g_assert_cmpuint(n->published->len, >, 0);
  g_assert_cmpint(gh_call_get_end_reason(gh_call_service_get_current(n->service)), ==, GH_CALL_END_SIGNALLING_FAILED);
}
void gh_test_register_call_transport(void)
{
  g_test_add("/calls/transport/deny-pending-and-subscribe", Network, NULL, setup, deny_pending, teardown);
  g_test_add("/calls/transport/closed-status", Network, NULL, setup, closed_status, teardown);
  g_test_add("/calls/transport/self-reject-signed-once", Network, NULL, setup, self_wrap, teardown);
  g_test_add("/calls/transport/signer-roundtrip-deny", Network, NULL, setup, roundtrip, teardown);
  g_test_add("/calls/transport/account-switch-held-signature", Network, NULL, setup, switch_pending, teardown);
  g_test_add("/calls/transport/nip65-fallback-ok-false", Network, NULL, setup, fallback_and_reject, teardown);
}
