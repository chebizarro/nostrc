/* EX-1 for test_expiry.c: a disappearing message's rumor carries its exact
 * expiration, each seal and gift wrap a later one of its own (PT-7), through
 * real seals (the mock org.nostr.Signer on a private bus) and through the
 * durable outbox; and the outbox lets go of a message the expiry purge
 * deleted while it was still being published. Waits iterate the main
 * context; their deadlines are failure bounds only. */
#include "expiry-send.h"

#include "gh-expiry.h"
#include "gh-nip17-inbox.h"
#include "gh-outbox.h"
#include "gh-test-signer.h"
#include "nostr-tag.h"
#include "nostr/nip17/nip17.h"
#include "nostr/nip59/nip59.h"

#include <glib/gstdio.h>
#include <sodium.h>

#define T0          G_GINT64_CONSTANT(1900000000) /* 46 min 40 s past an hour */
#define HOUR        G_GINT64_CONSTANT(3600)
#define DAY         GH_EXPIRY_TIMER_DAY
#define KEY_ALICE   1
#define KEY_BOB     2
#define DISC        "wss://discovery.test.invalid"
#define ALICE_INBOX "wss://alice-inbox.test.invalid"
#define BOB_INBOX   "wss://bob-inbox.test.invalid"
#define STORE_ID    "0b7c2f52-3d7e-4a55-8f0e-6a1f3c9d2e41"

static GhTestBus bus;
static gboolean bus_up;
static gchar *hex_alice, *hex_bob, *npub_alice, *npub_bob;

static gint64
ceil_hour(gint64 t)
{
  return (t + HOUR - 1) / HOUR * HOUR;
}

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static void
keys_init(void)
{
  if (hex_alice)
    return;
  hex_alice = gh_test_pub(KEY_ALICE);
  hex_bob = gh_test_pub(KEY_BOB);
  npub_alice = gh_test_npub(KEY_ALICE);
  npub_bob = gh_test_npub(KEY_BOB);
}

void
test_expiry_send_cleanup(void)
{
  if (bus_up)
    gh_test_bus_down(&bus);
  bus_up = FALSE;
  g_clear_pointer(&hex_alice, g_free);
  g_clear_pointer(&hex_bob, g_free);
  g_clear_pointer(&npub_alice, g_free);
  g_clear_pointer(&npub_bob, g_free);
}

/* The number of tags named key and the value of the last one (0: none). */
static guint
tag_count(NostrEvent *event, const gchar *key, gint64 *out_value)
{
  NostrTags *tags = nostr_event_get_tags(event);
  guint n = 0;
  *out_value = 0;
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), key) != 0)
      continue;
    n++;
    *out_value = g_ascii_strtoll(nostr_tag_get(tag, 1), NULL, 10);
  }
  return n;
}

static gchar *
event_id_of(const gchar *json)
{
  NostrEvent *event = nostr_event_new();
  gchar id[65] = { 0 };
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_compute_id(event, id), ==, NOSTR_EVENT_VALIDATION_OK);
  nostr_event_free(event);
  return g_strdup(id);
}

/* A gift wrap for key's owner: its expiration, its seal's (the seal's only
 * tag) and the rumor's exact one inside (0: none). */
static void
assert_layers(const gchar *wrap_json, guint key, gint64 wrap_expiration,
              gint64 seal_expiration, const gchar *rumor_id, gint64 rumor_expiration)
{
  gint64 value = 0;
  NostrEvent *wrap = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(wrap, wrap_json, NULL), ==, 1);
  g_assert_true(nostr_nip59_validate_gift_wrap(wrap));
  g_assert_cmpstr(nostr_event_get_pubkey(wrap), !=, hex_alice);
  g_assert_cmpuint(tag_count(wrap, "expiration", &value), ==, wrap_expiration ? 1 : 0);
  g_assert_cmpint(value, ==, wrap_expiration);
  g_assert_cmpuint(tag_count(wrap, "p", &value), ==, 1);

  NostrEvent *seal = nostr_nip17_unwrap_gift_wrap(wrap, gh_test_secret[key]);
  g_assert_nonnull(seal);
  g_assert_cmpint(nostr_event_get_kind(seal), ==, 13);
  g_assert_cmpstr(nostr_event_get_pubkey(seal), ==, hex_alice);
  NostrTags *seal_tags = nostr_event_get_tags(seal);
  g_assert_cmpuint(seal_tags ? nostr_tags_size(seal_tags) : 0, ==, seal_expiration ? 1 : 0);
  g_assert_cmpuint(tag_count(seal, "expiration", &value), ==, seal_expiration ? 1 : 0);
  g_assert_cmpint(value, ==, seal_expiration);
  /* Its created_at is randomized into the past, never the send time. */
  g_assert_cmpint(nostr_event_get_created_at(seal), <, g_get_real_time() / G_USEC_PER_SEC);

  NostrEvent *rumor = nostr_nip17_unwrap_seal(seal, gh_test_secret[key]);
  g_assert_nonnull(rumor);
  gchar id[65] = { 0 };
  g_assert_cmpint(nostr_event_compute_id(rumor, id), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpstr(id, ==, rumor_id);
  g_assert_cmpuint(tag_count(rumor, "expiration", &value), ==, rumor_expiration ? 1 : 0);
  g_assert_cmpint(value, ==, rumor_expiration);
  nostr_event_free(rumor);
  nostr_event_free(seal);
  nostr_event_free(wrap);
}

/* ---- the account and the signer ----------------------------------------------- */

typedef struct {
  GhTestSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
} Account;

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data; (void)error;
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
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static void
account_up(Account *a)
{
  memset(a, 0, sizeof *a);
  keys_init();
  if (!bus_up) {
    gh_test_bus_up(&bus);
    bus_up = TRUE;
  }
  gh_test_signer_up(&bus, &a->mock);
  a->settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { DISC, NULL };
  g_settings_set_strv(a->settings, "discovery-relays", sources);
  g_settings_set_string(a->settings, "signer-method", "auto");
  g_settings_set_string(a->settings, "current-npub", npub_alice);
  a->accounts = gh_account_controller_new_full(a->settings, bus.client, fake_list, NULL);
  gh_test_spin_until(listed, a->accounts);
  g_assert_cmpint(gh_account_controller_get_state(a->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
}

static void
account_down(Account *a)
{
  gh_test_release(a->accounts);
  GhTestSenders check = { &bus, &a->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  drain();
  g_object_unref(a->settings);
  gh_test_signer_down(&bus, &a->mock);
}

/* ---- EX-1 through real seals -------------------------------------------------------- */

typedef struct {
  GhNip17Envelope *envelope;
  GError *error;
  gboolean done;
} Sealed;

static void
sealed_cb(GObject *source, GAsyncResult *result, gpointer data)
{
  Sealed *s = data;
  (void)source;
  s->envelope = gh_nip17_envelope_build_finish(result, &s->error);
  s->done = TRUE;
}

static gboolean
is_done(gpointer data)
{
  return ((Sealed *)data)->done;
}

/* Seals rumor with outer (NULL: the plain seal) and waits. */
static void
seal(Account *a, const gchar *rumor, const GhNip17OuterExpiration *outer, Sealed *s)
{
  memset(s, 0, sizeof *s);
  if (outer)
    gh_nip17_envelope_seal_expiring_async(a->accounts, rumor, outer, NULL, sealed_cb, s);
  else
    gh_nip17_envelope_seal_async(a->accounts, rumor, NULL, sealed_cb, s);
  gh_test_spin_until(is_done, s);
}

static void
sealed_clear(Sealed *s)
{
  g_clear_pointer(&s->envelope, gh_nip17_envelope_free);
  g_clear_error(&s->error);
}

static void
assert_refused(Account *a, const gchar *rumor, const GhNip17OuterExpiration *outer)
{
  guint calls = a->mock.calls;
  Sealed s;
  seal(a, rumor, outer, &s);
  g_assert_null(s.envelope);
  g_assert_error(s.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint(a->mock.calls, ==, calls); /* refused before any signer call */
  sealed_clear(&s);
}

void
test_expiry_ex1_envelope(void)
{
  Account a;
  account_up(&a);
  const gint64 expires = T0 + DAY;
  g_autoptr(GError) error = NULL;

  /* The rumor carries the exact expiration, inside the encryption. */
  g_autofree gchar *rumor_id = NULL;
  g_autofree gchar *rumor = gh_nip17_rumor_new_expiring(hex_alice, hex_bob, "vanishing", T0,
                                                        expires, &rumor_id, &error);
  g_assert_no_error(error);
  gint64 created_at = 0, expires_at = 0;
  g_assert_true(gh_nip17_rumor_get_expiration(rumor, &created_at, &expires_at));
  g_assert_cmpint(created_at, ==, T0);
  g_assert_cmpint(expires_at, ==, expires);
  g_assert_null(gh_nip17_rumor_new_expiring(hex_alice, hex_bob, "x", T0, T0, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_assert_null(gh_nip17_rumor_new_expiring(hex_alice, hex_bob, "x", T0,
                                            GH_NIP17_MAX_EXPIRATION + 1, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_autofree gchar *plain_id = NULL;
  g_autofree gchar *plain = gh_nip17_rumor_new(hex_alice, hex_bob, "stays", T0, &plain_id, NULL);
  g_assert_true(gh_nip17_rumor_get_expiration(plain, NULL, &expires_at));
  g_assert_cmpint(expires_at, ==, 0);

  /* Each layer gets its own drawn value (fake clock: fixed draws). */
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  const guint32 draws[] = { 0, (guint32)DAY, 1800, 7200 };
  for (guint i = 0; i < G_N_ELEMENTS(draws); i++)
    gh_clock_fake_push_random(clock, draws[i]);
  GhNip17OuterExpiration outer;
  g_assert_true(gh_expiry_draw_outer(clock, T0, expires, &outer));
  g_assert_cmpint(outer.recipient.seal, ==, ceil_hour(expires));
  g_assert_cmpint(outer.recipient.wrap, ==, ceil_hour(expires + DAY));
  g_assert_cmpint(outer.self_copy.seal, ==, ceil_hour(expires + 1800));
  g_assert_cmpint(outer.self_copy.wrap, ==, ceil_hour(expires + 7200));

  /* EX-1: the rumor exact; the seal and wrap of each copy their own values,
   * all within PT-7's bounds (checked on the draws in test_expiry.c). */
  guint calls = a.mock.calls;
  Sealed s;
  seal(&a, rumor, &outer, &s);
  g_assert_no_error(s.error);
  g_assert_nonnull(s.envelope);
  g_assert_cmpuint(a.mock.calls, ==, calls + 4); /* two encryptions, two seal signatures */
  assert_layers(s.envelope->recipient_wrap_json, KEY_BOB, outer.recipient.wrap,
                outer.recipient.seal, rumor_id, expires);
  assert_layers(s.envelope->sender_wrap_json, KEY_ALICE, outer.self_copy.wrap,
                outer.self_copy.seal, rumor_id, expires);
  sealed_clear(&s);

  /* Never an expiring rumor without outer expirations (the plain seal
   * included), none earlier than the rumor's, none on a rumor that does not
   * expire; nothing reaches the signer. */
  assert_refused(&a, rumor, NULL);
  GhNip17OuterExpiration early = outer;
  early.self_copy.wrap = expires - 1;
  assert_refused(&a, rumor, &early);
  early = outer;
  early.recipient.seal = expires - HOUR;
  assert_refused(&a, rumor, &early);
  assert_refused(&a, plain, &outer);

  /* A message that does not disappear has no expiration anywhere. */
  seal(&a, plain, NULL, &s);
  g_assert_no_error(s.error);
  assert_layers(s.envelope->recipient_wrap_json, KEY_BOB, 0, 0, plain_id, 0);
  assert_layers(s.envelope->sender_wrap_json, KEY_ALICE, 0, 0, plain_id, 0);
  sealed_clear(&s);

  /* A note to self uses only the self-copy's values. */
  g_autofree gchar *memo_id = NULL;
  g_autofree gchar *memo = gh_nip17_rumor_new_expiring(hex_alice, hex_alice, "memo", T0,
                                                      T0 + GH_EXPIRY_TIMER_WEEK, &memo_id, NULL);
  GhNip17OuterExpiration self_only;
  g_assert_true(gh_expiry_draw_outer(clock, T0, T0 + GH_EXPIRY_TIMER_WEEK, &self_only));
  self_only.recipient = (GhNip17LayerExpiration){ 0, 0 };
  seal(&a, memo, &self_only, &s);
  g_assert_no_error(s.error);
  g_assert_null(s.envelope->recipient_wrap_json);
  assert_layers(s.envelope->sender_wrap_json, KEY_ALICE, self_only.self_copy.wrap,
                self_only.self_copy.seal, memo_id, T0 + GH_EXPIRY_TIMER_WEEK);
  sealed_clear(&s);
  account_down(&a);
}

/* ---- EX-1 through the durable outbox -------------------------------------------------- */

/* The account's own relay lists, discovered on DISC. */
typedef struct {
  GhRelayScope *scope;
  gchar *author;
} ScopeOpen;

static void
scope_open_free(gpointer data)
{
  ScopeOpen *open = data;
  gh_relay_scope_unref(open->scope);
  g_free(open->author);
  g_free(open);
}

static gpointer
scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data,
           GError **error)
{
  (void)url; (void)error;
  ScopeOpen *open = g_new0(ScopeOpen, 1);
  open->scope = gh_relay_scope_ref(scope);
  open->author = g_strdup(nostr_filter_authors_get(&filters->filters[0], 0));
  g_ptr_array_add(data, open);
  return open;
}

static void
scope_close(gpointer handle, gpointer data)
{
  (void)handle; (void)data;
}

static const GhRelayTransport scope_transport = { scope_open, scope_close };

/* Every wrap handed to a relay; no relay ever answers. */
typedef struct {
  GhRelayPublish *publish;
  gchar *event_id;
  gboolean closed;
} PubOpen;

static void
pub_open_free(gpointer data)
{
  PubOpen *open = data;
  gh_relay_publish_unref(open->publish);
  g_free(open->event_id);
  g_free(open);
}

static gpointer
pub_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json, gpointer data,
         GError **error)
{
  (void)url; (void)event_json; (void)error;
  PubOpen *open = g_new0(PubOpen, 1);
  open->publish = gh_relay_publish_ref(publish);
  open->event_id = g_strdup(gh_relay_publish_get_event_id(publish));
  g_ptr_array_add(data, open);
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

/* A resolver that knows only Bob's inbox. */
#define FAKE_TYPE_RESOLVER (fake_resolver_get_type())
G_DECLARE_FINAL_TYPE(FakeResolver, fake_resolver, FAKE, RESOLVER, GObject)

struct _FakeResolver {
  GObject parent_instance;
};

static void
fake_resolve_async(GhInboxResolver *resolver, const gchar *pubkey, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new(resolver, cancellable, callback, data);
  GhInboxResult *result = g_new0(GhInboxResult, 1);
  result->recipient = g_strdup(pubkey);
  result->sources = result->answered = 1;
  result->status = GH_INBOX_NOT_FOUND;
  if (g_strcmp0(pubkey, hex_bob) == 0) {
    const gchar *relays[] = { BOB_INBOX, NULL };
    result->status = GH_INBOX_FOUND;
    result->relays = g_strdupv((GStrv)relays);
    result->event_id = g_strnfill(64, 'e');
    result->created_at = 100;
  }
  g_task_return_pointer(task, result, (GDestroyNotify)gh_inbox_result_free);
  g_object_unref(task);
}

static GhInboxResult *
fake_resolve_finish(GhInboxResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
fake_resolver_iface_init(GhInboxResolverInterface *iface)
{
  iface->resolve_async = fake_resolve_async;
  iface->resolve_finish = fake_resolve_finish;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(FakeResolver, fake_resolver, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GH_TYPE_INBOX_RESOLVER, fake_resolver_iface_init))

static void fake_resolver_class_init(FakeResolverClass *klass) { (void)klass; }
static void fake_resolver_init(FakeResolver *self) { (void)self; }

/* A network monitor that is always online. */
#define FAKE_TYPE_MONITOR (fake_monitor_get_type())
G_DECLARE_FINAL_TYPE(FakeMonitor, fake_monitor, FAKE, MONITOR, GObject)

struct _FakeMonitor {
  GObject parent_instance;
};

enum { MONITOR_PROP_0, MONITOR_PROP_AVAILABLE, MONITOR_PROP_METERED, MONITOR_PROP_CONNECTIVITY };

static gboolean
fake_monitor_initable_init(GInitable *initable, GCancellable *cancellable, GError **error)
{
  (void)initable; (void)cancellable; (void)error;
  return TRUE;
}

static void
fake_monitor_initable_iface_init(GInitableIface *iface)
{
  iface->init = fake_monitor_initable_init;
}

static gboolean
fake_monitor_can_reach(GNetworkMonitor *monitor, GSocketConnectable *connectable,
                       GCancellable *cancellable, GError **error)
{
  (void)monitor; (void)connectable; (void)cancellable; (void)error;
  return TRUE;
}

static void
fake_monitor_iface_init(GNetworkMonitorInterface *iface)
{
  iface->can_reach = fake_monitor_can_reach;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(FakeMonitor, fake_monitor, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, fake_monitor_initable_iface_init)
  G_IMPLEMENT_INTERFACE(G_TYPE_NETWORK_MONITOR, fake_monitor_iface_init))

static void
fake_monitor_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  switch (prop_id) {
  case MONITOR_PROP_AVAILABLE: g_value_set_boolean(value, TRUE); break;
  case MONITOR_PROP_METERED: g_value_set_boolean(value, FALSE); break;
  case MONITOR_PROP_CONNECTIVITY: g_value_set_enum(value, G_NETWORK_CONNECTIVITY_FULL); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
fake_monitor_class_init(FakeMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = fake_monitor_get_property;
  g_object_class_override_property(object_class, MONITOR_PROP_AVAILABLE, "network-available");
  g_object_class_override_property(object_class, MONITOR_PROP_METERED, "network-metered");
  g_object_class_override_property(object_class, MONITOR_PROP_CONNECTIVITY, "connectivity");
}

static void fake_monitor_init(FakeMonitor *self) { (void)self; }

static gchar *
signed_inbox_list(const gchar *url)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 10050);
  nostr_event_set_created_at(event, 100);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("relay", url, NULL)));
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[KEY_ALICE]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

typedef struct {
  guint removed;
  gint64 removed_id;
} OutboxEvents;

static void
on_item_removed(GhOutbox *outbox, GhOutboxItem *item, gpointer data)
{
  OutboxEvents *events = data;
  (void)outbox;
  events->removed++;
  events->removed_id = gh_outbox_item_get_outbox_id(item);
}

typedef struct {
  GhOutbox *outbox;
  GPtrArray *rumor_ids;
  guint n_outbox;
} PurgeEvents;

/* What the application does on "purged" (gh-app-services.c). */
static void
on_purged(GhExpiry *expiry, const gchar *const *rumor_ids, guint n_outbox, gpointer data)
{
  PurgeEvents *events = data;
  (void)expiry;
  for (guint i = 0; rumor_ids && rumor_ids[i]; i++)
    g_ptr_array_add(events->rumor_ids, g_strdup(rumor_ids[i]));
  events->n_outbox += n_outbox;
  if (n_outbox > 0)
    gh_outbox_prune(events->outbox);
}

static gboolean
sealed(gpointer data)
{
  GhStoreOutboxState state = gh_outbox_item_get_state(data);
  return state != GH_STORE_OUTBOX_QUEUED && state != GH_STORE_OUTBOX_SEALING;
}

static const gchar *
stored_wrap(GhStoreOutboxEntry *entry, GhStoreOutboxRole role)
{
  for (guint i = 0; i < entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, i);
    if (event->role == role)
      return event->event_json;
  }
  g_error("no stored wrap with role %d", role);
}

void
test_expiry_ex1_outbox(void)
{
  Account a;
  account_up(&a);
  g_autoptr(GPtrArray) scopes = g_ptr_array_new_with_free_func(scope_open_free);
  g_autoptr(GPtrArray) publishes = g_ptr_array_new_with_free_func(pub_open_free);
  GhAccountRelays *relays = gh_account_relays_new(a.accounts, a.settings, &scope_transport, scopes);
  FakeResolver *resolver = g_object_new(FAKE_TYPE_RESOLVER, NULL);
  FakeMonitor *network = g_object_new(FAKE_TYPE_MONITOR, NULL);
  GhDmSender *sender = gh_dm_sender_new(a.accounts, relays, GH_INBOX_RESOLVER(resolver),
                                        &pub_transport, publishes);
  GhClock *clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  g_autofree gchar *data_dir = g_dir_make_tmp("groundhog-expiry-XXXXXX", NULL);
  g_assert_nonnull(data_dir);
  guint8 raw[GH_STORE_KEY_SIZE];
  randombytes_buf(raw, sizeof raw);
  g_autoptr(GBytes) key = g_bytes_new(raw, sizeof raw);
  GhStoreConfig store_config = { .data_dir = data_dir, .account_pubkey = hex_alice, .clock = clock };
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&store_config, key, STORE_ID, GH_STORE_OPEN_CREATE,
                                          &error);
  g_assert_no_error(error);
  GhExpiryConfig expiry_config = { .store = store };
  GhExpiry *expiry = gh_expiry_new(&expiry_config);
  g_assert_true(gh_expiry_purge(expiry, &error));
  GhOutboxConfig outbox_config = {
    .store = store,
    .accounts = a.accounts,
    .account_relays = relays,
    .inboxes = GH_INBOX_RESOLVER(resolver),
    .sender = sender,
    .network = G_NETWORK_MONITOR(network),
    .transport = &pub_transport,
    .transport_data = publishes,
  };
  GhOutbox *outbox = gh_outbox_new(&outbox_config, &error);
  g_assert_no_error(error);
  OutboxEvents outbox_events = { 0 };
  g_signal_connect(outbox, "item-removed", G_CALLBACK(on_item_removed), &outbox_events);
  PurgeEvents purge_events = { outbox, g_ptr_array_new_with_free_func(g_free), 0 };
  g_signal_connect(expiry, "purged", G_CALLBACK(on_purged), &purge_events);
  /* The account's own inbox list: the self-copy's target, apart from Bob's. */
  ScopeOpen *own = NULL;
  for (guint i = 0; i < scopes->len && !own; i++)
    if (g_strcmp0(((ScopeOpen *)g_ptr_array_index(scopes, i))->author, hex_alice) == 0)
      own = g_ptr_array_index(scopes, i);
  g_assert_nonnull(own);
  g_autofree gchar *inbox_list = signed_inbox_list(ALICE_INBOX);
  gh_relay_scope_event(own->scope, DISC, inbox_list);
  gh_relay_scope_eose(own->scope, DISC);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);

  /* Without a timer nothing expires. */
  g_autoptr(GhOutboxItem) stays = gh_outbox_send(outbox, hex_bob, "stays", &error);
  g_assert_no_error(error);
  gh_test_spin_until(sealed, stays);
  g_autoptr(GhStoreOutboxEntry) stays_entry =
    gh_store_outbox_load(store, gh_outbox_item_get_outbox_id(stays), &error);
  g_assert_no_error(error);
  gint64 created_at = 0, expires_at = -1;
  g_assert_true(gh_nip17_rumor_get_expiration(stays_entry->rumor_json, &created_at, &expires_at));
  g_assert_cmpint(expires_at, ==, 0);
  g_autofree gchar *stays_id = event_id_of(stays_entry->rumor_json);
  assert_layers(stored_wrap(stays_entry, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP), KEY_BOB, 0, 0,
                stays_id, 0);
  g_assert_cmpint(gh_expiry_get_next_run(expiry), ==, 0);

  /* One day: the timer is kept in the store; the next message expires at
   * send + 1 day inside, and each seal and wrap later (fixed draws). */
  g_autofree gchar *room = strcmp(hex_alice, hex_bob) < 0
    ? g_strconcat(hex_alice, ",", hex_bob, NULL) : g_strconcat(hex_bob, ",", hex_alice, NULL);
  g_assert_true(gh_expiry_set_timer(expiry, room, DAY, &error));
  g_assert_no_error(error);
  const guint32 draws[] = { 0, (guint32)DAY, 1800, 7200 };
  for (guint i = 0; i < G_N_ELEMENTS(draws); i++)
    gh_clock_fake_push_random(clock, draws[i]);
  const gint64 sent = gh_clock_get_unix(clock), expires = sent + DAY;
  g_autoptr(GhOutboxItem) vanishes = gh_outbox_send(outbox, hex_bob, "vanishes", &error);
  g_assert_no_error(error);
  const gint64 vanishes_outbox = gh_outbox_item_get_outbox_id(vanishes);
  /* The store told the scheduler at T-enqueue. */
  g_assert_cmpint(gh_expiry_get_next_run(expiry), ==, expires);
  gh_test_spin_until(sealed, vanishes);
  g_autoptr(GhStoreOutboxEntry) entry = gh_store_outbox_load(store, vanishes_outbox, &error);
  g_assert_no_error(error);
  g_assert_true(gh_nip17_rumor_get_expiration(entry->rumor_json, &created_at, &expires_at));
  g_assert_cmpint(created_at, ==, sent);
  g_assert_cmpint(expires_at, ==, expires);
  g_autofree gchar *vanishes_id = event_id_of(entry->rumor_json);
  const gchar *recipient_wrap = stored_wrap(entry, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  assert_layers(recipient_wrap, KEY_BOB, ceil_hour(expires + DAY), ceil_hour(expires),
                vanishes_id, expires);
  assert_layers(stored_wrap(entry, GH_STORE_OUTBOX_ROLE_SELF_WRAP), KEY_ALICE,
                ceil_hour(expires + 7200), ceil_hour(expires + 1800), vanishes_id, expires);
  g_autofree gchar *recipient_wrap_id = event_id_of(recipient_wrap);
  PubOpen *in_flight = NULL;
  for (guint i = 0; i < publishes->len && !in_flight; i++)
    if (g_strcmp0(((PubOpen *)g_ptr_array_index(publishes, i))->event_id, recipient_wrap_id) == 0)
      in_flight = g_ptr_array_index(publishes, i);
  g_assert_nonnull(in_flight);
  g_assert_false(in_flight->closed);
  const guint opens = publishes->len;

  /* Bob's relay never answered. At the expiration the purge deletes the
   * message with its outbox entry (rumor and wraps), and the outbox lets go:
   * its publish closes, "item-removed" is emitted, nothing is sent again. */
  gh_clock_fake_advance(clock, (expires - sent - 1) * G_USEC_PER_SEC);
  g_assert_cmpuint(purge_events.rumor_ids->len, ==, 0);
  gh_clock_fake_advance(clock, G_USEC_PER_SEC);
  g_assert_cmpuint(purge_events.rumor_ids->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(purge_events.rumor_ids, 0), ==, vanishes_id);
  g_assert_cmpuint(purge_events.n_outbox, ==, 1);
  g_assert_cmpuint(outbox_events.removed, ==, 1);
  g_assert_cmpint(outbox_events.removed_id, ==, vanishes_outbox);
  g_assert_true(in_flight->closed);
  g_autoptr(GError) gone_error = NULL;
  g_assert_null(gh_store_outbox_load(store, vanishes_outbox, &gone_error));
  g_assert_error(gone_error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  g_autoptr(GhOutboxItem) gone = gh_outbox_lookup(outbox, vanishes_outbox);
  g_assert_null(gone);
  g_autoptr(GhOutboxItem) kept = gh_outbox_lookup(outbox, gh_outbox_item_get_outbox_id(stays));
  g_assert_nonnull(kept);
  drain();
  for (guint i = opens; i < publishes->len; i++)
    g_assert_cmpstr(((PubOpen *)g_ptr_array_index(publishes, i))->event_id, !=, recipient_wrap_id);

  g_ptr_array_unref(purge_events.rumor_ids);
  gh_test_release(outbox);
  gh_test_release(sender);
  gh_test_release(relays);
  g_object_unref(resolver);
  g_object_unref(network);
  gh_store_close(store);
  /* The application disposes it right after its store closed. */
  g_object_run_dispose(G_OBJECT(expiry));
  g_object_unref(expiry);
  gh_clock_unref(clock);
  account_down(&a);
  g_autoptr(GError) rm_error = NULL;
  g_assert_true(gh_store_delete_files(data_dir, hex_alice, &rm_error));
  g_assert_no_error(rm_error);
  g_autofree gchar *accounts_dir = g_build_filename(data_dir, "groundhog", "accounts", NULL);
  g_autofree gchar *groundhog_dir = g_build_filename(data_dir, "groundhog", NULL);
  g_rmdir(accounts_dir);
  g_rmdir(groundhog_dir);
  g_rmdir(data_dir);
}
