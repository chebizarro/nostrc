/* Production NIP-17 sealing: kind-10050 lookup, envelope/target selection,
 * and generation/cancel revocation. A recording GhRelayScope drives lookup
 * (with a separate local WebSocket lookup case); a private test bus hosts
 * the signer. Deadlines are failure bounds only. */
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
notify_finalized(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(15, deadline_hit, &expired);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
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

static void
on_finalize(gpointer data, GObject *object)
{
  (void)object;
  g_idle_add(notify_finalized, data);
}

static gboolean
finalized(gpointer data)
{
  return *(gboolean *)data;
}

static void
release(gpointer object)
{
  gboolean done = FALSE;
  g_object_weak_ref(G_OBJECT(object), on_finalize, &done);
  g_object_run_dispose(G_OBJECT(object));
  g_object_unref(object);
  spin_until(finalized, &done);
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

/* The bus signal wakes the context while we wait for private signer teardown. */
static void
owner_changed(GDBusConnection *connection, const gchar *sender, const gchar *path,
              const gchar *interface, const gchar *signal, GVariant *parameters, gpointer data)
{
  (void)connection; (void)sender; (void)path; (void)interface;
  (void)signal; (void)parameters; (void)data;
}

static void
wait_signer_senders_closed(SenderCheck *check)
{
  guint id = g_dbus_connection_signal_subscribe(check->bus->client, "org.freedesktop.DBus",
    "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", NULL,
    G_DBUS_SIGNAL_FLAGS_NONE, owner_changed, NULL, NULL);
  spin_until(signer_senders_closed, check);
  g_dbus_connection_signal_unsubscribe(check->bus->client, id);
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

/* ---- fixture ------------------------------------------------------------- */

typedef struct {
  BusFixture bus;
  MockSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GPtrArray *scopes; /* ScopeOpen */
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
  f->relays = gh_account_relays_new(f->accounts, f->settings, &scope_transport, f->scopes);
  f->lookup = gh_inbox_lookup_new(f->accounts, f->settings, &scope_transport, f->scopes);
  f->sender = gh_dm_sender_new(f->accounts, f->relays, GH_INBOX_RESOLVER(f->lookup));
}

static void
fixture_down(Fixture *f)
{
  release(f->sender);
  release(f->lookup);
  release(f->relays);
  release(f->accounts);
  SenderCheck check = { &f->bus, &f->mock };
  wait_signer_senders_closed(&check);
  /* Nothing is left open on any relay. */
  for (guint i = 0; i < f->scopes->len; i++)
    g_assert_true(((ScopeOpen *)g_ptr_array_index(f->scopes, i))->closed);
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

static gboolean
send_done(gpointer data)
{
  return gh_dm_send_is_done(data);
}

#define RLEG(status) ((GhDmSendLeg *)g_ptr_array_index((status)->recipients, 0))

static void
assert_targets(const GhDmSendLeg *leg, ...)
{
  va_list args;
  va_start(args, leg);
  for (guint i = 0; i < leg->relays->len; i++) {
    const GhDmRelayOutcome *target = g_ptr_array_index(leg->relays, i);
    const gchar *want = va_arg(args, const gchar *);
    g_assert_nonnull(want);
    g_assert_cmpstr(target->url, ==, want);
  }
  g_assert_null(va_arg(args, const gchar *));
  va_end(args);
}

static GhDmSend *
seal_text(GhDmSender *sender, const gchar *recipient, const gchar *content,
          GCancellable *cancellable)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *rumor = gh_nip17_rumor_new(hex_alice, recipient, content, 100, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(rumor);
  return gh_dm_sender_seal_room_rumor(sender, rumor, NULL, cancellable);
}

static void
record_phase(GhDmSend *send, gpointer data)
{
  GArray *phases = data;
  GhDmSendPhase phase = gh_dm_send_get_status(send)->phase;
  if (!phases->len || g_array_index(phases, GhDmSendPhase, phases->len - 1) != phase)
    g_array_append_val(phases, phase);
}

/* Unwraps a sealed wrap with the receiver's key and checks the rumor. */
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
  GhDmSend *send = seal_text(f.sender, hex_bob, "hello bob", NULL);
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
  spin_until(send_done, send);
  g_assert_true(all_lookups_closed(&f));
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_SEALED);
  g_assert_cmpuint(status->recipients->len, ==, 1);
  g_assert_cmpstr(RLEG(status)->pubkey, ==, hex_bob);
  g_assert_cmpstr(status->self_copy->pubkey, ==, hex_alice);
  assert_targets(RLEG(status), BOB_A, BOB_B, NULL);
  assert_targets(status->self_copy, ALICE_INBOX, NULL);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  g_assert_cmpint(RLEG(status)->inbox, ==, GH_INBOX_FOUND);
  g_assert_cmpstr(RLEG(status)->inbox_event_id, ==, list_id);

  assert_wrap_carries(RLEG(status)->wrap_json, SECRET_BOB, status);
  assert_wrap_carries(status->self_copy->wrap_json, SECRET_ALICE, status);
  g_assert_nonnull(strstr(status->rumor_json, "hello bob"));
  g_assert_cmpuint(strlen(RLEG(status)->wrap_id), ==, 64);
  g_assert_cmpuint(strlen(status->self_copy->wrap_id), ==, 64);
  g_assert_cmpuint(strlen(status->rumor_id), ==, 64);
  g_assert_nonnull(strstr(status->rumor_json, status->rumor_id));
  GhDmSendPhase want[] = { GH_DM_SEND_PHASE_RESOLVING, GH_DM_SEND_PHASE_SEALING,
                           GH_DM_SEND_PHASE_DONE };
  g_assert_cmpmem(phases->data, phases->len * sizeof(GhDmSendPhase), want, sizeof want);

  g_object_unref(send);

  /* A second message reuses this generation's cached inbox: no new REQ. */
  GhDmSend *again = seal_text(f.sender, hex_bob, "again", NULL);
  spin_until(send_done, again);
  g_assert_cmpuint(lookup_opens(&f), ==, 2);
  assert_targets(RLEG(gh_dm_send_get_status(again)), BOB_A, BOB_B, NULL);
  g_assert_cmpint(gh_dm_send_get_status(again)->result, ==, GH_DM_SEND_RESULT_SEALED);
  g_object_unref(again);
  fixture_down(&f);
}

static GhDmSend *
send_after_lookup(Fixture *f, gboolean own_inbox, const gchar *content, ...)
{
  settle_own(f, own_inbox);
  GhDmSend *send = seal_text(f->sender, hex_bob, content, NULL);
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
  g_assert_nonnull(status->error_message);
  g_assert_null(status->rumor_json);
  g_assert_cmpuint(f.mock.calls, ==, 0); /* nothing encrypted or signed */
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
  g_object_unref(send);

  /* Next to the genuine list, the forgeries still do not redirect anything. */
  GhDmSend *second = seal_text(f.sender, hex_bob, "hello", NULL);
  spin_until(lookup_started, &f);
  GhRelayScope *scope = lookup_scope(&f);
  gh_relay_scope_event(scope, DISC, carol);
  gh_relay_scope_event(scope, DISC2, genuine);
  gh_relay_scope_event(scope, DISC2, pinned);
  gh_relay_scope_event(scope, DISC, wrong_kind);
  eose_all(scope);
  spin_until(send_done, second);
  assert_targets(RLEG(gh_dm_send_get_status(second)), BOB_A, NULL);
  assert_targets(gh_dm_send_get_status(second)->self_copy, ALICE_INBOX, NULL);
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
  spin_until(send_done, send);
  assert_targets(RLEG(gh_dm_send_get_status(send)), BOB_B, NULL);
  g_object_unref(send);

  /* A newer list that names no usable relay supersedes the older one: the
   * recipient has withdrawn their inbox, so nothing is sent. */
  gh_inbox_resolver_forget(GH_INBOX_RESOLVER(f.lookup), hex_bob);
  g_autofree gchar *withdrawn = inbox_list(SECRET_BOB, 300, "https://bob.test.invalid", NULL);
  GhDmSend *second = seal_text(f.sender, hex_bob, "hello", NULL);
  spin_until(lookup_started, &f);
  GhRelayScope *scope = lookup_scope(&f);
  gh_relay_scope_event(scope, DISC, newer);
  gh_relay_scope_event(scope, DISC2, withdrawn);
  eose_all(scope);
  spin_until(send_done, second);
  g_assert_cmpint(gh_dm_send_get_status(second)->result, ==,
                  GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX);
  g_assert_cmpint(RLEG(gh_dm_send_get_status(second))->inbox, ==, GH_INBOX_EMPTY);
  g_object_unref(second);
  fixture_down(&f);
}

static void
test_no_own_inbox(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, NULL);
  GhDmSend *send = send_after_lookup(&f, FALSE, "hello", list, DISC, NULL);
  spin_until(send_done, send);
  const GhDmSendStatus *status = gh_dm_send_get_status(send);
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_SEALED);
  g_assert_cmpint(status->flags, ==, GH_DM_SEND_FLAG_NO_OWN_INBOX);
  g_assert_cmpint(status->self_copy->inbox, ==, GH_INBOX_NOT_FOUND);
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
  GhDmSend *send = seal_text(f.sender, hex_bob, "hello", NULL);
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
  g_autofree gchar *valid = gh_nip17_rumor_new(hex_alice, hex_bob, "hello", 100, NULL, NULL);
  g_autofree gchar *foreign = gh_nip17_rumor_new(hex_bob, hex_alice, "hello", 100, NULL, NULL);
  g_assert_nonnull(valid);
  g_assert_nonnull(foreign);
  gchar *bad[] = { NULL, "not an event", foreign };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GhDmSend) send = gh_dm_sender_seal_room_rumor(f.sender, bad[i], NULL, NULL);
    g_assert_true(gh_dm_send_is_done(send));
    g_assert_cmpint(gh_dm_send_get_status(send)->failure, ==, GH_DM_SEND_FAILURE_INVALID);
  }
  /* No active account. */
  g_settings_set_string(f.settings, "current-npub", "");
  drain(); /* external pair writes reconcile together at idle */
  GhDmSend *send = gh_dm_sender_seal_room_rumor(f.sender, valid, NULL, NULL);
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
  GhDmSend *send = seal_text(f.sender, hex_alice, "note to self", NULL);
  spin_until(send_done, send);
  const GhDmSendStatus *status = gh_dm_send_get_status(send);
  /* No recipient lookup (the own list is the inbox), two signer operations,
   * and one wrap to the own inbox only. */
  g_assert_cmpuint(lookup_opens(&f), ==, 0);
  g_assert_cmpuint(f.mock.calls, ==, 2);
  assert_targets(RLEG(status), ALICE_INBOX, NULL);
  g_assert_null(status->self_copy); /* one wrap: the recipient leg is self */
  g_assert_cmpuint(status->recipients->len, ==, 1);
  g_assert_cmpstr(RLEG(status)->pubkey, ==, hex_alice);
  assert_wrap_carries(RLEG(status)->wrap_json, SECRET_ALICE, status);
  g_autofree gchar *p_self = g_strdup_printf("[\"p\",\"%s\"]", hex_alice);
  g_assert_nonnull(strstr(status->rumor_json, p_self));
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_SEALED);
  g_assert_cmpint(status->flags, ==, GH_DM_SEND_FLAG_SELF_DM);
  g_object_unref(send);
  fixture_down(&f);

  /* Without an own inbox list a note to self has nowhere to go. */
  Fixture g = { 0 };
  fixture_up(&g);
  settle_own(&g, FALSE);
  GhDmSend *nowhere = seal_text(g.sender, hex_alice, "note to self", NULL);
  spin_until(send_done, nowhere);
  g_assert_cmpint(gh_dm_send_get_status(nowhere)->result, ==,
                  GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX);
  g_assert_cmpuint(g.mock.calls, ==, 0);
  g_object_unref(nowhere);
  fixture_down(&g);
}

/* ---- cancellation and account switch in every phase ---------------------- */

enum { STAGE_OWN_LISTS, STAGE_LOOKUP, STAGE_SIGNER_FIRST, STAGE_SIGNER_LAST };
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
  GhDmSend *send = seal_text(f.sender, hex_bob, "hello", cancellable);
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
    break;
  }
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
    spin_until(send_done, send);
    break;
  default:
    g_assert_not_reached();
  }

  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_CANCELLED);
  g_assert_cmpint(status->phase, ==, GH_DM_SEND_PHASE_DONE);
  drain();
  g_assert_true(all_lookups_closed(&f));
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
    wait_signer_senders_closed(&check);
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
  for (guint stage = STAGE_OWN_LISTS; stage <= STAGE_SIGNER_LAST; stage++)
    for (guint how = BY_CANCELLABLE; how <= BY_ACCOUNT_SWITCH; how++)
      run_interrupt(stage, how);
}


/* Disposing the sender (app shutdown) cancels every unfinished send. */
static void
test_sender_dispose(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autofree gchar *list = inbox_list(SECRET_BOB, 100, BOB_A, NULL);
  f.mock.hold_call = 1;
  GhDmSend *send = send_after_lookup(&f, TRUE, "hello", list, DISC, NULL);
  CallCount count = { &f.mock, 1 };
  spin_until(calls_reached, &count);
  g_assert_false(gh_dm_send_is_done(send));
  g_object_run_dispose(G_OBJECT(f.sender));
  g_assert_true(gh_dm_send_is_done(send));
  g_assert_cmpint(gh_dm_send_get_status(send)->result, ==, GH_DM_SEND_RESULT_CANCELLED);
  g_object_unref(send);
  fixture_down(&f);
}

#ifdef GROUNDHOG_TEST_WIRE
/* ---- real local relays --------------------------------------------------- */

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
  wait_signer_senders_closed(&check);
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
  nostrc_test_bus_add_func("/groundhog/dm-send/no-own-inbox", test_no_own_inbox);
  nostrc_test_bus_add_func("/groundhog/dm-send/lookup-unreachable", test_lookup_unreachable);
  nostrc_test_bus_add_func("/groundhog/dm-send/signer-denial", test_signer_denial);
  nostrc_test_bus_add_func("/groundhog/dm-send/invalid-input", test_invalid_input);
  nostrc_test_bus_add_func("/groundhog/dm-send/self-dm", test_self_dm);
  nostrc_test_bus_add_func("/groundhog/dm-send/interrupts", test_interrupts);
  nostrc_test_bus_add_func("/groundhog/dm-send/sender-dispose", test_sender_dispose);
#ifdef GROUNDHOG_TEST_WIRE
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
