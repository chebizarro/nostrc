/* NIP-17 send pipeline: recipient 10050 lookup, per-wrap relay targeting,
 * aggregate status, and generation/cancel revocation. Relay traffic runs
 * through recording GhRelayScope/GhRelayPublish transports (and, with
 * libsoup, real local WebSocket relays); signer operations run against a mock
 * org.nostr.Signer on a private test bus. Waits iterate the main context and
 * their deadlines are failure bounds only. */
#include "gh-dm-send.h"
#include "gh-inbox-lookup.h"
#include "gh-identity.h"
#include "gh-signer.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-tag.h"
#include "nostr-utils.h"
#include "nostr/nip17/nip17.h"
#include "nostr/nip19/nip19.h"
#include "nostr/nip44/nip44.h"
#include "nostr/nip59/nip59.h"
#include "nostrc-test-bus.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GROUNDHOG_TEST_WIRE
#include "../relay/wire-relay.h"
#endif

#define SECRET_ALICE "0000000000000000000000000000000000000000000000000000000000000001"
#define SECRET_BOB   "0000000000000000000000000000000000000000000000000000000000000002"
#define SECRET_CAROL "0000000000000000000000000000000000000000000000000000000000000003"
#define DISC        "wss://discovery.test.invalid"
#define DISC2       "wss://discovery-2.test.invalid"
#define OWN_READ    "wss://alice-read.test.invalid"
#define OWN_WRITE   "wss://alice-write.test.invalid"
#define ALICE_INBOX "wss://alice-inbox.test.invalid"
#define BOB_A       "wss://bob-inbox-a.test.invalid"
#define BOB_B       "wss://bob-inbox-b.test.invalid"
#define UNRELATED   "wss://unrelated.test.invalid"

static gchar *npub_alice, *npub_bob, *hex_alice, *hex_bob, *hex_carol;

/* ---- generic helpers ----------------------------------------------------- */

static gchar *
npub_for_secret(const gchar *secret)
{
  g_autofree gchar *hex = nostr_key_get_public(secret);
  guint8 bytes[32];
  g_assert_nonnull(hex);
  for (guint i = 0; i < 32; i++) {
    unsigned int value;
    g_assert_cmpint(sscanf(hex + 2 * i, "%2x", &value), ==, 1);
    bytes[i] = value;
  }
  gchar *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  return npub;
}

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  const gchar *npubs[] = { npub_alice, npub_bob };
  for (guint i = 0; i < G_N_ELEMENTS(npubs); i++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npubs[i]);
    info->label = g_strdup(i ? "Bob" : "Alice");
    g_ptr_array_add(ids, info);
  }
  return ids;
}

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static gboolean
spin_tick(gpointer data)
{
  (void)data;
  return G_SOURCE_CONTINUE;
}

static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(15, deadline_hit, &expired);
  /* The tick re-checks pred when nothing else would wake the loop
   * (nostrc-qp24.8.5): a GTask drops its source object on its worker thread
   * after queueing the callback, so a ref-count or weak-pointer condition
   * can turn true with no main-context event, and a condition asked of the
   * bus synchronously (has a sender disconnected?) has none either. */
  guint tick = g_timeout_add(10, spin_tick, NULL);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
  if (expired)
    g_error("condition waited for at line %d did not hold within 15s", line);
  g_source_remove(timer);
}
#define spin_until(pred, data) spin_until_at((pred), (data), __LINE__)

/* Runs everything already dispatchable, without blocking. */
static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static gboolean
is_null(gpointer data)
{
  return *(gpointer *)data == NULL;
}

static void
release(gpointer object)
{
  gpointer weak = object;
  g_object_add_weak_pointer(G_OBJECT(object), &weak);
  g_object_run_dispose(G_OBJECT(object));
  g_object_unref(object);
  spin_until(is_null, &weak);
}

static gchar *
signed_event(const gchar *secret, int kind, gint64 created_at, NostrTags *tags,
             gchar **out_id)
{
  NostrEvent *event = nostr_event_new();
  g_assert_nonnull(event);
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, secret), ==, 0);
  if (out_id) {
    char *id = nostr_event_get_id(event);
    *out_id = g_strdup(id);
    free(id);
  }
  char *json = nostr_event_serialize_compact(event);
  g_assert_nonnull(json);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

/* A kind-10050 list of "relay" tags, NULL-terminated URLs. */
static gchar *
inbox_list_v(const gchar *secret, gint64 created_at, gchar **out_id, va_list args)
{
  NostrTags *tags = nostr_tags_new(0);
  for (const gchar *url = va_arg(args, const gchar *); url; url = va_arg(args, const gchar *))
    nostr_tags_append(tags, nostr_tag_new("relay", url, NULL));
  return signed_event(secret, 10050, created_at, tags, out_id);
}

static gchar *
inbox_list(const gchar *secret, gint64 created_at, ...)
{
  va_list args;
  va_start(args, created_at);
  gchar *json = inbox_list_v(secret, created_at, NULL, args);
  va_end(args);
  return json;
}

static gchar *
inbox_list_id(const gchar *secret, gint64 created_at, gchar **out_id, ...)
{
  va_list args;
  va_start(args, out_id);
  gchar *json = inbox_list_v(secret, created_at, out_id, args);
  va_end(args);
  return json;
}

static gchar *
nip65_list(const gchar *secret, gint64 created_at, const gchar *read, const gchar *write)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("r", read, "read", NULL));
  nostr_tags_append(tags, nostr_tag_new("r", write, "write", NULL));
  return signed_event(secret, 10002, created_at, tags, NULL);
}

/* ---- mock org.nostr.Signer ----------------------------------------------- */

typedef struct {
  NostrcTestBus *bus;
  GDBusConnection *client; /* owned by bus */
  GDBusConnection *owner;  /* owned by bus */
} BusFixture;

typedef struct {
  GDBusNodeInfo *node;
  guint registration;
  GPtrArray *held;
  GPtrArray *senders;
  guint calls;
  guint hold_call;
  guint deny_call;
} MockSigner;

static const gchar *
secret_for(const gchar *npub)
{
  return g_strcmp0(npub, npub_bob) == 0 ? SECRET_BOB : SECRET_ALICE;
}

static void
mock_signer_call(GDBusConnection *connection, const gchar *sender, const gchar *path,
                 const gchar *interface, const gchar *method, GVariant *parameters,
                 GDBusMethodInvocation *invocation, gpointer user_data)
{
  MockSigner *mock = user_data;
  (void)connection; (void)path; (void)interface;
  if (g_str_equal(method, "EnableTypedApprovalErrors")) {
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }
  const gchar *input, *second, *third;
  g_variant_get(parameters, "(&s&s&s)", &input, &second, &third);
  mock->calls++;
  g_ptr_array_add(mock->senders, g_strdup(sender));
  if (mock->hold_call == mock->calls) {
    g_ptr_array_add(mock->held, g_object_ref(invocation));
    return;
  }
  if (mock->deny_call == mock->calls) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.ApprovalDenied", "test denial");
    return;
  }
  if (g_str_equal(method, "SignEvent")) {
    /* (unsigned event, npub, app) */
    NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_compact(event, input, NULL), ==, 1);
    g_assert_cmpint(nostr_event_sign(event, secret_for(second)), ==, 0);
    gchar *signed_json = nostr_event_serialize_compact(event);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", signed_json));
    free(signed_json);
    nostr_event_free(event);
  } else if (g_str_equal(method, "NIP44Encrypt")) {
    /* (plaintext, peer, npub) */
    guint8 sk[32], pk[32];
    g_assert_true(nostr_hex2bin(sk, secret_for(third), sizeof sk));
    g_assert_true(nostr_hex2bin(pk, second, sizeof pk));
    char *ciphertext = NULL;
    g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)input, strlen(input),
                                           &ciphertext), ==, 0);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", ciphertext));
    free(ciphertext);
  } else {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.Failed", "not used by the send pipeline");
  }
}

static const GDBusInterfaceVTable mock_vtable = { mock_signer_call, NULL, NULL, { 0 } };

/* One private test bus for the whole binary (tests/common/nostrc-test-bus.h:
 * closing GDBus connections races GDBus worker polls on macOS, a fatal
 * "poll(2) failed" warning); only the mock object is per fixture. Every case
 * is added with nostrc_test_bus_add_func(), which arms that tolerance for
 * the case (GTest clears it between cases). */
static BusFixture shared_bus;

static void
shared_bus_up(void)
{
  g_autoptr(GError) error = NULL;
  shared_bus.bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(shared_bus.bus);
  shared_bus.client = nostrc_test_bus_connect(shared_bus.bus);
  shared_bus.owner = nostrc_test_bus_connect(shared_bus.bus);
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(shared_bus.owner,
    "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "RequestName", g_variant_new("(su)", "org.nostr.Signer", 4u),
    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
  g_assert_no_error(error);
}

static void
shared_bus_down(void)
{
  nostrc_test_bus_down(shared_bus.bus);
  shared_bus = (BusFixture){ 0 };
}

static void
bus_up(BusFixture *fixture, MockSigner *mock)
{
  g_autoptr(GError) error = NULL;
  *fixture = shared_bus;
  mock->held = g_ptr_array_new_with_free_func(g_object_unref);
  mock->senders = g_ptr_array_new_with_free_func(g_free);
  mock->node = g_dbus_node_info_new_for_xml(
    "<node><interface name='org.nostr.Signer'>"
    "<method name='EnableTypedApprovalErrors'/>"
    "<method name='SignEvent'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='NIP44Encrypt'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='NIP44Decrypt'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "</interface></node>", &error);
  g_assert_no_error(error);
  mock->registration = g_dbus_connection_register_object(fixture->owner,
    "/org/nostr/signer", mock->node->interfaces[0], &mock_vtable, mock, NULL, &error);
  g_assert_no_error(error);
}

static void
bus_down(BusFixture *fixture, MockSigner *mock)
{
  while (mock->held->len) {
    g_dbus_method_invocation_return_dbus_error(g_ptr_array_index(mock->held, 0),
      "org.nostr.Signer.Error.ApprovalDenied", "test cleanup");
    g_ptr_array_remove_index(mock->held, 0);
  }
  g_dbus_connection_unregister_object(fixture->owner, mock->registration);
  g_ptr_array_unref(mock->held);
  g_ptr_array_unref(mock->senders);
  g_dbus_node_info_unref(mock->node);
  memset(fixture, 0, sizeof *fixture);
}

typedef struct {
  BusFixture *bus;
  MockSigner *mock;
} SenderCheck;

/* Every private signer connection is gone: pending approvals are revoked. */
static gboolean
signer_senders_closed(gpointer data)
{
  SenderCheck *check = data;
  for (guint i = 0; i < check->mock->senders->len; i++) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(check->bus->client,
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "NameHasOwner", g_variant_new("(s)", g_ptr_array_index(check->mock->senders, i)),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_assert_no_error(error);
    gboolean has_owner;
    g_variant_get(reply, "(b)", &has_owner);
    if (has_owner)
      return FALSE;
  }
  return TRUE;
}

typedef struct {
  MockSigner *mock;
  guint count;
} CallCount;

static gboolean
calls_reached(gpointer data)
{
  CallCount *want = data;
  return want->mock->calls >= want->count;
}

/* ---- recording relay transports ------------------------------------------ */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gchar *author;
  gboolean lookup; /* kinds [10050] (recipient lookup) vs [10002,10050] (own lists) */
  gboolean closed;
} ScopeOpen;

static void
scope_open_free(gpointer data)
{
  ScopeOpen *open = data;
  gh_relay_scope_unref(open->scope);
  g_free(open->url);
  g_free(open->author);
  g_free(open);
}

static gpointer
scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
           gpointer data, GError **error)
{
  GPtrArray *opens = data;
  (void)error;
  g_assert_cmpuint(filters->count, ==, 1);
  const NostrFilter *filter = &filters->filters[0];
  g_assert_cmpuint(nostr_filter_authors_len(filter), ==, 1);
  ScopeOpen *open = g_new0(ScopeOpen, 1);
  open->scope = gh_relay_scope_ref(scope);
  open->url = g_strdup(url);
  open->author = g_strdup(nostr_filter_authors_get(filter, 0));
  if (nostr_filter_kinds_len(filter) == 1) {
    g_assert_cmpint(nostr_filter_kinds_get(filter, 0), ==, 10050);
    open->lookup = TRUE;
  } else {
    g_assert_cmpuint(nostr_filter_kinds_len(filter), ==, 2);
  }
  g_ptr_array_add(opens, open);
  return open;
}

static void
scope_close(gpointer handle, gpointer data)
{
  (void)data;
  ScopeOpen *open = handle;
  g_assert_false(open->closed);
  open->closed = TRUE;
}

static const GhRelayTransport scope_transport = { scope_open, scope_close };

typedef struct {
  GhRelayPublish *publish;
  gchar *url;
  gchar *p;        /* the wrap's single recipient p tag */
  gchar *event_id;
  gchar *event_json; /* exactly what the transport was asked to send */
  gboolean closed;
} PubOpen;

static void
pub_open_free(gpointer data)
{
  PubOpen *open = data;
  gh_relay_publish_unref(open->publish);
  g_free(open->url);
  g_free(open->p);
  g_free(open->event_id);
  g_free(open->event_json);
  g_free(open);
}

static gpointer
pub_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
         gpointer data, GError **error)
{
  GPtrArray *opens = data;
  (void)error;
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, event_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 1059);
  g_assert_true(nostr_nip59_validate_gift_wrap(event));
  PubOpen *open = g_new0(PubOpen, 1);
  char *p = nostr_nip59_get_recipient(event);
  open->p = g_strdup(p);
  free(p);
  nostr_event_free(event);
  open->publish = gh_relay_publish_ref(publish);
  open->url = g_strdup(url);
  open->event_id = g_strdup(gh_relay_publish_get_event_id(publish));
  open->event_json = g_strdup(event_json);
  g_ptr_array_add(opens, open);
  return open;
}

static void
pub_close(gpointer handle, gpointer data)
{
  (void)data;
  PubOpen *open = handle;
  g_assert_false(open->closed);
  open->closed = TRUE;
}

static const GhRelayPublishTransport pub_transport = { pub_open, pub_close };

/* ---- fixture ------------------------------------------------------------- */

typedef struct {
  BusFixture bus;
  MockSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GPtrArray *scopes; /* ScopeOpen */
  GPtrArray *pubs;   /* PubOpen */
  GhAccountRelays *relays;
  GhInboxLookup *lookup;
  GhDmSender *sender;
} Fixture;

static void
fixture_up(Fixture *f)
{
  bus_up(&f->bus, &f->mock);
  f->settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { DISC, DISC2, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub_alice);
  f->accounts = gh_account_controller_new_full(f->settings, f->bus.client, fake_list, NULL);
  spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->scopes = g_ptr_array_new_with_free_func(scope_open_free);
  f->pubs = g_ptr_array_new_with_free_func(pub_open_free);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &scope_transport, f->scopes);
  f->lookup = gh_inbox_lookup_new(f->accounts, f->settings, &scope_transport, f->scopes);
  f->sender = gh_dm_sender_new(f->accounts, f->relays, GH_INBOX_RESOLVER(f->lookup), &pub_transport, f->pubs);
}

static void
fixture_down(Fixture *f)
{
  release(f->sender);
  release(f->lookup);
  release(f->relays);
  release(f->accounts);
  SenderCheck check = { &f->bus, &f->mock };
  spin_until(signer_senders_closed, &check);
  /* Nothing is left open on any relay. */
  for (guint i = 0; i < f->scopes->len; i++)
    g_assert_true(((ScopeOpen *)g_ptr_array_index(f->scopes, i))->closed);
  for (guint i = 0; i < f->pubs->len; i++)
    g_assert_true(((PubOpen *)g_ptr_array_index(f->pubs, i))->closed);
  g_ptr_array_unref(f->pubs);
  g_ptr_array_unref(f->scopes);
  g_object_unref(f->settings);
  bus_down(&f->bus, &f->mock);
}

static ScopeOpen *
own_open(Fixture *f, const gchar *author)
{
  for (guint i = f->scopes->len; i > 0; i--) {
    ScopeOpen *open = g_ptr_array_index(f->scopes, i - 1);
    if (!open->lookup && !open->closed && g_strcmp0(open->author, author) == 0)
      return open;
  }
  g_assert_not_reached();
}

/* The account's own relay-list discovery answers on its discovery sources.
 * Its NIP-65 read/write relays are never recipient-lookup sources. */
static void
settle_own(Fixture *f, gboolean with_inbox)
{
  ScopeOpen *open = own_open(f, hex_alice);
  g_assert_true(g_str_equal(open->url, DISC) || g_str_equal(open->url, DISC2));
  g_autofree gchar *nip65 = nip65_list(SECRET_ALICE, 100, OWN_READ, OWN_WRITE);
  gh_relay_scope_event(open->scope, DISC, nip65);
  if (with_inbox) {
    g_autofree gchar *inbox = inbox_list(SECRET_ALICE, 100, ALICE_INBOX, NULL);
    gh_relay_scope_event(open->scope, DISC, inbox);
  }
  gh_relay_scope_eose(open->scope, DISC);
  gh_relay_scope_eose(open->scope, DISC2);
  g_assert_cmpint(gh_account_relays_get_state(f->relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);
}

static guint
lookup_opens(Fixture *f)
{
  guint count = 0;
  for (guint i = 0; i < f->scopes->len; i++)
    count += ((ScopeOpen *)g_ptr_array_index(f->scopes, i))->lookup;
  return count;
}

static gboolean
lookup_started(gpointer data)
{
  Fixture *f = data;
  for (guint i = 0; i < f->scopes->len; i++) {
    ScopeOpen *open = g_ptr_array_index(f->scopes, i);
    if (open->lookup && !open->closed)
      return TRUE;
  }
  return FALSE;
}

/* The open lookup REQs: exactly the discovery-relays (in any order), each
 * for bob, all on one scope. Never the account's own NIP-65 read/write or
 * 10050 relays, which would learn whom it is about to message (charter
 * §4.3, PD-12). */
static GhRelayScope *
lookup_scope(Fixture *f)
{
  GhRelayScope *scope = NULL;
  const gchar *want[] = { DISC, DISC2 };
  guint seen = 0, n = 0;
  for (guint i = 0; i < f->scopes->len; i++) {
    ScopeOpen *open = g_ptr_array_index(f->scopes, i);
    if (!open->lookup || open->closed)
      continue;
    guint j = 0;
    while (j < G_N_ELEMENTS(want) && g_strcmp0(open->url, want[j]) != 0)
      j++;
    g_assert_cmpuint(j, <, G_N_ELEMENTS(want));
    g_assert_false(seen & (1u << j));
    seen |= 1u << j;
    n++;
    g_assert_cmpstr(open->author, ==, hex_bob);
    g_assert_true(!scope || scope == open->scope);
    scope = open->scope;
  }
  g_assert_cmpuint(n, ==, G_N_ELEMENTS(want));
  return scope;
}

static void
eose_all(GhRelayScope *scope)
{
  gh_relay_scope_eose(scope, DISC);
  gh_relay_scope_eose(scope, DISC2);
}

/* Serves an optional list from the discovery relay, then every source EOSEs. */
static void
answer_lookup(Fixture *f, const gchar *event_json)
{
  spin_until(lookup_started, f);
  GhRelayScope *scope = lookup_scope(f);
  if (event_json)
    gh_relay_scope_event(scope, DISC, event_json);
  eose_all(scope);
}

static gboolean
all_lookups_closed(Fixture *f)
{
  for (guint i = 0; i < f->scopes->len; i++) {
    ScopeOpen *open = g_ptr_array_index(f->scopes, i);
    if (open->lookup && !open->closed)
      return FALSE;
  }
  return TRUE;
}

static PubOpen *
pub_find(Fixture *f, const gchar *p, const gchar *url)
{
  PubOpen *found = NULL;
  for (guint i = 0; i < f->pubs->len; i++) {
    PubOpen *open = g_ptr_array_index(f->pubs, i);
    if (g_strcmp0(open->p, p) == 0 && g_strcmp0(open->url, url) == 0)
      found = open;
  }
  g_assert_nonnull(found);
  return found;
}

/* The wrap addressed to p went to exactly these relays (open, in order). */
static void
assert_pub_urls(Fixture *f, const gchar *p, ...)
{
  va_list args;
  va_start(args, p);
  guint n = 0;
  const gchar *want = va_arg(args, const gchar *);
  for (guint i = 0; i < f->pubs->len; i++) {
    PubOpen *open = g_ptr_array_index(f->pubs, i);
    if (g_strcmp0(open->p, p) != 0 || open->closed)
      continue;
    g_assert_nonnull(want);
    g_assert_cmpstr(open->url, ==, want);
    want = va_arg(args, const gchar *);
    n++;
  }
  va_end(args);
  g_assert_null(want);
  (void)n;
}

static guint
open_pubs(Fixture *f)
{
  guint count = 0;
  for (guint i = 0; i < f->pubs->len; i++)
    count += !((PubOpen *)g_ptr_array_index(f->pubs, i))->closed;
  return count;
}

static void
relay_ok(Fixture *f, const gchar *p, const gchar *url, gboolean accepted,
         const gchar *message)
{
  PubOpen *open = pub_find(f, p, url);
  gh_relay_publish_ok(open->publish, url, open->event_id, accepted, message);
}

static void
relay_failed(Fixture *f, const gchar *p, const gchar *url)
{
  PubOpen *open = pub_find(f, p, url);
  gh_relay_publish_failed(open->publish, url, "connection refused");
}

static gboolean
send_done(gpointer data)
{
  return gh_dm_send_is_done(data);
}

static gboolean
send_publishing(gpointer data)
{
  return gh_dm_send_get_status(data)->phase >= GH_DM_SEND_PHASE_PUBLISHING;
}

#define RLEG(status) ((GhDmSendLeg *)g_ptr_array_index((status)->recipients, 0))

static const GhDmRelayOutcome *
outcome_at(const GhDmSendLeg *leg, guint index)
{
  g_assert_cmpuint(index, <, leg->relays->len);
  return g_ptr_array_index(leg->relays, index);
}

static void
record_phase(GhDmSend *send, gpointer data)
{
  GArray *phases = data;
  GhDmSendPhase phase = gh_dm_send_get_status(send)->phase;
  if (!phases->len || g_array_index(phases, GhDmSendPhase, phases->len - 1) != phase)
    g_array_append_val(phases, phase);
}

/* Unwraps a published wrap with the receiver's key and checks the rumor. */
static void
assert_wrap_carries(const gchar *wrap_json, const gchar *receiver_secret,
                    const GhDmSendStatus *status)
{
  NostrEvent *wrap = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(wrap, wrap_json, NULL), ==, 1);
  g_assert_cmpstr(nostr_event_get_pubkey(wrap), !=, hex_alice); /* ephemeral key */
  NostrEvent *seal = nostr_nip59_unwrap(wrap, receiver_secret);
  g_assert_nonnull(seal);
  g_assert_cmpstr(nostr_event_get_pubkey(seal), ==, hex_alice);
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, receiver_secret, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, hex_alice, sizeof pk));
  guint8 *plaintext = NULL;
  size_t length = 0;
  g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, nostr_event_get_content(seal),
                                         &plaintext, &length), ==, 0);
  g_assert_cmpmem(plaintext, length, status->rumor_json, strlen(status->rumor_json));
  free(plaintext);
  nostr_event_free(seal);
  nostr_event_free(wrap);
}

/* ---- tests --------------------------------------------------------------- */

static void
test_happy_path(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autoptr(GArray) phases = g_array_new(FALSE, FALSE, sizeof(GhDmSendPhase));
  GhDmSend *send = gh_dm_sender_send(f.sender, hex_bob, "hello bob", NULL);
  g_signal_connect(send, "changed", G_CALLBACK(record_phase), phases);
  const GhDmSendStatus *status = gh_dm_send_get_status(send);

  /* The send waits for the account's own lists (its self-copy targets). */
  drain();
  g_assert_cmpint(status->phase, ==, GH_DM_SEND_PHASE_RESOLVING);
  g_assert_cmpuint(lookup_opens(&f), ==, 0);
  settle_own(&f, TRUE);
  spin_until(lookup_started, &f);
  g_assert_cmpuint(lookup_opens(&f), ==, 2);

  /* Bob's list: invalid and duplicate entries are dropped. */
  g_autofree gchar *list_id = NULL;
  g_autofree gchar *list = inbox_list_id(SECRET_BOB, 100, &list_id, BOB_A,
                                         "https://not-a-relay.test.invalid", BOB_B, BOB_A,
                                         NULL);
  answer_lookup(&f, list);
  spin_until(send_publishing, send);
  g_assert_true(all_lookups_closed(&f));
  g_assert_cmpuint(f.mock.calls, ==, 4);
  g_assert_cmpint(RLEG(status)->inbox, ==, GH_INBOX_FOUND);
  g_assert_cmpstr(RLEG(status)->inbox_event_id, ==, list_id);

  /* Disjoint targets: recipient wrap only to bob's inbox, self-copy only to
   * alice's inbox; nothing anywhere else. */
  g_assert_cmpuint(f.pubs->len, ==, 3);
  assert_pub_urls(&f, hex_bob, BOB_A, BOB_B, NULL);
  assert_pub_urls(&f, hex_alice, ALICE_INBOX, NULL);
  g_assert_cmpstr(pub_find(&f, hex_bob, BOB_A)->event_id, ==, RLEG(status)->wrap_id);
  g_assert_cmpstr(pub_find(&f, hex_alice, ALICE_INBOX)->event_id, ==,
                  status->self_copy->wrap_id);
  assert_wrap_carries(RLEG(status)->wrap_json, SECRET_BOB, status);
  assert_wrap_carries(status->self_copy->wrap_json, SECRET_ALICE, status);
  g_assert_nonnull(strstr(status->rumor_json, "hello bob"));
  g_assert_cmpuint(strlen(status->rumor_id), ==, 64);
  g_assert_nonnull(strstr(status->rumor_json, status->rumor_id));
  g_assert_cmpint(outcome_at(RLEG(status), 0)->outcome, ==,
                  GH_RELAY_PUBLISH_PENDING);

  g_assert_cmpint(gh_dm_send_status_get_message_status(status), ==, GH_MESSAGE_STATUS_SENDING);
  relay_ok(&f, hex_bob, BOB_A, TRUE, "");
  g_assert_false(gh_dm_send_is_done(send));
  /* The recipient is reached: the message counts as sent while the rest of
   * the fanout is still reporting. */
  g_assert_cmpint(gh_dm_send_status_get_message_status(status), ==, GH_MESSAGE_STATUS_SENT);
  relay_ok(&f, hex_bob, BOB_B, TRUE, "duplicate: already have it");
  relay_ok(&f, hex_alice, ALICE_INBOX, TRUE, "");
  g_assert_true(gh_dm_send_is_done(send));
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_SENT);
  g_assert_cmpint(status->failure, ==, GH_DM_SEND_FAILURE_NONE);
  g_assert_cmpint(status->flags, ==, GH_DM_SEND_FLAG_NONE);
  g_assert_cmpint(gh_dm_send_status_get_message_status(status), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_true(gh_dm_send_status_self_copy_stored(status));
  g_assert_cmpuint(status->recipients->len, ==, 1);
  g_assert_cmpstr(RLEG(status)->pubkey, ==, hex_bob);
  g_assert_cmpstr(status->self_copy->pubkey, ==, hex_alice);
  g_assert_cmpint(status->self_copy->inbox, ==, GH_INBOX_FOUND);
  g_assert_cmpuint(RLEG(status)->accepted, ==, 2);
  g_assert_cmpuint(status->self_copy->accepted, ==, 1);
  g_assert_cmpint(outcome_at(RLEG(status), 1)->prefix, ==,
                  GH_RELAY_OK_PREFIX_DUPLICATE);
  g_assert_cmpuint(open_pubs(&f), ==, 0);

  GhDmSendPhase want[] = { GH_DM_SEND_PHASE_RESOLVING, GH_DM_SEND_PHASE_SEALING,
                           GH_DM_SEND_PHASE_PUBLISHING, GH_DM_SEND_PHASE_DONE };
  g_assert_cmpmem(phases->data, phases->len * sizeof(GhDmSendPhase), want, sizeof want);

  /* The status is plain data an outbox can keep. */
  g_autoptr(GhDmSendStatus) copy = gh_dm_send_status_copy(status);
  g_assert_cmpstr(copy->rumor_id, ==, status->rumor_id);
  g_assert_cmpstr(copy->self_copy->wrap_json, ==, status->self_copy->wrap_json);
  g_assert_cmpuint(RLEG(copy)->relays->len, ==, 2);
  g_assert_cmpstr(outcome_at(RLEG(copy), 1)->message, ==,
                  "duplicate: already have it");
  g_object_unref(send);

  /* A second message reuses this generation's cached inbox: no new REQ. */
  GhDmSend *again = gh_dm_sender_send(f.sender, hex_bob, "again", NULL);
  spin_until(send_publishing, again);
  g_assert_cmpuint(lookup_opens(&f), ==, 2);
  assert_pub_urls(&f, hex_bob, BOB_A, BOB_B, NULL);
  relay_ok(&f, hex_bob, BOB_A, TRUE, "");
  relay_ok(&f, hex_bob, BOB_B, TRUE, "");
  relay_ok(&f, hex_alice, ALICE_INBOX, TRUE, "");
  g_assert_cmpint(gh_dm_send_get_status(again)->result, ==, GH_DM_SEND_RESULT_SENT);
  g_object_unref(again);
  fixture_down(&f);
}

static GhDmSend *
send_after_lookup(Fixture *f, gboolean own_inbox, const gchar *content, ...)
{
  settle_own(f, own_inbox);
  GhDmSend *send = gh_dm_sender_send(f->sender, hex_bob, content, NULL);
  spin_until(lookup_started, f);
  GhRelayScope *scope = lookup_scope(f);
  va_list args;
  va_start(args, content);
  for (const gchar *event = va_arg(args, const gchar *); event;
       event = va_arg(args, const gchar *)) {
    const gchar *url = va_arg(args, const gchar *);
    gh_relay_scope_event(scope, url, event);
  }
  va_end(args);
  eose_all(scope);
  return send;
}

static void
test_no_recipient_inbox(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  GhDmSend *send = send_after_lookup(&f, TRUE, "hello", NULL);
  spin_until(send_done, send);
  const GhDmSendStatus *status = gh_dm_send_get_status(send);
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX);
  g_assert_cmpint(RLEG(status)->inbox, ==, GH_INBOX_NOT_FOUND);
  g_assert_cmpint(gh_dm_send_status_get_message_status(status), ==,
                  GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX);
  g_assert_nonnull(status->error_message);
  g_assert_null(status->rumor_json);
  g_assert_cmpuint(f.mock.calls, ==, 0); /* nothing encrypted or signed */
  g_assert_cmpuint(f.pubs->len, ==, 0);  /* and no fallback relay contacted */
  g_assert_true(all_lookups_closed(&f));
  g_object_unref(send);
  fixture_down(&f);
}

static void
test_forged_inbox_ignored(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  /* Carol's list, bob's list tampered after signing, bob's kind-10002 and a
   * far-future bob list are all refused. */
  g_autofree gchar *carol = inbox_list(SECRET_CAROL, 900, UNRELATED, NULL);
  g_autofree gchar *genuine = inbox_list(SECRET_BOB, 100, BOB_A, NULL);
  g_autofree gchar *tampered = g_strdup(genuine);
  gchar *where = strstr(tampered, "bob-inbox-a");
  g_assert_nonnull(where);
  memcpy(where, "bob-inbox-z", strlen("bob-inbox-z"));
  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("relay", UNRELATED, NULL));
  g_autofree gchar *wrong_kind = signed_event(SECRET_BOB, 10002, 900, tags, NULL);
  gint64 future = g_get_real_time() / G_USEC_PER_SEC + 24 * 3600;
  g_autofree gchar *pinned = inbox_list(SECRET_BOB, future, UNRELATED, NULL);

  GhDmSend *send = send_after_lookup(&f, TRUE, "hello", carol, DISC, tampered, DISC2,
                                     wrong_kind, DISC2, pinned, DISC, NULL);
  spin_until(send_done, send);
  g_assert_cmpint(gh_dm_send_get_status(send)->result, ==,
                  GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX);
  g_assert_cmpuint(f.pubs->len, ==, 0);
  g_object_unref(send);

  /* Next to the genuine list, the forgeries still do not redirect anything. */
  GhDmSend *second = gh_dm_sender_send(f.sender, hex_bob, "hello", NULL);
  spin_until(lookup_started, &f);
  GhRelayScope *scope = lookup_scope(&f);
  gh_relay_scope_event(scope, DISC, carol);
  gh_relay_scope_event(scope, DISC2, genuine);
  gh_relay_scope_event(scope, DISC2, pinned);
  gh_relay_scope_event(scope, DISC, wrong_kind);
  eose_all(scope);
  spin_until(send_publishing, second);
  assert_pub_urls(&f, hex_bob, BOB_A, NULL);
  assert_pub_urls(&f, hex_alice, ALICE_INBOX, NULL);
  g_assert_cmpuint(f.pubs->len, ==, 2);
  gh_dm_send_cancel(second);
  g_object_unref(second);
  fixture_down(&f);
}

static void
test_newest_wins(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autofree gchar *newer = inbox_list(SECRET_BOB, 200, BOB_B, NULL);
  g_autofree gchar *older = inbox_list(SECRET_BOB, 100, BOB_A, NULL);
  GhDmSend *send = send_after_lookup(&f, TRUE, "hello", newer, DISC2, older, DISC, NULL);
  spin_until(send_publishing, send);
  assert_pub_urls(&f, hex_bob, BOB_B, NULL);
  gh_dm_send_cancel(send);
  g_object_unref(send);

  /* A newer list that names no usable relay supersedes the older one: the
   * recipient has withdrawn their inbox, so nothing is sent. */
  gh_inbox_resolver_forget(GH_INBOX_RESOLVER(f.lookup), hex_bob);
  g_autofree gchar *withdrawn = inbox_list(SECRET_BOB, 300, "https://bob.test.invalid", NULL);
  guint published = f.pubs->len;
  GhDmSend *second = gh_dm_sender_send(f.sender, hex_bob, "hello", NULL);
  spin_until(lookup_started, &f);
  GhRelayScope *scope = lookup_scope(&f);
  gh_relay_scope_event(scope, DISC, newer);
  gh_relay_scope_event(scope, DISC2, withdrawn);
  eose_all(scope);
  spin_until(send_done, second);
  g_assert_cmpint(gh_dm_send_get_status(second)->result, ==,
                  GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX);
  g_assert_cmpint(RLEG(gh_dm_send_get_status(second))->inbox, ==, GH_INBOX_EMPTY);
  g_assert_cmpuint(f.pubs->len, ==, published);
  g_object_unref(second);
  fixture_down(&f);
}

static void
test_partial_rejected_auth(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, BOB_B, NULL);

  /* Partial: one recipient relay refuses. */
  GhDmSend *partial = send_after_lookup(&f, TRUE, "one", list, DISC, NULL);
  spin_until(send_publishing, partial);
  relay_ok(&f, hex_bob, BOB_A, TRUE, "");
  relay_ok(&f, hex_bob, BOB_B, FALSE, "blocked: not on the allow list");
  relay_ok(&f, hex_alice, ALICE_INBOX, TRUE, "");
  const GhDmSendStatus *status = gh_dm_send_get_status(partial);
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_SENT);
  g_assert_cmpint(status->flags, ==, GH_DM_SEND_FLAG_RECIPIENT_PARTIAL);
  g_assert_cmpint(outcome_at(RLEG(status), 1)->outcome, ==,
                  GH_RELAY_PUBLISH_REJECTED);
  g_assert_cmpint(outcome_at(RLEG(status), 1)->prefix, ==,
                  GH_RELAY_OK_PREFIX_BLOCKED);
  g_object_unref(partial);

  /* All recipient relays refuse: FAILED, even though the self-copy landed. */
  GhDmSend *rejected = gh_dm_sender_send(f.sender, hex_bob, "two", NULL);
  spin_until(send_publishing, rejected);
  relay_ok(&f, hex_alice, ALICE_INBOX, TRUE, "");
  relay_ok(&f, hex_bob, BOB_A, FALSE, "invalid: bad wrap");
  relay_failed(&f, hex_bob, BOB_B);
  status = gh_dm_send_get_status(rejected);
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_FAILED);
  g_assert_cmpint(status->failure, ==, GH_DM_SEND_FAILURE_RELAYS);
  g_assert_cmpint(gh_dm_send_status_get_message_status(status), ==, GH_MESSAGE_STATUS_NOT_SENT);
  g_assert_cmpint(status->flags, ==, GH_DM_SEND_FLAG_NONE);
  g_assert_cmpuint(status->self_copy->accepted, ==, 1);
  g_assert_cmpint(outcome_at(RLEG(status), 1)->outcome, ==,
                  GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_object_unref(rejected);

  /* AUTH_REQUIRED is surfaced per relay and never counted as accepted. */
  GhDmSend *auth = gh_dm_sender_send(f.sender, hex_bob, "three", NULL);
  spin_until(send_publishing, auth);
  relay_ok(&f, hex_bob, BOB_A, FALSE, "auth-required: we only accept DMs from members");
  relay_ok(&f, hex_bob, BOB_B, TRUE, "");
  relay_ok(&f, hex_alice, ALICE_INBOX, FALSE, "auth-required: log in first");
  status = gh_dm_send_get_status(auth);
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_SENT);
  g_assert_cmpint(status->flags, ==, GH_DM_SEND_FLAG_RECIPIENT_PARTIAL |
                                     GH_DM_SEND_FLAG_AUTH_REQUIRED |
                                     GH_DM_SEND_FLAG_SELF_COPY_FAILED);
  g_assert_cmpint(outcome_at(RLEG(status), 0)->outcome, ==,
                  GH_RELAY_PUBLISH_AUTH_REQUIRED);
  g_assert_cmpint(outcome_at(status->self_copy, 0)->outcome, ==,
                  GH_RELAY_PUBLISH_AUTH_REQUIRED);
  g_assert_cmpuint(status->self_copy->accepted, ==, 0);
  /* A self-copy failure never changes the message status. */
  g_assert_cmpint(gh_dm_send_status_get_message_status(status), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_false(gh_dm_send_status_self_copy_stored(status));
  g_object_unref(auth);
  fixture_down(&f);
}

static void
test_no_own_inbox(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, NULL);
  GhDmSend *send = send_after_lookup(&f, FALSE, "hello", list, DISC, NULL);
  spin_until(send_publishing, send);
  g_assert_cmpuint(f.pubs->len, ==, 1);
  assert_pub_urls(&f, hex_bob, BOB_A, NULL);
  relay_ok(&f, hex_bob, BOB_A, TRUE, "");
  const GhDmSendStatus *status = gh_dm_send_get_status(send);
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_SENT);
  g_assert_cmpint(status->flags, ==, GH_DM_SEND_FLAG_NO_OWN_INBOX);
  g_assert_cmpint(status->self_copy->inbox, ==, GH_INBOX_NOT_FOUND);
  g_assert_false(gh_dm_send_status_self_copy_stored(status));
  g_assert_cmpuint(status->self_copy->relays->len, ==, 0);
  g_assert_nonnull(status->self_copy->wrap_json); /* kept for a later outbox */
  g_assert_cmpuint(strlen(status->self_copy->wrap_id), ==, 64);
  g_assert_cmpstr(status->self_copy->wrap_id, !=, RLEG(status)->wrap_id);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  g_object_unref(send);
  fixture_down(&f);
}

static void
test_lookup_unreachable(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  settle_own(&f, TRUE);
  GhDmSend *send = gh_dm_sender_send(f.sender, hex_bob, "hello", NULL);
  spin_until(lookup_started, &f);
  GhRelayScope *scope = lookup_scope(&f);
  /* A discovery relay that wants AUTH gets none and fails as a source: the
   * lookup's URLs keep the default NONE identity (and check_privacy.py keeps
   * account AUTH out of the lookup), so nothing is signed. */
  gh_relay_scope_auth_challenge(scope, DISC, "lookup-challenge");
  gh_relay_scope_notice(scope, DISC, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "auth-required: authenticate to read");
  drain();
  g_assert_false(gh_dm_send_is_done(send));
  gh_relay_scope_notice(scope, DISC2, GH_RELAY_NOTICE_ERROR, NULL, FALSE, "refused");
  spin_until(send_done, send);
  const GhDmSendStatus *status = gh_dm_send_get_status(send);
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_INBOX_UNKNOWN);
  g_assert_cmpint(RLEG(status)->inbox, ==, GH_INBOX_UNREACHABLE);
  g_assert_cmpuint(f.mock.calls, ==, 0);
  g_assert_cmpuint(f.pubs->len, ==, 0);
  g_object_unref(send);
  fixture_down(&f);
}

static void
test_signer_denial(void)
{
  for (guint stage = 1; stage <= 4; stage += 3) {
    Fixture f = { 0 };
    fixture_up(&f);
    f.mock.deny_call = stage;
    g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, NULL);
    GhDmSend *send = send_after_lookup(&f, TRUE, "hello", list, DISC, NULL);
    spin_until(send_done, send);
    const GhDmSendStatus *status = gh_dm_send_get_status(send);
    g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_FAILED);
    g_assert_cmpint(status->failure, ==, GH_DM_SEND_FAILURE_SIGNER);
    g_assert_cmpuint(f.mock.calls, ==, stage);
    g_assert_cmpuint(f.pubs->len, ==, 0);
    g_assert_null(RLEG(status)->wrap_json);
    g_object_unref(send);
    fixture_down(&f);
  }
}

static void
test_invalid_input(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  const gchar *bad[][2] = { { "not-hex", "hello" }, { NULL, "hello" }, { NULL, "" } };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    const gchar *recipient = bad[i][0] ? bad[i][0] : hex_bob;
    if (i == 1)
      recipient = "zz";
    GhDmSend *send = gh_dm_sender_send(f.sender, recipient, bad[i][1], NULL);
    g_assert_true(gh_dm_send_is_done(send));
    g_assert_cmpint(gh_dm_send_get_status(send)->failure, ==, GH_DM_SEND_FAILURE_INVALID);
    g_object_unref(send);
  }
  /* No active account. */
  g_settings_set_string(f.settings, "current-npub", "");
  drain(); /* external pair writes reconcile together at idle */
  GhDmSend *send = gh_dm_sender_send(f.sender, hex_bob, "hello", NULL);
  g_assert_true(gh_dm_send_is_done(send));
  g_assert_cmpint(gh_dm_send_get_status(send)->result, ==, GH_DM_SEND_RESULT_FAILED);
  g_assert_cmpint(gh_dm_send_get_status(send)->failure, ==, GH_DM_SEND_FAILURE_INVALID);
  g_object_unref(send);
  drain();
  g_assert_cmpuint(lookup_opens(&f), ==, 0);
  g_assert_cmpuint(f.mock.calls, ==, 0);
  fixture_down(&f);
}

static void
test_self_dm(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  settle_own(&f, TRUE);
  GhDmSend *send = gh_dm_sender_send(f.sender, hex_alice, "note to self", NULL);
  spin_until(send_publishing, send);
  const GhDmSendStatus *status = gh_dm_send_get_status(send);
  /* No recipient lookup (the own list is the inbox), two signer operations,
   * and one wrap to the own inbox only. */
  g_assert_cmpuint(lookup_opens(&f), ==, 0);
  g_assert_cmpuint(f.mock.calls, ==, 2);
  g_assert_cmpuint(f.pubs->len, ==, 1);
  assert_pub_urls(&f, hex_alice, ALICE_INBOX, NULL);
  g_assert_null(status->self_copy); /* one wrap: the recipient leg is self */
  g_assert_cmpuint(status->recipients->len, ==, 1);
  g_assert_cmpstr(RLEG(status)->pubkey, ==, hex_alice);
  assert_wrap_carries(RLEG(status)->wrap_json, SECRET_ALICE, status);
  g_autofree gchar *p_self = g_strdup_printf("[\"p\",\"%s\"]", hex_alice);
  g_assert_nonnull(strstr(status->rumor_json, p_self));
  relay_ok(&f, hex_alice, ALICE_INBOX, TRUE, "");
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_SENT);
  g_assert_cmpint(status->flags, ==, GH_DM_SEND_FLAG_SELF_DM);
  g_object_unref(send);
  fixture_down(&f);

  /* Without an own inbox list a note to self has nowhere to go. */
  Fixture g = { 0 };
  fixture_up(&g);
  settle_own(&g, FALSE);
  GhDmSend *nowhere = gh_dm_sender_send(g.sender, hex_alice, "note to self", NULL);
  spin_until(send_done, nowhere);
  g_assert_cmpint(gh_dm_send_get_status(nowhere)->result, ==,
                  GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX);
  g_assert_cmpuint(g.mock.calls, ==, 0);
  g_assert_cmpuint(g.pubs->len, ==, 0);
  g_object_unref(nowhere);
  fixture_down(&g);
}

/* ---- cancellation and account switch in every phase ---------------------- */

enum { STAGE_OWN_LISTS, STAGE_LOOKUP, STAGE_SIGNER_FIRST, STAGE_SIGNER_LAST, STAGE_PUBLISH };
enum { BY_CANCELLABLE, BY_CANCEL_CALL, BY_ACCOUNT_SWITCH };

static void
run_interrupt(guint stage, guint how)
{
  Fixture f = { 0 };
  fixture_up(&f);
  if (stage != STAGE_OWN_LISTS)
    settle_own(&f, TRUE);
  if (stage == STAGE_SIGNER_FIRST)
    f.mock.hold_call = 1;
  if (stage == STAGE_SIGNER_LAST)
    f.mock.hold_call = 4;
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, BOB_B, NULL);
  GhDmSend *send = gh_dm_sender_send(f.sender, hex_bob, "hello", cancellable);
  const GhDmSendStatus *status = gh_dm_send_get_status(send);
  GhRelayScope *lookup = NULL;

  switch (stage) {
  case STAGE_OWN_LISTS:
    drain();
    g_assert_cmpuint(lookup_opens(&f), ==, 0);
    break;
  case STAGE_LOOKUP:
    spin_until(lookup_started, &f);
    lookup = lookup_scope(&f);
    break;
  case STAGE_SIGNER_FIRST:
  case STAGE_SIGNER_LAST: {
    answer_lookup(&f, list);
    CallCount count = { &f.mock, f.mock.hold_call };
    spin_until(calls_reached, &count);
    g_assert_cmpint(status->phase, ==, GH_DM_SEND_PHASE_SEALING);
    g_assert_cmpint(gh_dm_send_status_get_message_status(status), ==,
                    GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
    break;
  }
  case STAGE_PUBLISH:
    answer_lookup(&f, list);
    spin_until(send_publishing, send);
    relay_ok(&f, hex_bob, BOB_A, TRUE, ""); /* already on a relay before the cancel */
    break;
  default:
    g_assert_not_reached();
  }
  g_assert_false(gh_dm_send_is_done(send));

  switch (how) {
  case BY_CANCELLABLE:
    g_cancellable_cancel(cancellable);
    spin_until(send_done, send);
    break;
  case BY_CANCEL_CALL:
    gh_dm_send_cancel(send);
    g_assert_true(gh_dm_send_is_done(send));
    break;
  case BY_ACCOUNT_SWITCH:
    g_assert_true(gh_account_controller_select(f.accounts, npub_bob, NULL));
    /* Traffic in the window between revocation and cancellation dispatch is
     * stale: it can neither complete the lookup nor count as accepted. */
    if (lookup) {
      gh_relay_scope_event(lookup, DISC, list);
      eose_all(lookup);
    }
    if (stage == STAGE_PUBLISH) {
      relay_ok(&f, hex_bob, BOB_B, TRUE, "");
      g_assert_true(gh_dm_send_is_done(send));
    }
    spin_until(send_done, send);
    break;
  default:
    g_assert_not_reached();
  }

  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_CANCELLED);
  g_assert_cmpint(status->phase, ==, GH_DM_SEND_PHASE_DONE);
  g_assert_cmpint(gh_dm_send_status_get_message_status(status), ==,
                  GH_MESSAGE_STATUS_CANCELLED);
  drain();
  g_assert_true(all_lookups_closed(&f));
  g_assert_cmpuint(open_pubs(&f), ==, 0);
  if (stage == STAGE_PUBLISH) {
    /* The earlier OK is a fact and is kept; the rest are cancelled, and a
     * late OK changes nothing. */
    g_assert_cmpuint(RLEG(status)->accepted, ==, 1);
    g_assert_cmpint(outcome_at(RLEG(status), 0)->outcome, ==,
                    GH_RELAY_PUBLISH_ACCEPTED);
    g_assert_cmpint(outcome_at(RLEG(status), 1)->outcome, ==,
                    GH_RELAY_PUBLISH_CANCELLED);
    g_assert_cmpint(outcome_at(status->self_copy, 0)->outcome, ==,
                    GH_RELAY_PUBLISH_CANCELLED);
    relay_ok(&f, hex_alice, ALICE_INBOX, TRUE, "");
    g_assert_cmpint(outcome_at(status->self_copy, 0)->outcome, ==,
                    GH_RELAY_PUBLISH_CANCELLED);
    g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_CANCELLED);
  } else {
    g_assert_cmpuint(f.pubs->len, ==, 0);
  }
  if (stage == STAGE_OWN_LISTS && how != BY_ACCOUNT_SWITCH) {
    /* Settling afterwards starts nothing. */
    settle_own(&f, TRUE);
    drain();
    g_assert_cmpuint(lookup_opens(&f), ==, 0);
  }
  if (stage == STAGE_LOOKUP && how != BY_ACCOUNT_SWITCH) {
    gh_relay_scope_event(lookup, DISC, list); /* closed scope: discarded */
    drain();
  }
  if (stage == STAGE_SIGNER_FIRST || stage == STAGE_SIGNER_LAST) {
    /* The pending approval was revoked, and nothing further was asked. */
    SenderCheck check = { &f.bus, &f.mock };
    spin_until(signer_senders_closed, &check);
    g_assert_cmpuint(f.mock.calls, ==, f.mock.hold_call);
  }
  if (stage <= STAGE_LOOKUP)
    g_assert_cmpuint(f.mock.calls, ==, 0);
  g_object_unref(send);
  fixture_down(&f);
}

static void
test_interrupts(void)
{
  for (guint stage = STAGE_OWN_LISTS; stage <= STAGE_PUBLISH; stage++)
    for (guint how = BY_CANCELLABLE; how <= BY_ACCOUNT_SWITCH; how++)
      run_interrupt(stage, how);
}


/* The outbox seam: seal, persist the plain status, publish it later. The
 * publish step republishes the stored bytes, asks the signer nothing, and on
 * a second run targets only relays that have not accepted. */
static void
test_seal_then_publish(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  settle_own(&f, TRUE);
  GhDmSend *seal = gh_dm_sender_seal(f.sender, hex_bob, "sealed first", NULL);
  g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, BOB_B, NULL);
  answer_lookup(&f, list);
  spin_until(send_done, seal);
  const GhDmSendStatus *sealed = gh_dm_send_get_status(seal);
  g_assert_cmpint(sealed->result, ==, GH_DM_SEND_RESULT_SEALED);
  g_assert_cmpint(gh_dm_send_status_get_message_status(sealed), ==, GH_MESSAGE_STATUS_SENDING);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  g_assert_cmpuint(f.pubs->len, ==, 0); /* sealing publishes nothing */
  g_assert_cmpuint(strlen(RLEG(sealed)->wrap_id), ==, 64);
  g_assert_cmpuint(strlen(sealed->self_copy->wrap_id), ==, 64);
  g_assert_cmpuint(RLEG(sealed)->relays->len, ==, 2);
  g_assert_cmpuint(sealed->self_copy->relays->len, ==, 1);
  g_autoptr(GhDmSendStatus) stored = gh_dm_send_status_copy(sealed);
  g_object_unref(seal);

  GhDmSend *first = gh_dm_sender_publish(f.sender, stored, NULL);
  g_assert_false(gh_dm_send_is_done(first));
  drain(); /* the publish starts from the main loop */
  g_assert_cmpuint(f.mock.calls, ==, 4); /* never re-sealed */
  assert_pub_urls(&f, hex_bob, BOB_A, BOB_B, NULL);
  assert_pub_urls(&f, hex_alice, ALICE_INBOX, NULL);
  g_assert_cmpstr(pub_find(&f, hex_bob, BOB_A)->event_json, ==, RLEG(stored)->wrap_json);
  g_assert_cmpstr(pub_find(&f, hex_alice, ALICE_INBOX)->event_json, ==,
                  stored->self_copy->wrap_json);
  relay_ok(&f, hex_bob, BOB_A, TRUE, "");
  relay_failed(&f, hex_bob, BOB_B);
  relay_ok(&f, hex_alice, ALICE_INBOX, TRUE, "");
  const GhDmSendStatus *after = gh_dm_send_get_status(first);
  g_assert_cmpint(after->result, ==, GH_DM_SEND_RESULT_SENT);
  g_assert_cmpint(after->flags, ==, GH_DM_SEND_FLAG_RECIPIENT_PARTIAL);
  g_autoptr(GhDmSendStatus) resumed = gh_dm_send_status_copy(after);
  g_object_unref(first);

  /* Resume: only the relay that did not accept gets the same wrap again. */
  guint before = f.pubs->len;
  GhDmSend *second = gh_dm_sender_publish(f.sender, resumed, NULL);
  drain(); /* the publish starts from the main loop */
  g_assert_cmpuint(f.pubs->len, ==, before + 1);
  PubOpen *retry = g_ptr_array_index(f.pubs, before);
  g_assert_cmpstr(retry->url, ==, BOB_B);
  g_assert_cmpstr(retry->event_json, ==, RLEG(stored)->wrap_json);
  relay_ok(&f, hex_bob, BOB_B, TRUE, "");
  const GhDmSendStatus *done = gh_dm_send_get_status(second);
  g_assert_cmpint(done->result, ==, GH_DM_SEND_RESULT_SENT);
  g_assert_cmpint(done->flags, ==, GH_DM_SEND_FLAG_NONE);
  g_assert_cmpuint(RLEG(done)->accepted, ==, 2);
  g_assert_cmpuint(done->self_copy->accepted, ==, 1);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  g_object_unref(second);
  fixture_down(&f);
}

/* A stored status is only republished for the account that sealed it and
 * only with an intact wrap addressed to that leg's receiver. */
static void
test_publish_rejects_foreign(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  settle_own(&f, TRUE);
  GhDmSend *seal = gh_dm_sender_seal(f.sender, hex_bob, "sealed", NULL);
  g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, NULL);
  answer_lookup(&f, list);
  spin_until(send_done, seal);
  g_autoptr(GhDmSendStatus) stored = gh_dm_send_status_copy(gh_dm_send_get_status(seal));
  g_object_unref(seal);

  /* Tampered bytes. */
  g_autoptr(GhDmSendStatus) tampered = gh_dm_send_status_copy(stored);
  gchar *content = strstr(RLEG(tampered)->wrap_json, "\"content\":\"");
  g_assert_nonnull(content);
  content[11] = content[11] == 'A' ? 'B' : 'A';
  GhDmSend *bad = gh_dm_sender_publish(f.sender, tampered, NULL);
  g_assert_true(gh_dm_send_is_done(bad));
  g_assert_cmpint(gh_dm_send_get_status(bad)->failure, ==, GH_DM_SEND_FAILURE_INVALID);
  g_object_unref(bad);

  /* The recipient's wrap moved onto the self-copy leg (wrong receiver). */
  g_autoptr(GhDmSendStatus) swapped = gh_dm_send_status_copy(stored);
  g_free(swapped->self_copy->wrap_json);
  swapped->self_copy->wrap_json = g_strdup(RLEG(stored)->wrap_json);
  g_free(swapped->self_copy->wrap_id);
  swapped->self_copy->wrap_id = g_strdup(RLEG(stored)->wrap_id);
  GhDmSend *wrong = gh_dm_sender_publish(f.sender, swapped, NULL);
  g_assert_true(gh_dm_send_is_done(wrong));
  g_assert_cmpint(gh_dm_send_get_status(wrong)->failure, ==, GH_DM_SEND_FAILURE_INVALID);
  g_object_unref(wrong);

  /* Another account is active. */
  g_assert_true(gh_account_controller_select(f.accounts, npub_bob, NULL));
  GhDmSend *foreign = gh_dm_sender_publish(f.sender, stored, NULL);
  g_assert_true(gh_dm_send_is_done(foreign));
  g_assert_cmpint(gh_dm_send_get_status(foreign)->failure, ==, GH_DM_SEND_FAILURE_INVALID);
  g_object_unref(foreign);
  drain();
  g_assert_cmpuint(f.pubs->len, ==, 0);
  fixture_down(&f);
}

/* Cancelling or switching during a republish keeps accepted facts and never
 * reports success. */
static void
test_publish_interrupted(void)
{
  for (guint how = BY_CANCELLABLE; how <= BY_ACCOUNT_SWITCH; how++) {
    Fixture f = { 0 };
    fixture_up(&f);
    settle_own(&f, TRUE);
    GhDmSend *seal = gh_dm_sender_seal(f.sender, hex_bob, "sealed", NULL);
    g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, BOB_B, NULL);
    answer_lookup(&f, list);
    spin_until(send_done, seal);
    g_autoptr(GhDmSendStatus) stored = gh_dm_send_status_copy(gh_dm_send_get_status(seal));
    g_object_unref(seal);
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    GhDmSend *send = gh_dm_sender_publish(f.sender, stored, cancellable);
    drain(); /* the publish starts from the main loop */
    relay_ok(&f, hex_bob, BOB_A, TRUE, "");
    if (how == BY_CANCELLABLE) {
      g_cancellable_cancel(cancellable);
      spin_until(send_done, send);
    } else if (how == BY_CANCEL_CALL) {
      gh_dm_send_cancel(send);
    } else {
      g_assert_true(gh_account_controller_select(f.accounts, npub_bob, NULL));
      relay_ok(&f, hex_bob, BOB_B, TRUE, ""); /* stale: revoked generation */
    }
    g_assert_true(gh_dm_send_is_done(send));
    const GhDmSendStatus *status = gh_dm_send_get_status(send);
    g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_CANCELLED);
    g_assert_cmpuint(RLEG(status)->accepted, ==, 1);
    g_assert_cmpint(outcome_at(RLEG(status), 1)->outcome, ==, GH_RELAY_PUBLISH_CANCELLED);
    g_assert_cmpuint(open_pubs(&f), ==, 0);
    g_assert_cmpuint(f.mock.calls, ==, 4);
    g_object_unref(send);
    fixture_down(&f);
  }
}

/* Disposing the sender (app shutdown) cancels every unfinished send. */
static void
test_sender_dispose(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, NULL);
  GhDmSend *send = send_after_lookup(&f, TRUE, "hello", list, DISC, NULL);
  spin_until(send_publishing, send);
  g_object_run_dispose(G_OBJECT(f.sender));
  g_assert_true(gh_dm_send_is_done(send));
  g_assert_cmpint(gh_dm_send_get_status(send)->result, ==, GH_DM_SEND_RESULT_CANCELLED);
  g_assert_cmpuint(open_pubs(&f), ==, 0);
  g_object_unref(send);
  fixture_down(&f);
}

#ifdef GROUNDHOG_TEST_WIRE
/* ---- real local relays --------------------------------------------------- */

typedef struct {
  GPtrArray *ids;
} InboxLog;

/* Stores and acknowledges every wrap: NIP-01 OK true. */
static void
inbox_accept(WireRelay *relay, SoupWebsocketConnection *connection,
             const gchar *event_id, gpointer data)
{
  (void)relay;
  InboxLog *log = data;
  g_ptr_array_add(log->ids, g_strdup(event_id));
  g_autofree gchar *ok = g_strdup_printf("[\"OK\",\"%s\",true,\"\"]", event_id);
  soup_websocket_connection_send_text(connection, ok);
}

/* Both wraps travel over real WebSockets (GNostrRelay publish transport) to
 * two local relays, each of which must receive exactly its own receiver's
 * wrap. The 10050 REQs use the recording scope transport so the lookup's
 * answer is scripted; the real REQ path is test_wire_lookup below. */
static void
test_wire_publish(void)
{
  WireRelay bob_relay = { 0 }, alice_relay = { 0 };
  InboxLog bob_log = { g_ptr_array_new_with_free_func(g_free) };
  InboxLog alice_log = { g_ptr_array_new_with_free_func(g_free) };
  relay_init(&bob_relay);
  relay_init(&alice_relay);
  bob_relay.on_event = inbox_accept;
  bob_relay.on_event_data = &bob_log;
  alice_relay.on_event = inbox_accept;
  alice_relay.on_event_data = &alice_log;

  Fixture f = { 0 };
  bus_up(&f.bus, &f.mock);
  f.settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { DISC, DISC2, NULL };
  g_settings_set_strv(f.settings, "discovery-relays", sources);
  g_settings_set_string(f.settings, "signer-method", "auto");
  g_settings_set_string(f.settings, "current-npub", npub_alice);
  f.accounts = gh_account_controller_new_full(f.settings, f.bus.client, fake_list, NULL);
  spin_until(listed, f.accounts);
  f.scopes = g_ptr_array_new_with_free_func(scope_open_free);
  f.pubs = g_ptr_array_new_with_free_func(pub_open_free);
  f.relays = gh_account_relays_new(f.accounts, f.settings, &scope_transport, f.scopes);
  f.lookup = gh_inbox_lookup_new(f.accounts, f.settings, &scope_transport, f.scopes);
  f.sender = gh_dm_sender_new(f.accounts, f.relays, GH_INBOX_RESOLVER(f.lookup), NULL, NULL);

  ScopeOpen *own = own_open(&f, hex_alice);
  g_autofree gchar *nip65 = nip65_list(SECRET_ALICE, 100, OWN_READ, OWN_WRITE);
  g_autofree gchar *own_inbox = inbox_list(SECRET_ALICE, 100, alice_relay.url, NULL);
  gh_relay_scope_event(own->scope, DISC, nip65);
  gh_relay_scope_event(own->scope, DISC, own_inbox);
  gh_relay_scope_eose(own->scope, DISC);
  gh_relay_scope_eose(own->scope, DISC2);
  GhDmSend *send = gh_dm_sender_send(f.sender, hex_bob, "hello over the wire", NULL);
  g_autofree gchar *bob_list = inbox_list(SECRET_BOB, 100, bob_relay.url, NULL);
  answer_lookup(&f, bob_list);
  spin_until(send_done, send);

  const GhDmSendStatus *status = gh_dm_send_get_status(send);
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_SENT);
  g_assert_cmpint(status->flags, ==, GH_DM_SEND_FLAG_NONE);
  g_assert_cmpuint(bob_log.ids->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(bob_log.ids, 0), ==, RLEG(status)->wrap_id);
  g_assert_cmpuint(alice_log.ids->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(alice_log.ids, 0), ==, status->self_copy->wrap_id);
  g_assert_cmpuint(bob_relay.reqs + alice_relay.reqs, ==, 0);
  g_assert_cmpuint(bob_relay.connections->len, ==, 1);
  g_assert_cmpuint(alice_relay.connections->len, ==, 1);
  g_object_unref(send);

  fixture_down(&f);
  relay_clear(&bob_relay);
  relay_clear(&alice_relay);
  g_ptr_array_unref(bob_log.ids);
  g_ptr_array_unref(alice_log.ids);
}

typedef struct {
  GhInboxResult *result;
  GError *error;
  gboolean done;
} LookupWait;

static void
on_lookup_done(GObject *source, GAsyncResult *res, gpointer data)
{
  LookupWait *wait = data;
  wait->result = gh_inbox_resolver_resolve_finish(GH_INBOX_RESOLVER(source), res,
                                                  &wait->error);
  wait->done = TRUE;
}

static gboolean
lookup_waited(gpointer data)
{
  return ((LookupWait *)data)->done;
}

#define WIRE_LOOKUP_ROUNDS 100

/* nostrc-qp24.10.6: GhInboxLookup over its real transport (GhRelayScope ->
 * GNostrSubscription -> libnostr -> a WebSocket) against a store-and-serve
 * relay that answers the REQ with the recipient's 10050 list and EOSE in one
 * burst. The lookup completes at EOSE, so every round must find the list:
 * before the fix nostr-gobject could emit EOSE ahead of the stored event and
 * real-socket lookups reported NOT_FOUND ("hasn't set up private
 * messaging"). The list is the whole burst, as for a typical recipient: the
 * race lost the event just before EOSE. Many rounds, each a fresh REQ on a
 * fresh socket, since the old failure was a race. */
static void
test_wire_lookup(void)
{
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  gchar *list_id = NULL;
  g_autofree gchar *list = inbox_list_id(SECRET_BOB, 1000, &list_id, BOB_A, BOB_B, NULL);
  wire_relay_inject(&relay, list);
  /* Someone else's list is kept but not served: the REQ names bob. */
  g_autofree gchar *carol = inbox_list(SECRET_CAROL, 2000, UNRELATED, NULL);
  wire_relay_inject(&relay, carol);

  BusFixture bus = { 0 };
  MockSigner mock = { 0 };
  bus_up(&bus, &mock);
  GSettings *settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { relay.url, NULL };
  g_settings_set_strv(settings, "discovery-relays", sources);
  g_settings_set_string(settings, "signer-method", "auto");
  g_settings_set_string(settings, "current-npub", npub_alice);
  GhAccountController *accounts =
    gh_account_controller_new_full(settings, bus.client, fake_list, NULL);
  spin_until(listed, accounts);
  g_assert_cmpint(gh_account_controller_get_state(accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  GhInboxLookup *lookup = gh_inbox_lookup_new(accounts, settings, NULL, NULL);

  guint missed = 0;
  for (guint round = 0; round < WIRE_LOOKUP_ROUNDS; round++) {
    gh_inbox_resolver_forget(GH_INBOX_RESOLVER(lookup), hex_bob); /* a real REQ each time */
    LookupWait wait = { 0 };
    gh_inbox_resolver_resolve_async(GH_INBOX_RESOLVER(lookup), hex_bob, NULL,
                                    on_lookup_done, &wait);
    spin_until(lookup_waited, &wait);
    g_assert_no_error(wait.error);
    g_assert_nonnull(wait.result);
    g_assert_false(wait.result->cached);
    g_assert_cmpuint(wait.result->sources, ==, 1);
    g_assert_cmpuint(wait.result->answered, ==, 1);
    if (wait.result->status != GH_INBOX_FOUND ||
        g_strcmp0(wait.result->event_id, list_id) != 0)
      missed++;
    else {
      g_assert_cmpuint(g_strv_length(wait.result->relays), ==, 2);
      g_assert_cmpstr(wait.result->relays[0], ==, BOB_A);
      g_assert_cmpstr(wait.result->relays[1], ==, BOB_B);
    }
    gh_inbox_result_free(wait.result);
  }
  g_test_message("%u/%u real-REQ lookups missed the stored 10050", missed,
                 WIRE_LOOKUP_ROUNDS);
  g_assert_cmpuint(missed, ==, 0);
  g_assert_cmpuint(relay.reqs, ==, WIRE_LOOKUP_ROUNDS);
  g_assert_cmpuint(relay.served, ==, WIRE_LOOKUP_ROUNDS);

  release(lookup);
  release(accounts);
  SenderCheck check = { &bus, &mock };
  spin_until(signer_senders_closed, &check);
  g_object_unref(settings);
  bus_down(&bus, &mock);
  relay_clear(&relay);
  g_free(list_id);
}
#endif

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  npub_alice = npub_for_secret(SECRET_ALICE);
  npub_bob = npub_for_secret(SECRET_BOB);
  hex_alice = gh_identity_pubkey_hex(npub_alice);
  hex_bob = gh_identity_pubkey_hex(npub_bob);
  hex_carol = nostr_key_get_public(SECRET_CAROL);
  shared_bus_up();
  nostrc_test_bus_add_func("/groundhog/dm-send/happy-path", test_happy_path);
  nostrc_test_bus_add_func("/groundhog/dm-send/no-recipient-inbox", test_no_recipient_inbox);
  nostrc_test_bus_add_func("/groundhog/dm-send/forged-inbox-ignored", test_forged_inbox_ignored);
  nostrc_test_bus_add_func("/groundhog/dm-send/newest-wins", test_newest_wins);
  nostrc_test_bus_add_func("/groundhog/dm-send/partial-rejected-auth",
                           test_partial_rejected_auth);
  nostrc_test_bus_add_func("/groundhog/dm-send/no-own-inbox", test_no_own_inbox);
  nostrc_test_bus_add_func("/groundhog/dm-send/lookup-unreachable", test_lookup_unreachable);
  nostrc_test_bus_add_func("/groundhog/dm-send/signer-denial", test_signer_denial);
  nostrc_test_bus_add_func("/groundhog/dm-send/invalid-input", test_invalid_input);
  nostrc_test_bus_add_func("/groundhog/dm-send/self-dm", test_self_dm);
  nostrc_test_bus_add_func("/groundhog/dm-send/interrupts", test_interrupts);
  nostrc_test_bus_add_func("/groundhog/dm-send/sender-dispose", test_sender_dispose);
  nostrc_test_bus_add_func("/groundhog/dm-send/seal-then-publish", test_seal_then_publish);
  nostrc_test_bus_add_func("/groundhog/dm-send/publish-rejects-foreign",
                           test_publish_rejects_foreign);
  nostrc_test_bus_add_func("/groundhog/dm-send/publish-interrupted", test_publish_interrupted);
#ifdef GROUNDHOG_TEST_WIRE
  nostrc_test_bus_add_func("/groundhog/dm-send/wire-publish", test_wire_publish);
  nostrc_test_bus_add_func("/groundhog/dm-send/wire-lookup", test_wire_lookup);
#endif
  int status = g_test_run();
  shared_bus_down();
  g_free(npub_alice);
  g_free(npub_bob);
  g_free(hex_alice);
  g_free(hex_bob);
  free(hex_carol);
  return status;
}
