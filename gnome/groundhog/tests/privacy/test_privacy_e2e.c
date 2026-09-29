/* G24 privacy acceptance harness (privacy charter §8.2 G24; a release gate).
 *
 * PT-1, PT-2, PT-4, PT-10, AT-5 (with attachments too: G21, whose Blossom
 * server is a local SoupServer, tests/media/blossom-fixture.c) and NO-1 of
 * charter §9.2, and PT-8's "a
 * message request triggers no lookup", run end to end: two Groundhog
 * accounts, Alice and Bob, each composed as gh-app-services.c composes one
 * Groundhog process (account controller, relay lists, the cached contact
 * directory with the durable outbox, the conversation model, the NIP-17
 * inbox, store key custody, the encrypted account store and the notifier).
 * Only the edges are stand-ins:
 *  - relays are local libsoup NIP-01/NIP-42 relays (H2: wire-relay.h in
 *    store-and-serve mode) reached through the real gnostr transports, so
 *    every frame on the wire is Groundhog's own;
 *  - org.nostr.Signer is the mock signer on a private bus
 *    (gh-test-signer.h). It holds the test keys; Groundhog holds none;
 *  - the Secret Service is FakeSecret (H5) and GSettings the memory backend,
 *    one of each per account, and org.gtk.Notifications the fake H4 server.
 *
 * Topology (PT-4). Bob's kind-10050 inbox relays are A and B, Alice's is C.
 * The discovery relays E and G, the discovery-relays setting of both
 * accounts, hold everyone's lists. A, B and C serve kind 1059 only to its
 * authenticated recipient and take EVENTs only from authenticated
 * connections. E answers any REQ (after sending a NIP-42 challenge that
 * nothing needs to answer); G demands AUTH for every REQ, so the contact
 * directory's ephemeral AUTH runs for real there, while own-list discovery,
 * which authenticates with nothing yet (charter §4.3 allows "none, or
 * ephemeral"; nostrc-qp24.67), is refused by G and served by E. D is Bob's
 * (and Carol's) kind-10002 relay, F a stranger's nprofile relay hint and M
 * the host of every URL in a message: bare TCP listeners that count
 * connection attempts and must count none. Carol (key 3) runs no Groundhog: she has a 10002 but
 * no 10050, and her messages are crafted here and handed to relay A.
 *
 * H7 (canary-scan.h) runs after every scenario over every file below the
 * test's XDG directories and TMPDIR (the encrypted stores and their -wal and
 * -shm files included, raw, both open and after close), both accounts'
 * GSettings, every frame on every relay in both directions, the captured
 * logs (G_MESSAGES_DEBUG=all: GLib's writer and the stdout/stderr
 * descriptors) and, for what a notification must never carry, every
 * notification payload.
 *
 * Nothing sleeps: every wait is for an observable condition, bounded only
 * to turn a hang into a failure (NO-1 also waits for the wall clock's second
 * to change; see wait_next_second()). PT-1's negative claim ("nothing is
 * published") is closed by a barrier instead of a quiet period: a reply sent
 * after the scripted actions and awaited until both of its wraps were
 * accepted and the reply received, so anything the script had caused to be
 * published would already be on the wire. */
#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include "canary-scan.h"
#include "fake-gtk-notifications.h"
#include "fake-secret.h"
#include "gh-account-store.h"
#include "gh-app-outbox.h"
#include "gh-notifier.h"
#include "gh-outbox.h"
#include "gh-store-conversations.h"
#include "gh-test-signer.h"
#include "wire-relay.h"
#if GROUNDHOG_TEST_ATTACHMENTS
#include "blossom-fixture.h"
#include "gh-attachment.h"
#endif

#include "nostr/nip19/nip19.h"
#include "nostr/nip59/nip59.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { ALICE = 1, BOB = 2, CAROL = 3 };

/* A failure bound for one awaited condition (sanitizer builds are slow). */
#define WAIT_SECONDS 90

static gchar *hex[GH_TEST_KEYS];
static gchar *npub[GH_TEST_KEYS];
static GhTestBus test_bus;
static GhTestSigner signer;
static FakeGtkNotifications *notifications;
static gchar *root;    /* every XDG directory and TMPDIR are below it */
static gchar *run_id;  /* makes this run's canaries unique */
static guint app_serial;

static const gchar *const xdg_names[] = { "data", "state", "cache", "config", "runtime", "tmp" };
static const gchar *const xdg_vars[] = { "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME",
                                         "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR", "TMPDIR" };

static gchar *
root_dir(const gchar *name)
{
  return g_build_filename(root, name, NULL);
}

/* ---- waiting ------------------------------------------------------------------ */

static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, const gchar *what, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(WAIT_SECONDS, gh_test_deadline_hit, &expired);
  guint tick = g_timeout_add(10, gh_test_tick, NULL);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
  if (expired)
    g_error("line %d: %s did not happen within %d s", line, what, WAIT_SECONDS);
  g_source_remove(timer);
}
#define spin_until(pred, data, what) spin_until_at((pred), (data), (what), __LINE__)

/* ---- tripwires: listeners that must never be contacted ------------------------ */

typedef struct {
  GSocketService *service;
  guint16 port;
  guint attempts;
} Tripwire;

static gboolean
tripwire_incoming(GSocketService *service, GSocketConnection *connection, GObject *source,
                  gpointer data)
{
  (void)service;
  (void)source;
  Tripwire *wire = data;
  wire->attempts++;
  g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
  return TRUE;
}

static void
tripwire_up(Tripwire *wire)
{
  g_autoptr(GError) error = NULL;
  wire->service = g_socket_service_new();
  g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  g_autoptr(GSocketAddress) address = g_inet_socket_address_new(loopback, 0);
  g_autoptr(GSocketAddress) effective = NULL;
  g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(wire->service), address,
                                              G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
                                              NULL, &effective, &error));
  g_assert_no_error(error);
  wire->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
  g_signal_connect(wire->service, "incoming", G_CALLBACK(tripwire_incoming), wire);
  g_socket_service_start(wire->service);
}

static void
tripwire_down(Tripwire *wire)
{
  g_signal_handlers_disconnect_by_data(wire->service, wire);
  g_socket_service_stop(wire->service);
  g_socket_listener_close(G_SOCKET_LISTENER(wire->service));
  g_clear_object(&wire->service);
}

static gchar *
tripwire_url(Tripwire *wire, const gchar *scheme, const gchar *path)
{
  return g_strdup_printf("%s://127.0.0.1:%u/%s", scheme, wire->port, path);
}

/* ---- signed fixtures ------------------------------------------------------------ */

static gchar *
sign_event(guint key, gint kind, gint64 created_at, const gchar *content, NostrTags *tags)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content);
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

/* A signed kind 10050 (tag "relay") or 10002 (tag "r"), as the account's own
 * list publication would have put it on the discovery relay. */
static void
seed_list(WireRelay *discovery, guint key, gint kind, const gchar *const *urls)
{
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; urls[i]; i++)
    nostr_tags_append(tags, nostr_tag_new(kind == 10050 ? "relay" : "r", urls[i], NULL));
  g_autofree gchar *json = sign_event(key, kind, g_get_real_time() / G_USEC_PER_SEC - 3600, "",
                                      tags);
  wire_relay_inject(discovery, json);
}

/* A NIP-17 rumor of kind 14 or 15 from key `from` to key `to`, sealed and
 * gift-wrapped (NIP-59) the way another client would: what Carol sends, and
 * a kind-15 file message as another client writes it. */
static gchar *
craft_wrap(guint from, guint to, gint kind, const gchar *content, NostrTags *extra_tags)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, kind);
  nostr_event_set_pubkey(rumor, hex[from]);
  nostr_event_set_created_at(rumor, g_get_real_time() / G_USEC_PER_SEC);
  nostr_event_set_content(rumor, content);
  NostrTags *tags = extra_tags ? extra_tags : nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", hex[to], NULL));
  nostr_event_set_tags(rumor, tags);
  rumor->id = nostr_event_get_id(rumor);
  char *rumor_json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);

  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[from], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, hex[to], sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)rumor_json, strlen(rumor_json),
                                          &ciphertext), ==, 0);
  free(rumor_json);
  NostrEvent *seal = nostr_event_new();
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, hex[from]);
  nostr_event_set_content(seal, ciphertext);
  nostr_event_set_created_at(seal, g_get_real_time() / G_USEC_PER_SEC - 60);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(seal, gh_test_secret[from]), ==, 0);
  guint8 ephemeral[32];
  g_assert_true(nostr_hex2bin(ephemeral, gh_test_secret[4], sizeof ephemeral));
  NostrEvent *wrap = nostr_nip59_wrap_with_key(seal, hex[to], ephemeral);
  nostr_event_free(seal);
  g_assert_nonnull(wrap);
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

static gchar *
room_of(guint a, guint b)
{
  return strcmp(hex[a], hex[b]) < 0 ? g_strconcat(hex[a], ",", hex[b], NULL)
                                    : g_strconcat(hex[b], ",", hex[a], NULL);
}

/* ---- one Groundhog process --------------------------------------------------- */

typedef struct {
  guint key;
  GSettingsBackend *backend;
  GSettings *settings;
  FakeSecret *secret;
  GApplication *application;
  GhClock *notify_clock; /* the notifier's (fake): its coalescing is driven here */
  /* The services, in gh-app-services.c's order. */
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhAppOutbox *sender;
  GhConversationStore *model;
  GhDmInbox *inbox;
  GhStoreKey *store_key;
  GhAccountStore *store;
  GhNotifier *notifier;
} App;

/* The signer holds all three test identities; each app selects its own. */
static GPtrArray *
list_identities(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  for (guint key = ALICE; key <= CAROL; key++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npub[key]);
    info->label = g_strdup_printf("Key %u", key);
    g_ptr_array_add(ids, info);
  }
  return ids;
}

/* gh-app-services.c's notifier_room_state(): mutes and blocks from the store. */
static gboolean
room_state(gpointer data, const gchar *room_id, GhNotifierRoomState *state)
{
  GhStoreConversations *store = gh_account_store_get_conversations(GH_ACCOUNT_STORE(data));
  GhStoreNotifyState stored = { 0 };
  if (store && !gh_store_conversations_get_notify_state(store, room_id, &stored, NULL))
    return FALSE;
  state->muted_until = stored.muted_until;
  state->blocked = stored.blocked;
  return TRUE;
}

static void
app_up(App *app, guint key, const gchar *const *discovery_urls)
{
  g_autoptr(GError) error = NULL;
  memset(app, 0, sizeof *app);
  app->key = key;
  app->backend = g_memory_settings_backend_new();
  app->settings = g_settings_new_with_backend("org.nostr.Groundhog", app->backend);
  g_settings_set_strv(app->settings, "discovery-relays", discovery_urls);
  g_settings_set_string(app->settings, "current-npub", npub[key]);
  /* Notifications on (the onboarding choice); their content level stays
   * the default, which NO-1 checks is "hidden". */
  g_settings_set_boolean(app->settings, "notifications-enabled", TRUE);
  app->secret = fake_secret_new();
  g_autofree gchar *id = g_strdup_printf("org.nostr.GroundhogTest.Privacy%u", ++app_serial);
  app->application = g_application_new(id, G_APPLICATION_DEFAULT_FLAGS);
  g_assert_true(g_application_register(app->application, NULL, &error));
  g_assert_no_error(error);
  app->notify_clock = gh_clock_new_fake(g_get_real_time());

  app->accounts = gh_account_controller_new_full(app->settings, test_bus.client,
                                                 list_identities, NULL);
  app->relays = gh_account_relays_new(app->accounts, app->settings, NULL, NULL);
  GhAppOutboxConfig sender = {
    .accounts = app->accounts,
    .account_relays = app->relays,
    .settings = app->settings,
  };
  app->sender = gh_app_outbox_new(&sender);
  app->model = gh_conversation_store_new();
  gh_app_outbox_set_conversations(app->sender, app->model);
  app->inbox = gh_dm_inbox_new_with_storage(app->accounts, app->relays, app->model, NULL, NULL,
                                            NULL);
  app->store_key = gh_store_key_new(GH_STORE_KEY_BACKEND(app->secret));
  GhAccountStoreConfig store = {
    .accounts = app->accounts,
    .store_key = app->store_key,
    .conversations = app->model,
    .inbox = app->inbox,
    .settings = app->settings,
    .create_outbox = gh_app_outbox_create,
    .outbox_data = app->sender,
  };
  app->store = gh_account_store_new(&store);
  GhNotifierConfig notifier = {
    .settings = app->settings,
    .conversations = app->model,
    .clock = app->notify_clock,
    .room_state = room_state,
    .room_state_data = app->store,
  };
  app->notifier = gh_notifier_new(app->application, &notifier);
}

/* Teardown in reverse order, as gh_app_services_free() does. */
static void
app_down(App *app)
{
  gh_test_release(g_steal_pointer(&app->notifier));
  gh_test_release(g_steal_pointer(&app->store));
  gh_test_release(g_steal_pointer(&app->store_key));
  gh_test_release(g_steal_pointer(&app->inbox));
  gh_app_outbox_set_conversations(app->sender, NULL);
  g_clear_object(&app->model);
  g_clear_pointer(&app->sender, gh_app_outbox_free);
  gh_test_release(g_steal_pointer(&app->relays));
  gh_test_release(g_steal_pointer(&app->accounts));
  g_clear_object(&app->application);
  g_clear_pointer(&app->notify_clock, gh_clock_unref);
  g_clear_object(&app->secret);
  g_clear_object(&app->settings);
  g_clear_object(&app->backend);
}

/* Store open, own lists discovered, inbox live and idle. */
static gboolean
app_ready(gpointer data)
{
  App *app = data;
  GhDmInboxCounters counters;
  gh_dm_inbox_get_counters(app->inbox, &counters);
  return gh_account_store_get_state(app->store) == GH_ACCOUNT_STORE_OPEN &&
         gh_account_relays_get_state(app->relays) == GH_ACCOUNT_RELAYS_COMPLETE &&
         gh_dm_inbox_get_state(app->inbox) == GH_DM_INBOX_LIVE && counters.pending == 0;
}

static GhOutbox *
app_outbox(App *app)
{
  GObject *outbox = gh_account_store_get_outbox(app->store);
  g_assert_nonnull(outbox);
  return GH_OUTBOX(outbox);
}

static GhOutboxItem *
app_send(App *app, guint to, const gchar *text)
{
  g_autoptr(GError) error = NULL;
  GhOutboxItem *item = gh_outbox_send(app_outbox(app), hex[to], text, &error);
  g_assert_no_error(error);
  g_assert_nonnull(item);
  return item;
}

static GhStoreConversations *
app_conversations(App *app)
{
  GhStoreConversations *conversations = gh_account_store_get_conversations(app->store);
  g_assert_nonnull(conversations);
  return conversations;
}

static GhMessage *
find_content(GhConversationStore *model, const gchar *content)
{
  for (guint c = 0; c < g_list_model_get_n_items(G_LIST_MODEL(model)); c++) {
    g_autoptr(GhConversation) conversation = g_list_model_get_item(G_LIST_MODEL(model), c);
    for (guint m = 0; m < g_list_model_get_n_items(G_LIST_MODEL(conversation)); m++) {
      g_autoptr(GhMessage) message = g_list_model_get_item(G_LIST_MODEL(conversation), m);
      if (g_strcmp0(gh_message_get_content(message), content) == 0)
        return message; /* the conversation keeps it */
    }
  }
  return NULL;
}

typedef struct {
  App *app;
  const gchar *content;
} ContentWait;

static gboolean
content_listed(gpointer data)
{
  ContentWait *wait = data;
  return find_content(wait->app->model, wait->content) != NULL;
}

static void
wait_received(App *app, const gchar *content)
{
  ContentWait wait = { app, content };
  spin_until(content_listed, &wait, "the message reaching its recipient's model");
}

static gboolean
item_settled(gpointer data)
{
  return gh_outbox_item_get_state(data) == GH_STORE_OUTBOX_SETTLED;
}

typedef struct {
  GhOutboxItem *item;
  GhMessageStatus status;
} StatusWait;

static gboolean
status_is(gpointer data)
{
  StatusWait *wait = data;
  return gh_outbox_item_get_status(wait->item) == wait->status;
}

static gboolean
inbox_idle(gpointer data)
{
  App *app = data;
  GhDmInboxCounters counters;
  gh_dm_inbox_get_counters(app->inbox, &counters);
  return gh_dm_inbox_get_state(app->inbox) == GH_DM_INBOX_LIVE && counters.pending == 0;
}

/* Sends and waits until both wraps were accepted and the recipient lists it. */
static GhOutboxItem *
send_and_deliver(App *from, App *to, const gchar *text)
{
  GhOutboxItem *item = app_send(from, to->key, text);
  spin_until(item_settled, item, "the outbox settling the message");
  wait_received(to, text);
  spin_until(inbox_idle, to, "the recipient's inbox going idle");
  spin_until(inbox_idle, from, "the sender's inbox taking its self-copy");
  return item;
}

/* ---- notifications (H4) ------------------------------------------------------- */

static const gchar *
app_id(App *app)
{
  return g_application_get_application_id(app->application);
}

/* Runs what the notifier scheduled for the next turn and records its calls. */
static void
notify_settle(App *app)
{
  gh_test_run_until_idle();
  gh_clock_fake_advance(app->notify_clock, 1);
  fake_gtk_notifications_sync(notifications,
                              g_application_get_dbus_connection(app->application));
}

/* Lets a new coalescing window open for every id (N2). */
static void
notify_next_window(App *app)
{
  gh_clock_fake_advance(app->notify_clock, GH_NOTIFIER_UPDATE_INTERVAL_MS * 1000);
  notify_settle(app);
}

/* What app currently shows under id, or NULL. Borrowed. */
static GVariant *
shown(App *app, const gchar *id)
{
  GPtrArray *calls = fake_gtk_notifications_get_calls(notifications);
  for (guint i = calls->len; i-- > 0;) {
    FakeNotificationCall *call = g_ptr_array_index(calls, i);
    if (g_str_equal(call->app_id, app_id(app)) && g_str_equal(call->id, id))
      return call->added ? call->notification : NULL;
  }
  return NULL;
}

static guint
count_added(App *app)
{
  guint n = 0;
  GPtrArray *calls = fake_gtk_notifications_get_calls(notifications);
  for (guint i = 0; i < calls->len; i++) {
    FakeNotificationCall *call = g_ptr_array_index(calls, i);
    n += call->added && g_str_equal(call->app_id, app_id(app));
  }
  return n;
}

static const gchar *
field(GVariant *notification, const gchar *key)
{
  const gchar *value = NULL;
  if (!notification || !g_variant_lookup(notification, key, "&s", &value))
    return NULL;
  return value;
}

/* Every payload app sent, printed. */
static gchar *
payloads_of(App *app)
{
  GString *out = g_string_new(NULL);
  GPtrArray *calls = fake_gtk_notifications_get_calls(notifications);
  for (guint i = 0; i < calls->len; i++) {
    FakeNotificationCall *call = g_ptr_array_index(calls, i);
    if (!call->notification || !g_str_equal(call->app_id, app_id(app)))
      continue;
    g_autofree gchar *printed = g_variant_print(call->notification, TRUE);
    g_string_append_printf(out, "%s %s\n", call->id, printed);
  }
  return g_string_free(out, FALSE);
}

/* NO-1 hidden: nothing that names who wrote or what. The npub is looked for
 * by its prefix and suffix too, since a short form elides its middle. */
static void
assert_payloads_hide(App *app, guint author, const gchar *const *secrets)
{
  g_autofree gchar *payloads = payloads_of(app);
  g_assert_cmpstr(payloads, !=, "");
  g_autofree gchar *npub_head = g_strndup(npub[author], 12);
  const gchar *npub_tail = npub[author] + strlen(npub[author]) - 6;
  const gchar *identity[] = { hex[author], npub[author], npub_head, npub_tail };
  for (guint i = 0; i < G_N_ELEMENTS(identity); i++)
    g_assert_null(strstr(payloads, identity[i]));
  for (guint i = 0; secrets && secrets[i]; i++)
    g_assert_null(strstr(payloads, secrets[i]));
}

/* ---- relay traffic (H2 frames) -------------------------------------------------- */

typedef struct {
  WireRelay a, b; /* Bob's inbox relays */
  WireRelay c;    /* Alice's inbox relay */
  WireRelay e;    /* discovery, open */
  WireRelay g;    /* discovery, AUTH required */
  Tripwire d;     /* Bob's and Carol's kind-10002 relay */
  Tripwire f;     /* a stranger's nprofile relay hint */
  Tripwire m;     /* the host of every URL sent in a message */
} Net;

static void
inbox_relay_up(WireRelay *relay)
{
  relay->serve = relay->auth_gate_dms = relay->auth_writes = relay->record = TRUE;
  relay_init(relay);
}

/* gated: every REQ needs AUTH. Otherwise a challenge is still sent on
 * connect (auth_gate_dms; nobody asks a discovery relay for kind 1059), so
 * any AUTH a client offered unasked would show. */
static void
discovery_relay_up(WireRelay *relay, gboolean gated)
{
  relay->serve = relay->record = TRUE;
  if (gated)
    relay->require_auth = TRUE;
  else
    relay->auth_gate_dms = TRUE;
  relay_init(relay);
}

/* Every wire relay of the topology, for loops: A, B, C, E, G. */
#define N_WIRE 5
static void
net_relays(Net *net, WireRelay *out[N_WIRE])
{
  out[0] = &net->a;
  out[1] = &net->b;
  out[2] = &net->c;
  out[3] = &net->e;
  out[4] = &net->g;
}

/* The client's frame type: "EVENT", "REQ", "CLOSE", "AUTH" or NULL. */
static const gchar *
frame_type(const gchar *text)
{
  static const gchar *const types[] = { "EVENT", "REQ", "CLOSE", "AUTH" };
  for (guint i = 0; i < G_N_ELEMENTS(types); i++) {
    g_autofree gchar *prefix = g_strdup_printf("[\"%s\"", types[i]);
    if (g_str_has_prefix(text, prefix))
      return types[i];
  }
  return NULL;
}

/* The signed event of a client ["EVENT",{...}] or ["AUTH",{...}] frame. */
static NostrEvent *
frame_event(const gchar *text)
{
  const gchar *type = frame_type(text);
  g_assert_true(g_strcmp0(type, "EVENT") == 0 || g_strcmp0(type, "AUTH") == 0);
  g_autofree gchar *prefix = g_strdup_printf("[\"%s\",", type);
  g_autofree gchar *json = wire_frame_payload(text, prefix);
  g_assert_nonnull(json);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  return event;
}

static gchar *
event_id_of(NostrEvent *event)
{
  char *id = nostr_event_get_id(event);
  gchar *out = g_strdup(id);
  free(id);
  return out;
}

static const gchar *
p_tag_of(NostrEvent *event)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), "p") == 0)
      return nostr_tag_get_value(tag);
  }
  return NULL;
}

/* For g_ptr_array_sort() of hex keys. */
static gint
compare_hex(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static gboolean
is_account(const gchar *pubkey)
{
  for (guint key = 1; key < GH_TEST_KEYS; key++)
    if (g_strcmp0(pubkey, hex[key]) == 0)
      return TRUE;
  return FALSE;
}

/* What a client did on one connection, from frame index `from` on. */
typedef struct {
  guint serial;
  guint events;
  guint reqs;
  guint closes;
  GPtrArray *event_ids; /* distinct ids of its EVENT frames */
  GPtrArray *auth_keys; /* pubkeys of its AUTH frames, in order */
  GPtrArray *reqs_text; /* its REQ frames */
} Conn;

static void
conn_free(gpointer data)
{
  Conn *conn = data;
  g_ptr_array_unref(conn->event_ids);
  g_ptr_array_unref(conn->auth_keys);
  g_ptr_array_unref(conn->reqs_text);
  g_free(conn);
}

static gboolean
str_in(GPtrArray *array, const gchar *value)
{
  for (guint i = 0; array && i < array->len; i++)
    if (g_strcmp0(g_ptr_array_index(array, i), value) == 0)
      return TRUE;
  return FALSE;
}

/* serial -> Conn for every connection that sent a frame since `from`. */
static GHashTable *
conns_since(WireRelay *relay, guint from)
{
  GHashTable *conns = g_hash_table_new_full(NULL, NULL, NULL, conn_free);
  for (guint i = from; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (!frame->inbound)
      continue;
    Conn *conn = g_hash_table_lookup(conns, GUINT_TO_POINTER(frame->connection));
    if (!conn) {
      conn = g_new0(Conn, 1);
      conn->serial = frame->connection;
      conn->event_ids = g_ptr_array_new_with_free_func(g_free);
      conn->auth_keys = g_ptr_array_new_with_free_func(g_free);
      conn->reqs_text = g_ptr_array_new_with_free_func(g_free);
      g_hash_table_insert(conns, GUINT_TO_POINTER(frame->connection), conn);
    }
    const gchar *type = frame_type(frame->text);
    g_assert_nonnull(type); /* nothing but NIP-01/NIP-42 client frames */
    if (g_str_equal(type, "EVENT")) {
      conn->events++;
      NostrEvent *event = frame_event(frame->text);
      g_autofree gchar *id = event_id_of(event);
      if (!str_in(conn->event_ids, id))
        g_ptr_array_add(conn->event_ids, g_steal_pointer(&id));
      nostr_event_free(event);
    } else if (g_str_equal(type, "AUTH")) {
      NostrEvent *event = frame_event(frame->text);
      g_assert_cmpint(nostr_event_get_kind(event), ==, 22242);
      g_ptr_array_add(conn->auth_keys, g_strdup(nostr_event_get_pubkey(event)));
      nostr_event_free(event);
    } else if (g_str_equal(type, "REQ")) {
      conn->reqs++;
      g_ptr_array_add(conn->reqs_text, g_strdup(frame->text));
    } else {
      conn->closes++;
    }
  }
  return conns;
}

/* The kinds and single author of one filter of a REQ frame. */
typedef struct {
  GArray *kinds;  /* gint */
  gchar *author;  /* the only author, or NULL */
  guint n_authors;
} ReqShape;

static void
req_shapes(const gchar *text, void (*each)(const ReqShape *shape, gpointer data), gpointer data)
{
  NostrEnvelope *envelope = nostr_envelope_parse(text);
  g_assert_nonnull(envelope);
  g_assert_cmpint(nostr_envelope_get_type(envelope), ==, NOSTR_ENVELOPE_REQ);
  NostrFilters *filters = nostr_req_envelope_get_filters((NostrReqEnvelope *)envelope);
  for (size_t i = 0; i < filters->count; i++) {
    NostrFilter *filter = &filters->filters[i];
    ReqShape shape = { g_array_new(FALSE, FALSE, sizeof(gint)), NULL, 0 };
    for (size_t k = 0; k < nostr_filter_kinds_len(filter); k++) {
      gint kind = nostr_filter_kinds_get(filter, k);
      g_array_append_val(shape.kinds, kind);
    }
    shape.n_authors = nostr_filter_authors_len(filter);
    if (shape.n_authors == 1)
      shape.author = g_strdup(nostr_filter_authors_get(filter, 0));
    each(&shape, data);
    g_array_unref(shape.kinds);
    g_free(shape.author);
  }
  nostr_envelope_free(envelope);
}

static gboolean
shape_has_kind(const ReqShape *shape, gint kind)
{
  for (guint i = 0; i < shape->kinds->len; i++)
    if (g_array_index(shape->kinds, gint, i) == kind)
      return TRUE;
  return FALSE;
}

/* A discovery-relay REQ is one of exactly two purposes (charter §4.3): the
 * account's own lists (kinds 10002 and 10050 by itself) or a contact
 * directory lookup (kind 10050, or kind 0 of an accepted contact, by one
 * other author). Counts directory lookups per author. */
typedef struct {
  const gchar *author;  /* whose lookups to count */
  guint lookups;
} DiscoveryCheck;

static void
check_discovery_shape(const ReqShape *shape, gpointer data)
{
  DiscoveryCheck *check = data;
  g_assert_cmpuint(shape->n_authors, ==, 1);
  g_assert_true(is_account(shape->author));
  for (guint i = 0; i < shape->kinds->len; i++) {
    gint kind = g_array_index(shape->kinds, gint, i);
    g_assert_true(kind == 0 || kind == 10002 || kind == 10050);
  }
  gboolean own_lists = shape->kinds->len == 2 && shape_has_kind(shape, 10002) &&
                       shape_has_kind(shape, 10050);
  gboolean directory = !shape_has_kind(shape, 10002);
  g_assert_true(own_lists || directory);
  if (directory && check && g_strcmp0(shape->author, check->author) == 0)
    check->lookups++;
}

/* Every REQ a discovery relay saw is own-list discovery or a directory
 * lookup; returns the connections that looked `author` up in the
 * directory. */
static guint
discovery_lookups(WireRelay *e, const gchar *author)
{
  guint connections = 0;
  g_autoptr(GHashTable) conns = conns_since(e, 0);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, conns);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Conn *conn = value;
    g_assert_cmpuint(conn->events, ==, 0); /* nothing is published to discovery */
    DiscoveryCheck check = { author, 0 };
    for (guint i = 0; i < conn->reqs_text->len; i++)
      req_shapes(g_ptr_array_index(conn->reqs_text, i), check_discovery_shape, &check);
    connections += check.lookups > 0;
  }
  return connections;
}

/* Whether any client frame on relay (since `from`) mentions text. */
static gboolean
client_frames_mention(WireRelay *relay, guint from, const gchar *text)
{
  for (guint i = from; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && strstr(frame->text, text))
      return TRUE;
  }
  return FALSE;
}

static gboolean
any_frames_mention(WireRelay *relay, const gchar *text)
{
  for (guint i = 0; i < relay->frames->len; i++)
    if (strstr(((WireFrame *)g_ptr_array_index(relay->frames, i))->text, text))
      return TRUE;
  return FALSE;
}

static void
check_no_kind0(const ReqShape *shape, gpointer data)
{
  (void)data;
  g_assert_false(shape_has_kind(shape, 0));
}

/* No REQ on relay asked for any profile (kind 0). */
static void
assert_no_profile_reqs(WireRelay *relay)
{
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && g_strcmp0(frame_type(frame->text), "REQ") == 0)
      req_shapes(frame->text, check_no_kind0, NULL);
  }
}

/* ---- the world: two accounts on one relay topology ---------------------------------- */

typedef struct {
  Net net;
  App alice, bob;
  CanaryScan *scan;        /* files, settings, relay frames, logs */
  CanaryScan *notify_scan; /* notification payloads */
  GPtrArray *strings;
  guint n_canaries;
} World;

/* A unique plaintext marker. Every canary must stay out of files, settings,
 * logs and relay traffic; one that is not preview_ok must stay out of
 * notifications too. */
static const gchar *
world_canary(World *w, const gchar *label, gboolean preview_ok)
{
  gchar *canary = g_strdup_printf("ghcanary-%s-%s-%u", run_id, label, ++w->n_canaries);
  canary_scan_add(w->scan, label, canary);
  if (!preview_ok)
    canary_scan_add(w->notify_scan, label, canary);
  g_ptr_array_add(w->strings, canary);
  return canary;
}

static void
world_secret(World *w, const gchar *label, const gchar *secret, gboolean literal)
{
  CanaryScan *scans[] = { w->scan, w->notify_scan };
  for (guint i = 0; i < G_N_ELEMENTS(scans); i++) {
    if (literal)
      canary_scan_add_literal(scans[i], label, secret);
    else
      canary_scan_add(scans[i], label, secret);
  }
}

static void
world_up(World *w)
{
  memset(w, 0, sizeof *w);
  fake_gtk_notifications_clear(notifications);
  canary_log_capture_reset();
  w->scan = canary_scan_new();
  w->notify_scan = canary_scan_new();
  w->strings = g_ptr_array_new_with_free_func(g_free);
  /* The accounts' keys live in the signer only: never in anything scanned. */
  for (guint key = ALICE; key <= CAROL; key++) {
    guint8 sk[32];
    g_assert_true(nostr_hex2bin(sk, gh_test_secret[key], sizeof sk));
    char *nsec = NULL;
    g_assert_cmpint(nostr_nip19_encode_nsec(sk, &nsec), ==, 0);
    world_secret(w, "account nsec", nsec, FALSE);
    world_secret(w, "account secret hex", gh_test_secret[key], TRUE);
    free(nsec);
  }

  Net *net = &w->net;
  inbox_relay_up(&net->a);
  inbox_relay_up(&net->b);
  inbox_relay_up(&net->c);
  discovery_relay_up(&net->e, FALSE);
  discovery_relay_up(&net->g, TRUE);
  tripwire_up(&net->d);
  tripwire_up(&net->f);
  tripwire_up(&net->m);
  g_autofree gchar *d_url = tripwire_url(&net->d, "ws", "relay");
  const gchar *bob_inbox[] = { net->a.url, net->b.url, NULL };
  const gchar *alice_inbox[] = { net->c.url, NULL };
  const gchar *outbox_d[] = { d_url, NULL };
  WireRelay *discovery[] = { &net->e, &net->g };
  for (guint i = 0; i < G_N_ELEMENTS(discovery); i++) {
    seed_list(discovery[i], BOB, 10050, bob_inbox);
    seed_list(discovery[i], ALICE, 10050, alice_inbox);
    seed_list(discovery[i], BOB, 10002, outbox_d);
    seed_list(discovery[i], CAROL, 10002, outbox_d);
  }

  const gchar *sources[] = { net->e.url, net->g.url, NULL };
  app_up(&w->alice, ALICE, sources);
  app_up(&w->bob, BOB, sources);
  spin_until(app_ready, &w->alice, "Alice's store, lists and inbox coming up");
  spin_until(app_ready, &w->bob, "Bob's store, lists and inbox coming up");
  const gchar *const *relays = gh_dm_inbox_get_relays(w->bob.inbox);
  g_assert_cmpuint(g_strv_length((gchar **)relays), ==, 2);
  g_assert_true(g_strv_contains(relays, net->a.url) && g_strv_contains(relays, net->b.url));
  relays = gh_dm_inbox_get_relays(w->alice.inbox);
  g_assert_cmpuint(g_strv_length((gchar **)relays), ==, 1);
  g_assert_cmpstr(relays[0], ==, net->c.url);

  /* Proof that the capture sees both debug-level GLib messages and bytes a
   * library writes straight to stderr. */
  g_debug("privacy harness log probe %s", run_id);
  fprintf(stderr, "privacy harness fd probe %s\n", run_id);
}

static gchar *
store_path(App *app, const gchar *suffix)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = gh_store_account_dir_path(NULL, hex[app->key], &error);
  g_assert_no_error(error);
  g_autofree gchar *name = g_strconcat("store.db", suffix, NULL);
  return g_build_filename(dir, name, NULL);
}

/* ST-1's precondition, so that H7 finding nothing in store.db means
 * something: the file is there, non-empty, and not a plain SQLite file. */
static void
assert_store_encrypted(App *app)
{
  g_autofree gchar *path = store_path(app, "");
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
  g_assert_cmpuint(length, >=, 4096);
  g_assert_cmpint(memcmp(contents, "SQLite format 3", 16), !=, 0);
}

static void
scan_frames(CanaryScan *scan, WireRelay *relay, const gchar *name)
{
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    g_autofree gchar *source = g_strdup_printf("relay %s, connection %u, %s frame %u", name,
                                               frame->connection,
                                               frame->inbound ? "client" : "relay", i);
    canary_scan_text(scan, source, frame->text);
  }
}

/* H7 over everything but the notifications, then the notifications. */
static void
world_scan(World *w, const gchar *what)
{
  CanaryScan *scan = w->scan;
  canary_scan_clear_hits(scan);
  guint files = 0;
  canary_scan_tree(scan, root, &files);
  g_assert_cmpuint(files, >, 0);
  canary_scan_settings(scan, "Alice's GSettings", w->alice.settings);
  canary_scan_settings(scan, "Bob's GSettings", w->bob.settings);
  scan_frames(scan, &w->net.a, "A");
  scan_frames(scan, &w->net.b, "B");
  scan_frames(scan, &w->net.c, "C");
  scan_frames(scan, &w->net.e, "E");
  scan_frames(scan, &w->net.g, "G");
  canary_log_capture_sync();
  g_autofree gchar *logs = canary_log_capture_dup();
  g_autofree gchar *log_probe = g_strdup_printf("privacy harness log probe %s", run_id);
  g_autofree gchar *fd_probe = g_strdup_printf("privacy harness fd probe %s", run_id);
  g_assert_nonnull(strstr(logs, log_probe));
  g_assert_nonnull(strstr(logs, fd_probe));
  canary_scan_text(scan, "captured logs", logs);
  g_autofree gchar *where = g_strdup_printf("%s: files, settings, relay frames and logs", what);
  canary_scan_check_clean(scan, where);

  canary_scan_clear_hits(w->notify_scan);
  g_autofree gchar *dump = fake_gtk_notifications_dump(notifications);
  canary_scan_text(w->notify_scan, "notification payloads", dump);
  g_autofree gchar *where_notified = g_strdup_printf("%s: notification payloads", what);
  canary_scan_check_clean(w->notify_scan, where_notified);

  assert_store_encrypted(&w->alice);
  assert_store_encrypted(&w->bob);
  g_assert_cmpuint(w->net.d.attempts, ==, 0);
  g_assert_cmpuint(w->net.f.attempts, ==, 0);
  g_assert_cmpuint(w->net.m.attempts, ==, 0);
}

static void
world_down(World *w)
{
  app_down(&w->bob);
  app_down(&w->alice);
  GhTestSenders check = { &test_bus, &signer };
  spin_until(gh_test_signer_senders_closed, &check, "every signer request connection closing");
  /* Closed and checkpointed stores, and anything written on the way out. */
  canary_scan_clear_hits(w->scan);
  canary_scan_tree(w->scan, root, NULL);
  canary_scan_check_clean(w->scan, "files after both stores closed");

  relay_clear(&w->net.a);
  relay_clear(&w->net.b);
  relay_clear(&w->net.c);
  relay_clear(&w->net.e);
  relay_clear(&w->net.g);
  tripwire_down(&w->net.d);
  tripwire_down(&w->net.f);
  tripwire_down(&w->net.m);
  /* The next scenario starts without stores (its keyrings are new). */
  for (guint i = 0; i < G_N_ELEMENTS(xdg_names) - 1; i++) {
    g_autofree gchar *dir = root_dir(xdg_names[i]);
    g_autofree gchar *groundhog = g_build_filename(dir, "groundhog", NULL);
    gh_test_remove_tree(groundhog);
  }
  canary_scan_free(w->scan);
  canary_scan_free(w->notify_scan);
  g_ptr_array_unref(w->strings);
}

/* ---- honest status ------------------------------------------------------------ */

/* PD-1/UX-5: a status never claims delivery or reading. */
static void
assert_honest_status(GhOutboxItem *item)
{
  const gchar *claims = "\\b(deliver(ed|y)?|read|seen|opened)\\b";
  g_assert_false(g_regex_match_simple(claims, gh_outbox_item_get_label(item),
                                      G_REGEX_CASELESS, 0));
  g_assert_false(g_regex_match_simple(claims, gh_outbox_item_get_accessible_description(item),
                                      G_REGEX_CASELESS, 0));
  g_autoptr(GEnumClass) statuses = g_type_class_ref(GH_TYPE_MESSAGE_STATUS);
  for (guint i = 0; i < statuses->n_values; i++) {
    g_assert_false(g_regex_match_simple(claims, statuses->values[i].value_nick,
                                        G_REGEX_CASELESS, 0));
    g_assert_false(g_regex_match_simple("deliver|read", statuses->values[i].value_name,
                                        G_REGEX_CASELESS, 0));
  }
}

/* ---- PT-4 relay minimization ----------------------------------------------------- */

static void
assert_targets(GhOutboxItem *item, const gchar *const *recipient_urls,
               const gchar *const *self_urls)
{
  g_autoptr(GPtrArray) targets = gh_outbox_item_dup_targets(item);
  guint recipient = 0, self = 0;
  for (guint i = 0; i < targets->len; i++) {
    GhOutboxTarget *target = g_ptr_array_index(targets, i);
    g_assert_cmpint(target->outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
    if (target->role == GH_STORE_OUTBOX_ROLE_SELF_WRAP) {
      g_assert_true(g_strv_contains(self_urls, target->url));
      self++;
    } else {
      g_assert_true(g_strv_contains(recipient_urls, target->url));
      recipient++;
    }
  }
  g_assert_cmpuint(recipient, ==, g_strv_length((gchar **)recipient_urls));
  g_assert_cmpuint(self, ==, g_strv_length((gchar **)self_urls));
}

/* The one publish connection on relay since `from`: it sent exactly the
 * wrap for `recipient`, authenticated once with `auth_key` (NULL: a fresh
 * key that is no account's) and did nothing else. Returns the wrap id. */
static gchar *
assert_one_publish(WireRelay *relay, guint from, const gchar *recipient, const gchar *auth_key,
                   gchar **ephemeral_out)
{
  g_autoptr(GHashTable) conns = conns_since(relay, from);
  Conn *publish = NULL;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, conns);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Conn *conn = value;
    if (conn->events == 0)
      continue;
    g_assert_null(publish); /* exactly one publish connection */
    publish = conn;
  }
  g_assert_nonnull(publish);
  /* S3/PD-6: a publish never shares a connection with a subscription. */
  g_assert_cmpuint(publish->reqs, ==, 0);
  g_assert_cmpuint(publish->event_ids->len, ==, 1);
  g_assert_cmpuint(publish->auth_keys->len, ==, 1);
  const gchar *key = g_ptr_array_index(publish->auth_keys, 0);
  if (auth_key)
    g_assert_cmpstr(key, ==, auth_key);
  else
    g_assert_false(is_account(key));
  if (ephemeral_out)
    *ephemeral_out = g_strdup(key);
  /* The wrap itself: kind 1059 to its recipient, from a key that is not the
   * sender's (NIP-59), as the relay stored it. */
  const gchar *id = g_ptr_array_index(publish->event_ids, 0);
  WireStored *stored = NULL;
  for (guint i = 0; i < relay->stored->len; i++)
    if (g_str_equal(((WireStored *)g_ptr_array_index(relay->stored, i))->id, id))
      stored = g_ptr_array_index(relay->stored, i);
  g_assert_nonnull(stored);
  g_assert_cmpint(nostr_event_get_kind(stored->event), ==, 1059);
  g_assert_cmpstr(p_tag_of(stored->event), ==, recipient);
  g_assert_false(is_account(nostr_event_get_pubkey(stored->event)));
  return g_strdup(id);
}

static guint
event_frames(WireRelay *relay)
{
  guint n = 0;
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    n += frame->inbound && g_strcmp0(frame_type(frame->text), "EVENT") == 0;
  }
  return n;
}

static void
test_pt4_relay_minimization(void)
{
  World w;
  world_up(&w);
  Net *net = &w.net;
  guint from_a = net->a.frames->len, from_b = net->b.frames->len, from_c = net->c.frames->len;
  g_assert_cmpuint(discovery_lookups(&net->e, hex[BOB]), ==, 0);

  /* a) Sending to Bob: the recipient wrap to exactly A and B, the self-copy
   * to exactly C, nothing to D, and E only for the directory lookup. */
  const gchar *text = world_canary(&w, "pt4-first", FALSE);
  g_autoptr(GhOutboxItem) item = send_and_deliver(&w.alice, &w.bob, text);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_false(gh_outbox_item_get_self_copy_missing(item));
  assert_honest_status(item);
  const gchar *bob_inbox[] = { net->a.url, net->b.url, NULL };
  const gchar *alice_inbox[] = { net->c.url, NULL };
  assert_targets(item, bob_inbox, alice_inbox);
  g_autofree gchar *key_a = NULL, *key_b = NULL;
  g_autofree gchar *wrap_a = assert_one_publish(&net->a, from_a, hex[BOB], NULL, &key_a);
  g_autofree gchar *wrap_b = assert_one_publish(&net->b, from_b, hex[BOB], NULL, &key_b);
  /* One wrap per recipient, not per relay; an unrelated AUTH key per
   * connection (R2). */
  g_assert_cmpstr(wrap_a, ==, wrap_b);
  g_assert_cmpstr(key_a, !=, key_b);
  /* The self-copy authenticates as Alice on her own inbox (SELF_WRAP). */
  g_autofree gchar *self_wrap = assert_one_publish(&net->c, from_c, hex[ALICE], hex[ALICE],
                                                   NULL);
  g_assert_cmpstr(self_wrap, !=, wrap_a);
  /* No account AUTH on the recipient's relays, and nothing there names the
   * sender at all: A and B learn Bob's inbox traffic, not Alice. */
  g_assert_false(any_frames_mention(&net->a, hex[ALICE]));
  g_assert_false(any_frames_mention(&net->b, hex[ALICE]));
  for (guint i = 0; i < net->a.auth_pubkeys->len; i++)
    g_assert_cmpstr(g_ptr_array_index(net->a.auth_pubkeys, i), !=, hex[ALICE]);
  for (guint i = 0; i < net->b.auth_pubkeys->len; i++)
    g_assert_cmpstr(g_ptr_array_index(net->b.auth_pubkeys, i), !=, hex[ALICE]);
  /* Alice's own inbox never learns whom she writes to. */
  g_assert_false(any_frames_mention(&net->c, hex[BOB]));
  /* Discovery: one directory lookup of Bob on each discovery relay. The
   * open one saw no AUTH at all (AUTH is lazy); the gated one only fresh
   * ephemeral keys, one per connection, and the account's own-list REQ
   * there was refused rather than signed in as the account. */
  g_assert_cmpuint(discovery_lookups(&net->e, hex[BOB]), ==, 1);
  g_assert_cmpuint(discovery_lookups(&net->g, hex[BOB]), ==, 1);
  g_assert_cmpuint(net->e.auth_frames, ==, 0);
  g_assert_cmpuint(net->g.auth_pubkeys->len, >=, 1);
  for (guint i = 0; i < net->g.auth_pubkeys->len; i++) {
    const gchar *key = g_ptr_array_index(net->g.auth_pubkeys, i);
    g_assert_false(is_account(key));
    for (guint j = 0; j < i; j++)
      g_assert_cmpstr(key, !=, g_ptr_array_index(net->g.auth_pubkeys, j));
  }
  g_assert_cmpuint(net->g.closed_reqs, >=, 2); /* both accounts' own lists */
  g_assert_cmpuint(event_frames(&net->e), ==, 0);
  g_assert_cmpuint(event_frames(&net->g), ==, 0);
  g_assert_cmpuint(net->d.attempts, ==, 0);

  /* Bob reads it: no receipt, and Alice's status stays "Sent". */
  g_autofree gchar *room = room_of(ALICE, BOB);
  gh_conversation_mark_read(gh_conversation_store_lookup(w.bob.model, room));
  gh_test_run_until_idle();
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);

  /* Again, with Bob's inbox list cached: no discovery traffic at all. */
  from_a = net->a.frames->len;
  from_b = net->b.frames->len;
  from_c = net->c.frames->len;
  guint e_frames = net->e.frames->len, e_connections = net->e.connections->len;
  guint g_frames = net->g.frames->len, g_connections = net->g.connections->len;
  const gchar *again = world_canary(&w, "pt4-cached", FALSE);
  g_autoptr(GhOutboxItem) second = send_and_deliver(&w.alice, &w.bob, again);
  g_assert_cmpint(gh_outbox_item_get_status(second), ==, GH_MESSAGE_STATUS_SENT);
  assert_targets(second, bob_inbox, alice_inbox);
  g_autofree gchar *second_a = assert_one_publish(&net->a, from_a, hex[BOB], NULL, NULL);
  g_autofree gchar *second_b = assert_one_publish(&net->b, from_b, hex[BOB], NULL, NULL);
  g_autofree gchar *second_c = assert_one_publish(&net->c, from_c, hex[ALICE], hex[ALICE],
                                                  NULL);
  g_assert_cmpstr(second_a, ==, second_b);
  g_assert_cmpuint(net->e.connections->len, ==, e_connections);
  g_assert_cmpuint(net->e.frames->len, ==, e_frames);
  g_assert_cmpuint(net->g.connections->len, ==, g_connections);
  g_assert_cmpuint(net->g.frames->len, ==, g_frames);

  /* b) Carol has no kind 10050, only a 10002 naming D: nothing is sealed or
   * published (no self-copy either), D is never tried, and the status says
   * why. Her inbox is looked for on the discovery relays only. */
  WireRelay *relays[N_WIRE];
  net_relays(net, relays);
  guint events[N_WIRE];
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    events[i] = event_frames(relays[i]);
  guint signer_calls = signer.calls;
  const gchar *to_carol = world_canary(&w, "pt4-no-inbox", FALSE);
  g_autoptr(GhOutboxItem) stuck = app_send(&w.alice, CAROL, to_carol);
  StatusWait cannot = { stuck, GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX };
  spin_until(status_is, &cannot, "the no-inbox status");
  gh_test_run_until_idle();
  g_assert_cmpint(gh_outbox_item_get_state(stuck), ==, GH_STORE_OUTBOX_NEEDS_ATTENTION);
  assert_honest_status(stuck);
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    g_assert_cmpuint(event_frames(relays[i]), ==, events[i]);
  g_assert_cmpuint(signer.calls, ==, signer_calls);
  g_assert_cmpuint(discovery_lookups(&net->e, hex[CAROL]), ==, 1);
  g_assert_cmpuint(discovery_lookups(&net->g, hex[CAROL]), ==, 1);
  g_assert_cmpuint(net->d.attempts, ==, 0);

  world_scan(&w, "PT-4");
  world_down(&w);
}

/* PT-4 for a NIP-17 room (W17, nostrc-qp24.78): Alice writes to Bob and
 * Carol together. One rumor names both; Bob's own wrap goes to exactly his
 * inbox relays A and B, each on its own connection with a fresh ephemeral
 * AUTH, the self-copy to C as Alice, and nothing at all is published for
 * Carol, who has no kind 10050 (her 10002 relay D is never tried). No relay
 * frame names a room member other than the wrap's receiver, so A and B
 * cannot tell who else is in the room. Alice's status is "Sent to some
 * people", Carol's state "no inbox"; Bob lists the message in the
 * three-person room. H7 then finds the text nowhere in plaintext. */
static void
test_pt4_room(void)
{
  World w;
  world_up(&w);
  Net *net = &w.net;
  guint from_a = net->a.frames->len, from_b = net->b.frames->len, from_c = net->c.frames->len;
  WireRelay *relays[N_WIRE];
  net_relays(net, relays);
  guint events[N_WIRE];
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    events[i] = event_frames(relays[i]);

  const gchar *text = world_canary(&w, "pt4-room", FALSE);
  const gchar *const room[] = { hex[BOB], hex[CAROL], NULL };
  g_autoptr(GError) error = NULL;
  g_autoptr(GhOutboxItem) item = gh_outbox_send_room(app_outbox(&w.alice), room, text, &error);
  g_assert_no_error(error);
  g_assert_nonnull(item);
  StatusWait partial = { item, GH_MESSAGE_STATUS_PARTIALLY_SENT };
  spin_until(status_is, &partial, "the room message reaching Bob only");
  wait_received(&w.bob, text);
  spin_until(inbox_idle, &w.bob, "Bob's inbox going idle");
  spin_until(inbox_idle, &w.alice, "Alice's inbox taking its self-copy");
  gh_test_run_until_idle();
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_NEEDS_ATTENTION);
  assert_honest_status(item);
  g_autoptr(GPtrArray) recipients = gh_outbox_item_dup_recipients(item);
  g_assert_cmpuint(recipients->len, ==, 2);
  for (guint i = 0; i < recipients->len; i++) {
    GhOutboxRecipient *recipient = g_ptr_array_index(recipients, i);
    g_assert_cmpint(recipient->state, ==, g_str_equal(recipient->pubkey, hex[BOB])
                                            ? GH_OUTBOX_RECIPIENT_SENT
                                            : GH_OUTBOX_RECIPIENT_NO_INBOX);
  }

  /* Bob's wrap: once on each of his relays, fresh keys; the self-copy as
   * Alice on her own; nothing anywhere else. */
  g_autofree gchar *key_a = NULL, *key_b = NULL;
  g_autofree gchar *wrap_a = assert_one_publish(&net->a, from_a, hex[BOB], NULL, &key_a);
  g_autofree gchar *wrap_b = assert_one_publish(&net->b, from_b, hex[BOB], NULL, &key_b);
  g_assert_cmpstr(wrap_a, ==, wrap_b);
  g_assert_cmpstr(key_a, !=, key_b);
  g_autofree gchar *self_wrap = assert_one_publish(&net->c, from_c, hex[ALICE], hex[ALICE],
                                                   NULL);
  g_assert_cmpstr(self_wrap, !=, wrap_a);
  g_assert_cmpuint(event_frames(&net->e), ==, events[3]);
  g_assert_cmpuint(event_frames(&net->g), ==, events[4]);
  g_assert_cmpuint(net->d.attempts, ==, 0);
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    for (guint j = 0; j < relays[i]->stored->len; j++) {
      WireStored *stored = g_ptr_array_index(relays[i]->stored, j);
      if (nostr_event_get_kind(stored->event) == 1059)
        g_assert_cmpstr(p_tag_of(stored->event), !=, hex[CAROL]);
    }
  }
  /* Who else is in the room stays inside the encryption. */
  g_assert_false(any_frames_mention(&net->a, hex[ALICE]));
  g_assert_false(any_frames_mention(&net->b, hex[ALICE]));
  g_assert_false(any_frames_mention(&net->a, hex[CAROL]));
  g_assert_false(any_frames_mention(&net->b, hex[CAROL]));
  g_assert_false(any_frames_mention(&net->c, hex[BOB]));
  g_assert_false(any_frames_mention(&net->c, hex[CAROL]));

  /* Bob has it in the room of all three. */
  GhMessage *received = find_content(w.bob.model, text);
  g_assert_nonnull(received);
  g_autoptr(GPtrArray) members = g_ptr_array_new();
  g_ptr_array_add(members, hex[ALICE]);
  g_ptr_array_add(members, hex[BOB]);
  g_ptr_array_add(members, hex[CAROL]);
  g_ptr_array_sort(members, compare_hex);
  g_ptr_array_add(members, NULL);
  g_autofree gchar *room_id = g_strjoinv(",", (gchar **)members->pdata);
  g_assert_cmpstr(gh_message_get_room_id(received), ==, room_id);

  world_scan(&w, "PT-4 room");
  world_down(&w);
}

/* ---- H7 self-test ------------------------------------------------------------------ */

/* The scanner finds a planted canary in every encoding and place it claims
 * to cover, so that "clean" in the scenarios below means something. */
static void
test_h7_scanner(void)
{
  g_autofree gchar *needle = g_strdup_printf("ghcanary-%s-selftest", run_id);
  CanaryScan *scan = canary_scan_new();
  canary_scan_add(scan, "selftest", needle);
  g_assert_cmpuint(canary_scan_text(scan, "clean", "nothing to see here"), ==, 0);
  g_autofree gchar *raw = g_strdup_printf("<<%s>>", needle);
  g_assert_cmpuint(canary_scan_text(scan, "raw", raw), ==, 1);
  g_autofree gchar *hex_upper = g_strdup("");
  for (const gchar *c = needle; *c; c++) {
    gchar *next = g_strdup_printf("%s%02X", hex_upper, (guchar)*c);
    g_free(hex_upper);
    hex_upper = next;
  }
  g_assert_cmpuint(canary_scan_text(scan, "hex", hex_upper), ==, 1);
  for (guint pad = 0; pad < 3; pad++) {
    g_autofree gchar *padded = g_strdup_printf("%.*s%s tail", pad, "xyz", needle);
    g_autofree gchar *b64 = g_base64_encode((const guchar *)padded, strlen(padded));
    g_assert_cmpuint(canary_scan_text(scan, "base64", b64), >=, 1);
  }
  gsize length = strlen(needle);
  g_autofree guint8 *utf16 = g_malloc0(length * 2 + 4);
  for (gsize i = 0; i < length; i++)
    utf16[2 + 2 * i] = (guint8)needle[i];
  g_assert_cmpuint(canary_scan_bytes(scan, "utf-16le", utf16, length * 2 + 4), ==, 1);
  canary_scan_clear_hits(scan);

  /* A tree: a -wal file deep down, a hex copy, and a symlink. */
  g_autofree gchar *tmp = root_dir("tmp");
  g_autofree gchar *tree = g_build_filename(tmp, "h7-selftest", NULL);
  g_autofree gchar *deep = g_build_filename(tree, "a", "b", NULL);
  g_assert_cmpint(g_mkdir_with_parents(deep, 0700), ==, 0);
  g_autofree gchar *wal = g_build_filename(deep, "store.db-wal", NULL);
  g_assert_true(g_file_set_contents(wal, raw, -1, NULL));
  g_autofree gchar *other = g_build_filename(tree, "other", NULL);
  g_assert_true(g_file_set_contents(other, hex_upper, -1, NULL));
  g_autofree gchar *link = g_build_filename(tree, "link", NULL);
  g_assert_cmpint(symlink(other, link), ==, 0);
  guint files = 0;
  g_assert_cmpuint(canary_scan_tree(scan, tree, &files), ==, 3);
  g_assert_cmpuint(files, ==, 2);
  gh_test_remove_tree(tree);
  canary_scan_clear_hits(scan);

  /* GSettings (memory backend). */
  g_autoptr(GSettingsBackend) backend = g_memory_settings_backend_new();
  g_autoptr(GSettings) settings = g_settings_new_with_backend("org.nostr.Groundhog", backend);
  g_assert_cmpuint(canary_scan_settings(scan, "settings", settings), ==, 0);
  const gchar *urls[] = { needle, NULL };
  g_settings_set_strv(settings, "discovery-relays", urls);
  g_assert_cmpuint(canary_scan_settings(scan, "settings", settings), ==, 1);
  canary_scan_clear_hits(scan);

  /* Logs: a debug message and bytes written straight to stderr. */
  canary_log_capture_reset();
  g_debug("selftest %s", needle);
  canary_log_capture_sync();
  g_autofree gchar *logged = canary_log_capture_dup();
  g_assert_cmpuint(canary_scan_text(scan, "logs", logged), ==, 1);
  canary_scan_clear_hits(scan);
  fprintf(stderr, "selftest %s\n", hex_upper);
  canary_log_capture_sync();
  g_autofree gchar *written = canary_log_capture_dup();
  g_assert_cmpuint(canary_scan_text(scan, "stderr", written), >=, 1);
  canary_log_capture_reset();
  canary_scan_free(scan);
}

/* ---- PT-1 no receipts, typing or presence ------------------------------------------- */

/* Charter PT-1: receive 3 DMs, open the conversation, type 200 characters
 * without sending, switch conversations, read, mute; no EVENT on any
 * connection, only REQ/CLOSE (+AUTH on the account's own inbox). Pinning
 * and marking unread have no implementation yet (conversations.pinned_rank
 * has no writer; the model has no unread marker): extend the script when
 * they land. */
static void
run_pt1(World *w)
{
  Net *net = &w->net;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *alice_room = room_of(ALICE, BOB);
  g_autofree gchar *carol_room = room_of(CAROL, BOB);
  GhConversation *earlier = gh_conversation_store_lookup(w->bob.model, alice_room);
  guint unread_before = earlier ? gh_conversation_get_unread_count(earlier) : 0;
  GhOutboxItem *items[3];
  for (guint i = 0; i < G_N_ELEMENTS(items); i++)
    items[i] = send_and_deliver(&w->alice, &w->bob, world_canary(w, "pt1-dm", FALSE));
  /* A second conversation to switch to: a request from Carol. */
  const gchar *from_carol = world_canary(w, "pt1-carol", FALSE);
  g_autofree gchar *carol_wrap = craft_wrap(CAROL, BOB, 14, from_carol, NULL);
  wire_relay_inject(&net->a, carol_wrap);
  wait_received(&w->bob, from_carol);
  spin_until(inbox_idle, &w->bob, "Bob's inbox going idle");
  gh_test_run_until_idle();

  WireRelay *relays[N_WIRE];
  net_relays(net, relays);
  guint frames[N_WIRE], connections[N_WIRE];
  for (guint i = 0; i < N_WIRE; i++) {
    frames[i] = relays[i]->frames->len;
    connections[i] = relays[i]->connections->len;
  }
  guint signer_calls = signer.calls;

  GhConversation *with_alice = gh_conversation_store_lookup(w->bob.model, alice_room);
  GhConversation *with_carol = gh_conversation_store_lookup(w->bob.model, carol_room);
  g_assert_nonnull(with_alice);
  g_assert_nonnull(with_carol);
  g_assert_cmpuint(gh_conversation_get_unread_count(with_alice), ==, unread_before + 3);
  GhStoreConversations *store = app_conversations(&w->bob);

  /* Open the conversation (what the window glue does). */
  gh_notifier_set_visible_conversation(w->bob.notifier, with_alice);
  gh_conversation_mark_read(with_alice);
  /* Type 200 characters without sending; every keystroke's draft is saved. */
  const gchar *draft_canary = world_canary(w, "pt1-draft", FALSE);
  g_autoptr(GString) source = g_string_new(draft_canary);
  while (source->len < 200)
    g_string_append(source, " and more words");
  g_autoptr(GString) typed = g_string_new(NULL);
  for (guint i = 0; i < 200; i++) {
    g_string_append_c(typed, source->str[i]);
    g_assert_true(gh_store_conversations_set_draft(store, alice_room, typed->str, &error));
    g_assert_no_error(error);
    g_main_context_iteration(NULL, FALSE);
  }
  /* Switch conversations and back; read each. */
  gh_notifier_set_visible_conversation(w->bob.notifier, with_carol);
  gh_conversation_mark_read(with_carol);
  gh_notifier_set_visible_conversation(w->bob.notifier, with_alice);
  gh_conversation_mark_read(with_alice);
  /* Mute (the conversation's mute action). */
  g_assert_true(gh_store_conversations_set_muted_until(store, alice_room,
                                                       GH_STORE_CONVERSATIONS_MUTED_ALWAYS,
                                                       &error));
  g_assert_no_error(error);
  gh_notifier_withdraw_conversation(w->bob.notifier, alice_room);
  gh_notifier_set_visible_conversation(w->bob.notifier, NULL);
  gh_test_run_until_idle();

  /* All of it stayed local: in Bob's encrypted store, nothing signed. */
  g_autofree gchar *draft = NULL;
  g_assert_true(gh_store_conversations_get_draft(store, alice_room, &draft, &error));
  g_assert_cmpstr(draft, ==, typed->str);
  GhStoreNotifyState state = { 0 };
  g_assert_true(gh_store_conversations_get_notify_state(store, alice_room, &state, &error));
  g_assert_cmpint(state.muted_until, ==, GH_STORE_CONVERSATIONS_MUTED_ALWAYS);
  g_assert_cmpuint(gh_conversation_get_unread_count(with_alice), ==, 0);
  g_assert_cmpuint(gh_conversation_get_unread_count(with_carol), ==, 0);
  g_assert_cmpuint(signer.calls, ==, signer_calls);

  /* The barrier: Bob replies, and it is accepted and received. */
  const gchar *reply = world_canary(w, "pt1-reply", FALSE);
  g_autoptr(GhOutboxItem) barrier = send_and_deliver(&w->bob, &w->alice, reply);
  g_autoptr(GPtrArray) targets = gh_outbox_item_dup_targets(barrier);
  g_autoptr(GPtrArray) barrier_ids = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < targets->len; i++) {
    GhOutboxTarget *target = g_ptr_array_index(targets, i);
    g_ptr_array_add(barrier_ids, g_strdup(target->event_id));
  }

  for (guint r = 0; r < N_WIRE; r++) {
    gboolean bob_inbox = r < 2, discovery = r >= 3;
    g_autoptr(GHashTable) conns = conns_since(relays[r], frames[r]);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, conns);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Conn *conn = value;
      if (conn->serial <= connections[r]) {
        /* Connections open before the script: REQ/CLOSE, and AUTH only on
         * the account's own inbox as that account. */
        g_assert_cmpuint(conn->events, ==, 0);
        for (guint k = 0; k < conn->auth_keys->len; k++) {
          const gchar *key = g_ptr_array_index(conn->auth_keys, k);
          g_assert_false(discovery);
          g_assert_cmpstr(key, ==, bob_inbox ? hex[BOB] : hex[ALICE]);
        }
        continue;
      }
      /* Connections the barrier opened: its wraps, and on E the directory
       * lookup of Alice. Nothing else. */
      for (guint k = 0; k < conn->event_ids->len; k++)
        g_assert_true(str_in(barrier_ids, g_ptr_array_index(conn->event_ids, k)));
      if (conn->reqs > 0) {
        g_assert_true(discovery);
        DiscoveryCheck check = { hex[ALICE], 0 };
        for (guint k = 0; k < conn->reqs_text->len; k++)
          req_shapes(g_ptr_array_index(conn->reqs_text, k), check_discovery_shape, &check);
        g_assert_cmpuint(check.lookups, ==, conn->reqs);
      }
      for (guint k = 0; k < conn->auth_keys->len; k++) {
        const gchar *key = g_ptr_array_index(conn->auth_keys, k);
        if (bob_inbox)
          g_assert_cmpstr(key, ==, hex[BOB]); /* his self-copy */
        else
          g_assert_false(is_account(key)); /* Alice's inbox, discovery: ephemeral */
      }
    }
  }
  /* Reading never reached the sender: her messages are still just "Sent". */
  for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
    g_assert_cmpint(gh_outbox_item_get_status(items[i]), ==, GH_MESSAGE_STATUS_SENT);
    assert_honest_status(items[i]);
    g_object_unref(items[i]);
  }
}

static void
test_pt1_no_receipts(void)
{
  World w;
  world_up(&w);
  run_pt1(&w);
  world_scan(&w, "PT-1");
  world_down(&w);
}

/* ---- PT-2 no remote fetch -------------------------------------------------------------- */

/* Charter PT-2 on the receive path: a DM with an https image URL, a link, a
 * plain http URL and a stranger's nprofile, and a kind-15 attachment, with
 * default settings. Nothing is fetched (M: zero connection attempts), the
 * stranger is never looked up (no REQ names him, F is never contacted) and
 * no profile at all is asked for. The kind-15 message is admitted (G21) and
 * its file is not fetched. Rendering those messages is the
 * conversation view's half (G12, test-groundhog-conversation-view). */
static void
test_pt2_no_remote_fetch(void)
{
  World w;
  world_up(&w);
  Net *net = &w.net;
  static const gchar *const fetch_keys[] = { "load-remote-images", "link-previews",
                                             "load-profile-pictures" };
  for (guint i = 0; i < G_N_ELEMENTS(fetch_keys); i++)
    g_assert_false(g_settings_get_boolean(w.bob.settings, fetch_keys[i]));

  char *stranger_secret = nostr_key_generate_private();
  char *stranger = nostr_key_get_public(stranger_secret);
  free(stranger_secret);
  g_autofree gchar *hint = tripwire_url(&net->f, "wss", "relay");
  char *relays[] = { hint };
  NostrProfilePointer pointer = { stranger, relays, 1 };
  char *nprofile = NULL;
  g_assert_cmpint(nostr_nip19_encode_nprofile(&pointer, &nprofile), ==, 0);
  g_autofree gchar *image = tripwire_url(&net->m, "https", "pt2-image.png");
  g_autofree gchar *link = tripwire_url(&net->m, "https", "pt2-page.html");
  g_autofree gchar *plain = tripwire_url(&net->m, "http", "pt2-plain");
  g_autofree gchar *body = g_strdup_printf("%s look %s and %s (also %s), ask nostr:%s",
                                           world_canary(&w, "pt2", FALSE), image, link, plain,
                                           nprofile);
  g_autoptr(GhOutboxItem) item = send_and_deliver(&w.alice, &w.bob, body);
  GhMessage *received = find_content(w.bob.model, body);
  g_assert_nonnull(received);
  g_assert_nonnull(strstr(gh_message_get_content(received), image));

  /* A kind-15 file message from Alice, as another client sends it. */
  g_autofree gchar *file_url = tripwire_url(&net->m, "https", "pt2-file.bin");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("file-type", "image/jpeg", NULL));
  nostr_tags_append(tags, nostr_tag_new("encryption-algorithm", "aes-gcm", NULL));
  nostr_tags_append(tags, nostr_tag_new("decryption-key", "00112233445566778899aabbccddeeff"
                                        "00112233445566778899aabbccddeeff", NULL));
  nostr_tags_append(tags, nostr_tag_new("decryption-nonce", "00112233445566778899aabb", NULL));
  nostr_tags_append(tags, nostr_tag_new("x", "7d865e959b2466918c9863afca942d0fb89d7c9ac0c99b"
                                        "afc3749504ded97730", NULL));
  GhDmInboxCounters before, after;
  gh_dm_inbox_get_counters(w.bob.inbox, &before);
  g_autofree gchar *file_wrap = craft_wrap(ALICE, BOB, 15, file_url, tags);
  wire_relay_inject(&net->a, file_wrap);
  gh_dm_inbox_get_counters(w.bob.inbox, &after);
  while (after.received == before.received || after.pending > 0) {
    g_main_context_iteration(NULL, TRUE);
    gh_dm_inbox_get_counters(w.bob.inbox, &after);
  }
  spin_until(inbox_idle, &w.bob, "Bob's inbox going idle");
  gh_dm_inbox_get_counters(w.bob.inbox, &after);
  /* G21: the inbox admits the file message (charter §6 receive step 1), and
   * admitting it fetches nothing: the file is only metadata until the user
   * chooses Download (PD-2, AT-7; the download itself is AT-5 below). */
  g_assert_cmpuint(after.admitted, ==, before.admitted + 1);
  g_assert_cmpuint(after.rejected, ==, before.rejected);
  GhMessage *file_message = find_content(w.bob.model, file_url);
  g_assert_nonnull(file_message);
  g_assert_cmpint(gh_message_get_kind(file_message), ==, 15);
  g_autoptr(GhNip17File) file = gh_message_dup_file(file_message);
  g_assert_nonnull(file);
  g_assert_cmpstr(file->url, ==, file_url);

  /* Nothing fetched, nobody looked up. */
  g_assert_cmpuint(net->m.attempts, ==, 0);
  g_assert_cmpuint(net->f.attempts, ==, 0);
  WireRelay *wire_relays[N_WIRE];
  net_relays(net, wire_relays);
  for (guint i = 0; i < N_WIRE; i++) {
    g_assert_false(client_frames_mention(wire_relays[i], 0, stranger));
    assert_no_profile_reqs(wire_relays[i]);
  }
  free(nprofile);
  free(stranger);
  world_scan(&w, "PT-2");
  world_down(&w);
}

/* ---- PT-8 requests trigger no lookup ---------------------------------------------------- */

/* Charter PT-8, receive side: a DM from someone Bob never accepted is a
 * request; nothing about its sender is asked of any relay (no kind 0, no
 * directory lookup), and its notification is hidden even at "preview".
 * (Accepting it and the kind-0 fetch that may follow are covered on a fake
 * clock by test-groundhog-contact-directory.) */
static void
test_pt8_request_no_lookup(void)
{
  World w;
  world_up(&w);
  Net *net = &w.net;
  g_settings_set_string(w.bob.settings, "notification-privacy", "preview");
  notify_settle(&w.bob);
  const gchar *canary = world_canary(&w, "pt8", FALSE);
  g_autofree gchar *wrap = craft_wrap(CAROL, BOB, 14, canary, NULL);
  wire_relay_inject(&net->a, wrap);
  wait_received(&w.bob, canary);
  spin_until(inbox_idle, &w.bob, "Bob's inbox going idle");
  g_autofree gchar *room = room_of(CAROL, BOB);
  GhConversation *request = gh_conversation_store_lookup(w.bob.model, room);
  g_assert_nonnull(request);
  g_assert_true(gh_conversation_get_is_request(request));
  notify_settle(&w.bob);

  GVariant *shown_request = shown(&w.bob, GH_NOTIFIER_ID_MESSAGES);
  g_assert_nonnull(shown_request);
  g_assert_cmpstr(field(shown_request, "title"), ==, "New message");
  g_assert_cmpstr(field(shown_request, "body"), ==, "1 new message");
  GPtrArray *calls = fake_gtk_notifications_get_calls(notifications);
  for (guint i = 0; i < calls->len; i++) {
    FakeNotificationCall *call = g_ptr_array_index(calls, i);
    g_assert_false(g_str_has_prefix(call->id, GH_NOTIFIER_ID_CONVERSATION_PREFIX));
  }
  const gchar *secrets[] = { canary, gh_conversation_get_title(request), NULL };
  assert_payloads_hide(&w.bob, CAROL, secrets);

  WireRelay *relays[N_WIRE];
  net_relays(net, relays);
  for (guint i = 0; i < N_WIRE; i++) {
    g_assert_false(client_frames_mention(relays[i], 0, hex[CAROL]));
    assert_no_profile_reqs(relays[i]);
  }
  g_assert_cmpuint(discovery_lookups(&net->e, hex[CAROL]), ==, 0);
  g_assert_cmpuint(discovery_lookups(&net->g, hex[CAROL]), ==, 0);
  world_scan(&w, "PT-8");
  world_down(&w);
}

/* ---- NO-1 content levels ---------------------------------------------------------------- */

/* The newest conv-<n> notification app shows, or NULL. */
static GVariant *
shown_conversation(App *app)
{
  GPtrArray *calls = fake_gtk_notifications_get_calls(notifications);
  for (guint i = calls->len; i-- > 0;) {
    FakeNotificationCall *call = g_ptr_array_index(calls, i);
    if (g_str_equal(call->app_id, app_id(app)) &&
        g_str_has_prefix(call->id, GH_NOTIFIER_ID_CONVERSATION_PREFIX))
      return call->added ? call->notification : NULL;
  }
  return NULL;
}

/* Rumor times have whole-second resolution and the read marker breaks a
 * same-second tie by rumor id (nostrc-qp24.75): a message sent in the second
 * of the last-read one may count as read and never be notified. Until that
 * is fixed, NO-1 starts each send after a read in a new second. */
static gboolean
second_changed(gpointer data)
{
  return g_get_real_time() / G_USEC_PER_SEC != *(gint64 *)data;
}

static void
wait_next_second(void)
{
  gint64 second = g_get_real_time() / G_USEC_PER_SEC;
  spin_until(second_changed, &second, "the next wall-clock second");
}

static void
read_and_withdraw(App *app, GhConversation *conversation)
{
  gh_conversation_mark_read(conversation);
  notify_next_window(app);
}

static void
test_no1_levels(void)
{
  World w;
  world_up(&w);
  App *bob = &w.bob;
  g_autofree gchar *level = g_settings_get_string(bob->settings, "notification-privacy");
  g_assert_cmpstr(level, ==, "hidden"); /* the default */
  g_autofree gchar *room = room_of(ALICE, BOB);

  /* First contact is a request (always hidden); Bob accepts it, so what
   * follows is shown at the level he chose. */
  g_autoptr(GhOutboxItem) first = send_and_deliver(&w.alice, bob,
                                                   world_canary(&w, "no1-first", FALSE));
  GhConversation *conversation = gh_conversation_store_lookup(bob->model, room);
  g_assert_nonnull(conversation);
  g_assert_true(gh_conversation_get_is_request(conversation));
  notify_settle(bob);
  gh_conversation_accept(conversation);
  g_assert_false(gh_conversation_get_is_request(conversation));
  read_and_withdraw(bob, conversation);
  const gchar *title = gh_conversation_get_title(conversation);

  /* hidden: "New message", the app-wide id, no name, npub, title or text. */
  const gchar *hidden_text = world_canary(&w, "no1-hidden", FALSE);
  guint before = count_added(bob);
  wait_next_second();
  g_autoptr(GhOutboxItem) hidden = send_and_deliver(&w.alice, bob, hidden_text);
  notify_settle(bob);
  g_assert_cmpuint(count_added(bob), ==, before + 1);
  GVariant *n = shown(bob, GH_NOTIFIER_ID_MESSAGES);
  g_assert_nonnull(n);
  g_assert_cmpstr(field(n, "title"), ==, "New message");
  g_assert_cmpstr(field(n, "body"), ==, "1 new message");
  g_assert_null(shown_conversation(bob));
  const gchar *hidden_secrets[] = { hidden_text, title, NULL };
  assert_payloads_hide(bob, ALICE, hidden_secrets);
  read_and_withdraw(bob, conversation);

  /* sender: the conversation's local name as the title, a generic body. */
  g_settings_set_string(bob->settings, "notification-privacy", "sender");
  notify_next_window(bob);
  const gchar *sender_text = world_canary(&w, "no1-sender", FALSE);
  wait_next_second();
  g_autoptr(GhOutboxItem) sender = send_and_deliver(&w.alice, bob, sender_text);
  notify_settle(bob);
  n = shown_conversation(bob);
  g_assert_nonnull(n);
  g_assert_cmpstr(field(n, "title"), ==, title);
  g_assert_cmpstr(field(n, "body"), ==, "New message");
  g_autofree gchar *payloads = payloads_of(bob);
  g_assert_null(strstr(payloads, sender_text));
  read_and_withdraw(bob, conversation);

  /* preview: the text, cut to at most 120 graphemes. */
  g_settings_set_string(bob->settings, "notification-privacy", "preview");
  notify_next_window(bob);
  const gchar *preview_text = world_canary(&w, "no1-preview", TRUE);
  g_autoptr(GString) long_text = g_string_new(preview_text);
  while (long_text->len < 300)
    g_string_append(long_text, " lorem ipsum");
  wait_next_second();
  g_autoptr(GhOutboxItem) preview = send_and_deliver(&w.alice, bob, long_text->str);
  notify_settle(bob);
  n = shown_conversation(bob);
  g_assert_nonnull(n);
  g_assert_cmpstr(field(n, "title"), ==, title);
  const gchar *body = field(n, "body");
  g_assert_nonnull(body);
  g_assert_cmpint(g_utf8_strlen(body, -1), <=, GH_NOTIFIER_PREVIEW_MAX);
  g_assert_true(g_str_has_prefix(body, preview_text));
  g_assert_true(g_str_has_suffix(body, "…"));
  world_scan(&w, "NO-1");
  world_down(&w);
}

/* ---- PT-10 logs ----------------------------------------------------------------------- */

/* Charter PT-10: with G_MESSAGES_DEBUG=all through PT-1 and a send carrying a
 * fake nsec and a bunker URI (what a user might paste), no canary, nsec or
 * bunker URI reaches any log. */
static void
test_pt10_logs(void)
{
  World w;
  world_up(&w);
  g_assert_cmpstr(g_getenv("G_MESSAGES_DEBUG"), ==, "all");
  char *fake_secret = nostr_key_generate_private();
  guint8 sk[32];
  g_assert_true(nostr_hex2bin(sk, fake_secret, sizeof sk));
  char *fake_nsec = NULL;
  g_assert_cmpint(nostr_nip19_encode_nsec(sk, &fake_nsec), ==, 0);
  char *fake_pub = nostr_key_get_public(fake_secret);
  g_autofree gchar *bunker = g_strdup_printf("bunker://%s?relay=wss%%3A%%2F%%2Fbunker.invalid"
                                             "&secret=%s", fake_pub, run_id);
  world_secret(&w, "fake nsec", fake_nsec, FALSE);
  world_secret(&w, "fake bunker URI", bunker, FALSE);
  g_autofree gchar *text = g_strdup_printf("%s keep these safe: %s %s",
                                           world_canary(&w, "pt10", FALSE), fake_nsec, bunker);
  g_autoptr(GhOutboxItem) item = send_and_deliver(&w.alice, &w.bob, text);
  run_pt1(&w);
  /* The capture is not empty: the real relay pipeline logged at debug level. */
  canary_log_capture_sync();
  g_autofree gchar *logs = canary_log_capture_dup();
  g_assert_nonnull(strstr(logs, "Fired subscription"));
  free(fake_secret);
  free(fake_nsec);
  free(fake_pub);
  world_scan(&w, "PT-10");
  world_down(&w);
}

/* ---- AT-5 no plaintext on disk ------------------------------------------------------------ */

static void
collect_files(const gchar *path, GPtrArray *out)
{
  GStatBuf st;
  if (g_lstat(path, &st) != 0)
    return;
  if (!S_ISDIR(st.st_mode)) {
    g_ptr_array_add(out, g_strdup(path));
    return;
  }
  GDir *dir = g_dir_open(path, 0, NULL);
  g_assert_nonnull(dir);
  const gchar *name;
  while ((name = g_dir_read_name(dir))) {
    g_autofree gchar *child = g_build_filename(path, name, NULL);
    collect_files(child, out);
  }
  g_dir_close(dir);
}

/* AT-5's file layout: XDG_CACHE_HOME, XDG_RUNTIME_DIR, XDG_STATE_HOME and
 * XDG_CONFIG_HOME are empty, TMPDIR holds only the test bus's own directory
 * and XDG_DATA_HOME the two encrypted stores and nothing else. */
static void
assert_at5_layout(World *w)
{
  static const gchar *const empty[] = { "cache", "runtime", "state", "config" };
  for (guint i = 0; i < G_N_ELEMENTS(empty); i++) {
    g_autofree gchar *dir = root_dir(empty[i]);
    g_assert_cmpuint(gh_test_count_files(dir), ==, 0);
  }
  g_autofree gchar *tmp = root_dir("tmp");
  g_autoptr(GPtrArray) tmp_files = g_ptr_array_new_with_free_func(g_free);
  collect_files(tmp, tmp_files);
  const gchar *bus_dir = nostrc_test_bus_get_dir(test_bus.bus);
  g_autofree gchar *bus_prefix = g_strconcat(bus_dir, G_DIR_SEPARATOR_S, NULL);
  for (guint i = 0; i < tmp_files->len; i++) {
    const gchar *path = g_ptr_array_index(tmp_files, i);
    if (!g_str_has_prefix(path, bus_prefix))
      g_error("AT-5: unexpected file in TMPDIR: %s", path);
  }
  g_autofree gchar *data = root_dir("data");
  g_autoptr(GPtrArray) data_files = g_ptr_array_new_with_free_func(g_free);
  collect_files(data, data_files);
  App *apps[] = { &w->alice, &w->bob };
  for (guint i = 0; i < data_files->len; i++) {
    const gchar *path = g_ptr_array_index(data_files, i);
    gboolean store_file = FALSE;
    for (guint a = 0; a < G_N_ELEMENTS(apps) && !store_file; a++) {
      static const gchar *const suffixes[] = { "", "-wal", "-shm" };
      for (guint s = 0; s < G_N_ELEMENTS(suffixes) && !store_file; s++) {
        g_autofree gchar *expected = store_path(apps[a], suffixes[s]);
        store_file = g_str_equal(path, expected);
      }
    }
    if (!store_file)
      g_error("AT-5: a file outside the encrypted stores: %s", path);
  }
}

/* Charter AT-5 for text messages: after sending, receiving and notifying,
 * XDG_CACHE_HOME and XDG_RUNTIME_DIR are still empty, TMPDIR holds only the
 * test bus's own directory, nothing is in XDG_STATE_HOME or XDG_CONFIG_HOME,
 * XDG_DATA_HOME holds the two encrypted stores and nothing else, and H7 is
 * clean everywhere. */
static void
test_at5_no_plaintext_on_disk(void)
{
  World w;
  world_up(&w);
  g_autoptr(GhOutboxItem) out = send_and_deliver(&w.alice, &w.bob,
                                                 world_canary(&w, "at5-out", FALSE));
  g_autoptr(GhOutboxItem) back = send_and_deliver(&w.bob, &w.alice,
                                                  world_canary(&w, "at5-back", FALSE));
  notify_settle(&w.alice);
  notify_settle(&w.bob);
  g_assert_cmpuint(count_added(&w.bob), >, 0);
  assert_at5_layout(&w);
  world_scan(&w, "AT-5");
  world_down(&w);
}

#if GROUNDHOG_TEST_ATTACHMENTS
/* A JPEG carrying canary in its image data (which survives stripping) and
 * gps in an APP1 EXIF segment (which must not). */
static GBytes *
canary_jpeg(const gchar *canary, const gchar *gps)
{
  GByteArray *out = g_byte_array_new();
  static const guint8 head[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x07, 'J', 'F', 'I', 'F', 0x00 };
  g_byte_array_append(out, head, sizeof head);
  gsize exif_size = 6 + strlen(gps);
  guint8 app1[10] = { 0xFF, 0xE1, (guint8)((exif_size + 2) >> 8), (guint8)(exif_size + 2),
                      'E', 'x', 'i', 'f', 0, 0 };
  g_byte_array_append(out, app1, sizeof app1);
  g_byte_array_append(out, (const guint8 *)gps, (guint)strlen(gps));
  static const guint8 sof[] = { 0xFF, 0xC0, 0x00, 0x0B, 8, 0x01, 0x00, 0x01, 0x00, 1, 1, 0x11,
                                0 };
  g_byte_array_append(out, sof, sizeof sof);
  static const guint8 sos[] = { 0xFF, 0xDA, 0x00, 0x08, 1, 1, 0, 0, 63, 0 };
  g_byte_array_append(out, sos, sizeof sos);
  for (guint i = 0; i < 64; i++) {
    g_byte_array_append(out, (const guint8 *)canary, (guint)strlen(canary));
    guint8 filler[1024];
    for (guint j = 0; j < sizeof filler; j++)
      filler[j] = (guint8)((i * 31 + j * 7) % 0xFE);
    g_byte_array_append(out, filler, sizeof filler);
  }
  static const guint8 eoi[] = { 0xFF, 0xD9 };
  g_byte_array_append(out, eoi, sizeof eoi);
  return g_byte_array_free_to_bytes(out);
}

typedef struct {
  gboolean done;
  GhNip17File *file;
  GBytes *bytes;
  GError *error;
} Transfer;

static gboolean
transfer_done(gpointer data)
{
  return ((Transfer *)data)->done;
}

static void
on_attachment_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Transfer *t = data;
  t->file = gh_attachment_upload_finish(result, &t->error);
  t->done = TRUE;
}

static void
on_attachment_downloaded(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Transfer *t = data;
  t->bytes = gh_attachment_download_finish(result, NULL, &t->error);
  t->done = TRUE;
}

/* Charter AT-5 with attachments, and AT-1 end to end (G21): Alice sends a
 * JPEG with EXIF GPS through a local Blossom server (loopback) and the real
 * outbox, relays and inbox; Bob receives the kind-15 message without any
 * download (AT-7), then downloads it on his explicit request, verified,
 * decrypted and kept only in his encrypted store; the preview guard accepts
 * it. Afterwards the transient directories are empty, the data directory
 * holds the two encrypted stores only, and H7 finds neither the file's
 * plaintext nor its GPS canary in any file, relay frame, setting or log. */
static void
test_at5_attachments(void)
{
  World w;
  world_up(&w);
  BlossomFixture *blossom = blossom_fixture_new();
  const gchar *servers[] = { blossom_fixture_url(blossom), NULL };
  g_settings_set_strv(w.alice.settings, "blossom-servers", servers);
  App *apps[] = { &w.alice, &w.bob };
  for (guint i = 0; i < G_N_ELEMENTS(apps); i++)
    g_settings_set_string(apps[i]->settings, "network-mode", "none"); /* loopback, direct */
  GhNetHttp *alice_http = gh_net_http_new(w.alice.settings);
  GhNetHttp *bob_http = gh_net_http_new(w.bob.settings);
  GhBlossomClient *alice_media = gh_blossom_client_new(w.alice.settings, alice_http);
  GhBlossomClient *bob_media = gh_blossom_client_new(w.bob.settings, bob_http);

  const gchar *canary = world_canary(&w, "at5-file", FALSE);
  const gchar *gps = world_canary(&w, "at5-gps", FALSE);
  g_autoptr(GBytes) jpeg = canary_jpeg(canary, gps);
  g_autoptr(GhAttachmentPrepared) stripped = gh_attachment_prepare(jpeg, NULL, 1 << 20, NULL);
  g_assert_nonnull(stripped);

  /* Send: strip, encrypt, upload; then the outbox's kind-15 message. */
  Transfer t = { 0 };
  gh_attachment_upload_async(alice_media, jpeg, NULL, NULL, on_attachment_uploaded, &t);
  spin_until(transfer_done, &t, "the attachment upload");
  g_assert_no_error(t.error);
  g_autoptr(GhNip17File) sent = g_steal_pointer(&t.file);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhOutboxItem) item = gh_outbox_send_file(app_outbox(&w.alice), hex[BOB], sent, &error);
  g_assert_no_error(error);
  spin_until(item_settled, item, "the outbox settling the file message");
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);

  /* Receive: a kind-15 message, and not one request for its file. */
  wait_received(&w.bob, sent->url);
  spin_until(inbox_idle, &w.bob, "Bob's inbox going idle");
  spin_until(inbox_idle, &w.alice, "Alice's inbox taking her self-copy");
  notify_settle(&w.bob);
  GhMessage *message = find_content(w.bob.model, sent->url);
  g_assert_cmpint(gh_message_get_kind(message), ==, 15);
  g_autoptr(GhNip17File) received = gh_message_dup_file(message);
  g_assert_nonnull(received);
  g_assert_cmpstr(received->x, ==, sent->x);
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 0);

  /* Download (Bob's explicit request) into his encrypted store, and the
   * preview's decode guard. */
  GhStore *bob_store = gh_account_store_get_store(w.bob.store);
  g_assert_nonnull(bob_store);
  memset(&t, 0, sizeof t);
  gh_attachment_download_async(bob_media, bob_store, received, NULL, on_attachment_downloaded,
                               &t);
  spin_until(transfer_done, &t, "the attachment download");
  g_assert_no_error(t.error);
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 1);
  g_assert_true(g_bytes_equal(t.bytes, stripped->plaintext));
  GhMediaFormat format = GH_MEDIA_FORMAT_OTHER;
  g_assert_true(gh_attachment_check_preview(t.bytes, &format, NULL, NULL, &error));
  g_assert_cmpint(format, ==, GH_MEDIA_FORMAT_JPEG);
  g_clear_pointer(&t.bytes, g_bytes_unref);
  g_assert_true(gh_store_checkpoint(bob_store, NULL));

  g_clear_object(&alice_media);
  g_clear_object(&bob_media);
  g_clear_object(&alice_http);
  g_clear_object(&bob_http);
  blossom_fixture_free(blossom);
  assert_at5_layout(&w);
  world_scan(&w, "AT-5 attachments");
  for (guint i = 0; i < G_N_ELEMENTS(apps); i++) {
    g_settings_reset(apps[i]->settings, "network-mode");
    g_settings_reset(apps[i]->settings, "blossom-servers");
  }
  world_down(&w);
}
#else
static void
test_at5_attachments(void)
{
  g_test_skip("AT-5 with attachments needs Groundhog's attachments core (groundhog-media: "
              "libsoup, the GhStore and OpenSSL), which this build does not have");
}
#endif

/* ---- main ------------------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
  /* Private XDG directories and TMPDIR before anything asks GLib for them
   * (GLib caches them), all below one root: what H7 scans. */
  const gchar *base = g_getenv("TMPDIR");
  gchar *template = g_build_filename(base && *base ? base : "/tmp", "groundhog-privacy-XXXXXX",
                                     NULL);
  if (!mkdtemp(template))
    g_error("cannot create the test root: %s", g_strerror(errno));
  /* Canonical (macOS: /private/var/...), as the store names its files. */
  char *real = realpath(template, NULL);
  g_assert_nonnull(real);
  root = g_strdup(real);
  free(real);
  g_free(template);
  for (guint i = 0; i < G_N_ELEMENTS(xdg_names); i++) {
    g_autofree gchar *dir = root_dir(xdg_names[i]);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(xdg_vars[i], dir, TRUE);
  }
  g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
  g_setenv("GNOTIFICATION_BACKEND", "gtk", TRUE); /* what a GNOME desktop receives */
  g_setenv("GIO_USE_VFS", "local", TRUE);
  g_setenv("GIO_USE_NETWORK_MONITOR", "base", TRUE); /* online: the outbox runs */
  g_test_init(&argc, &argv, NULL);
  canary_log_capture_install();

  g_autofree gchar *uuid = g_uuid_string_random();
  run_id = g_strndup(uuid, 8);
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    npub[key] = gh_test_npub(key);
    hex[key] = gh_test_pub(key);
  }
  int status = 77;
  if (!nostrc_test_bus_available()) {
    g_printerr("Groundhog privacy harness skipped: no dbus-daemon\n");
    goto out;
  }
  gh_test_bus_up(&test_bus);
  gh_test_signer_up(&test_bus, &signer);
  notifications = fake_gtk_notifications_new(nostrc_test_bus_connect(test_bus.bus));
  nostrc_test_bus_add_func("/groundhog/privacy/h7-scanner", test_h7_scanner);
  nostrc_test_bus_add_func("/groundhog/privacy/pt4-relay-minimization",
                           test_pt4_relay_minimization);
  nostrc_test_bus_add_func("/groundhog/privacy/pt4-room", test_pt4_room);
  nostrc_test_bus_add_func("/groundhog/privacy/pt1-no-receipts", test_pt1_no_receipts);
  nostrc_test_bus_add_func("/groundhog/privacy/pt2-no-remote-fetch", test_pt2_no_remote_fetch);
  nostrc_test_bus_add_func("/groundhog/privacy/pt8-request-no-lookup",
                           test_pt8_request_no_lookup);
  nostrc_test_bus_add_func("/groundhog/privacy/no1-levels", test_no1_levels);
  nostrc_test_bus_add_func("/groundhog/privacy/pt10-logs", test_pt10_logs);
  nostrc_test_bus_add_func("/groundhog/privacy/at5-text-no-plaintext-on-disk",
                           test_at5_no_plaintext_on_disk);
  nostrc_test_bus_add_func("/groundhog/privacy/at5-attachments", test_at5_attachments);
  status = g_test_run();
  fake_gtk_notifications_free(notifications);
  gh_test_signer_down(&test_bus, &signer);
  gh_test_bus_down(&test_bus);
out:
  canary_log_capture_uninstall();
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(npub[key]);
    g_free(hex[key]);
  }
  g_free(run_id);
  gh_test_remove_tree(root);
  g_free(root);
  return status;
}
