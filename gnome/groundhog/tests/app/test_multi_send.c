/* W17 (nostrc-qp24.78): NIP-17 send to a room of several people.
 *
 * One rumor per message, with a "p" tag per recipient; one seal and one
 * gift wrap per recipient plus the self-copy, each under a fresh ephemeral
 * key with its own randomized created_at (NIP-59), each wrap only to its
 * receiver's kind-10050 relays; per-recipient durable state, retries and
 * status in the outbox; honest "Sent" / "Sent to some people"; nothing
 * published for a recipient without a 10050; the 10-recipient limit; the
 * disappearing timer (G07) on every layer; GhAuthPolicy (ephemeral AUTH on
 * recipients' relays, the account only on its own); a block in a room.
 *
 * Harnesses: a real SQLCipher GhStore, the fake GhClock (H6) for every
 * outbox timer and jitter, a fake GNetworkMonitor, a fake GhInboxResolver,
 * the mock org.nostr.Signer on a private bus (gh-test-signer.h) and, with
 * libsoup, real local store-and-serve WebSocket relays (H2,
 * tests/relay/wire-relay.h) reached through the real gnostr publish
 * transport. Three accounts (Alice sends, Bob and Carol receive; Dave for
 * a third recipient): what the relays store is opened with each receiver's
 * key, so each can be shown to open only its own wrap. Waits iterate the
 * main context against a deadline; the fake clock only moves when a test
 * moves it. */
#include "gh-outbox.h"
#include "gh-auth-policy.h"
#include "gh-expiry.h"
#include "gh-nip17-envelope.h"
#include "gh-store-conversations.h"
#include "gh-test-signer.h"
#include "nostr-tag.h"
#include "nostr/nip59/nip59.h"

#include <glib/gstdio.h>

#ifdef GROUNDHOG_TEST_WIRE
#include "../relay/wire-relay.h"
#endif
#if GROUNDHOG_TEST_ATTACHMENTS && defined(GROUNDHOG_TEST_WIRE)
#include "blossom-fixture.h"
#include "gh-attachment.h"
#endif

#define DISC        "wss://discovery.test.invalid"
#define STORE_ID    "7c1e2a9b-3d4f-4a5b-8c6d-9e0f1a2b3c4d"
#define T0          G_GINT64_CONSTANT(1900000000)
#define KEY_ALICE   1
#define KEY_BOB     2
#define KEY_CAROL   3
#define KEY_DAVE    4

static gchar *hex[GH_TEST_KEYS];
static gchar *npub_alice;
static GhTestBus shared_bus;

/* ---- small helpers ------------------------------------------------------- */

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static void
rm_rf(const gchar *path)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *name;
    while (dir && (name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      rm_rf(child);
    }
    if (dir)
      g_dir_close(dir);
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

static G_GNUC_UNUSED gchar *
signed_event(guint key, int kind, gint64 created_at, NostrTags *tags)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

/* A syntactically valid pubkey that is nobody's (limits only). */
static gchar *
hex_of(const gchar *seed)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

static gchar *
numbered_key(const gchar *what, guint i)
{
  g_autofree gchar *seed = g_strdup_printf("%s %u", what, i);
  return hex_of(seed);
}

/* NULL-terminated, sorted: the room key of these members (charter §3.3). */
static gint
compare_items(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(*(const gchar *const *) a, *(const gchar *const *) b);
}

static gchar *
room_key(const gchar *const *members)
{
  g_autoptr(GPtrArray) sorted = g_ptr_array_new();
  for (guint i = 0; members[i]; i++)
    g_ptr_array_add(sorted, (gpointer) members[i]);
  g_ptr_array_sort(sorted, compare_items);
  g_ptr_array_add(sorted, NULL);
  return g_strjoinv(",", (gchar **) sorted->pdata);
}

/* The rumor's "p" values, in order. */
static GStrv
rumor_p_tags(const gchar *rumor_json)
{
  NostrEvent *rumor = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(rumor, rumor_json, NULL), ==, 1);
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  NostrTags *tags = nostr_event_get_tags(rumor);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++)
    if (g_strcmp0(nostr_tag_get_key(nostr_tags_get(tags, i)), "p") == 0)
      g_strv_builder_add(builder, nostr_tag_get_value(nostr_tags_get(tags, i)));
  nostr_event_free(rumor);
  return g_strv_builder_end(builder);
}

/* The event's expiration tag value, 0 without one. */
static G_GNUC_UNUSED gint64
expiration_of(NostrEvent *event)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), "expiration") == 0)
      return g_ascii_strtoll(nostr_tag_get_value(tag), NULL, 10);
  }
  return 0;
}

/* ---- fake GNetworkMonitor ------------------------------------------------ */

#define FAKE_TYPE_MONITOR (fake_monitor_get_type())
G_DECLARE_FINAL_TYPE(FakeMonitor, fake_monitor, FAKE, MONITOR, GObject)

struct _FakeMonitor {
  GObject parent_instance;
  gboolean available;
};

enum { MONITOR_PROP_0, MONITOR_PROP_AVAILABLE, MONITOR_PROP_METERED, MONITOR_PROP_CONNECTIVITY };

static gboolean
fake_monitor_initable_init(GInitable *initable, GCancellable *cancellable, GError **error)
{
  (void) initable; (void) cancellable; (void) error;
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
  (void) connectable; (void) cancellable;
  if (FAKE_MONITOR(monitor)->available)
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE, "offline");
  return FALSE;
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
  FakeMonitor *self = FAKE_MONITOR(object);
  switch (prop_id) {
  case MONITOR_PROP_AVAILABLE: g_value_set_boolean(value, self->available); break;
  case MONITOR_PROP_METERED: g_value_set_boolean(value, FALSE); break;
  case MONITOR_PROP_CONNECTIVITY:
    g_value_set_enum(value, self->available ? G_NETWORK_CONNECTIVITY_FULL
                                            : G_NETWORK_CONNECTIVITY_LOCAL);
    break;
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

static void
fake_monitor_init(FakeMonitor *self)
{
  self->available = TRUE;
}

/* ---- fake GhInboxResolver ------------------------------------------------ */

#define FAKE_TYPE_RESOLVER (fake_resolver_get_type())
G_DECLARE_FINAL_TYPE(FakeResolver, fake_resolver, FAKE, RESOLVER, GObject)

struct _FakeResolver {
  GObject parent_instance;
  GHashTable *answers; /* pubkey -> GhInboxResult; absent: NOT_FOUND */
  guint calls;
};

static void
fake_resolve_async(GhInboxResolver *resolver, const gchar *pubkey, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer data)
{
  FakeResolver *self = FAKE_RESOLVER(resolver);
  self->calls++;
  GTask *task = g_task_new(resolver, cancellable, callback, data);
  GhInboxResult *answer = g_hash_table_lookup(self->answers, pubkey);
  GhInboxResult *copy = NULL;
  if (answer) {
    copy = gh_inbox_result_copy(answer);
  } else {
    copy = g_new0(GhInboxResult, 1);
    copy->status = GH_INBOX_NOT_FOUND;
    copy->recipient = g_strdup(pubkey);
    copy->sources = copy->answered = 1;
  }
  g_task_return_pointer(task, copy, (GDestroyNotify) gh_inbox_result_free);
  g_object_unref(task);
}

static GhInboxResult *
fake_resolve_finish(GhInboxResolver *resolver, GAsyncResult *result, GError **error)
{
  (void) resolver;
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

static void
fake_resolver_finalize(GObject *object)
{
  g_hash_table_unref(FAKE_RESOLVER(object)->answers);
  G_OBJECT_CLASS(fake_resolver_parent_class)->finalize(object);
}

static void
fake_resolver_class_init(FakeResolverClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = fake_resolver_finalize;
}

static void
fake_resolver_init(FakeResolver *self)
{
  self->answers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify) gh_inbox_result_free);
}

/* status FOUND with the NULL-terminated URLs, or NOT_FOUND/UNREACHABLE. */
static void
fake_resolver_set(FakeResolver *self, const gchar *pubkey, GhInboxStatus status, ...)
{
  GhInboxResult *result = g_new0(GhInboxResult, 1);
  result->status = status;
  result->recipient = g_strdup(pubkey);
  result->sources = 1;
  result->answered = status == GH_INBOX_UNREACHABLE ? 0 : 1;
  result->failed = status == GH_INBOX_UNREACHABLE ? 1 : 0;
  if (status == GH_INBOX_FOUND) {
    GPtrArray *urls = g_ptr_array_new();
    va_list args;
    va_start(args, status);
    for (const gchar *url = va_arg(args, const gchar *); url; url = va_arg(args, const gchar *))
      g_ptr_array_add(urls, g_strdup(url));
    va_end(args);
    g_ptr_array_add(urls, NULL);
    result->relays = (GStrv) g_ptr_array_free(urls, FALSE);
    result->event_id = g_strnfill(64, 'e');
    result->created_at = 100;
  }
  g_hash_table_replace(self->answers, g_strdup(pubkey), result);
}

/* ---- recording scope transport (the account's own lists) ------------------ */

typedef struct {
  GhRelayScope *scope;
  gchar *author;
  gboolean closed;
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
  (void) url; (void) error;
  ScopeOpen *open = g_new0(ScopeOpen, 1);
  open->scope = gh_relay_scope_ref(scope);
  open->author = g_strdup(nostr_filter_authors_get(&filters->filters[0], 0));
  g_ptr_array_add(data, open);
  return open;
}

static void
scope_close(gpointer handle, gpointer data)
{
  (void) data;
  ((ScopeOpen *) handle)->closed = TRUE;
}

static const GhRelayTransport scope_transport = { scope_open, scope_close };

/* ---- fixture --------------------------------------------------------------- */

typedef struct {
  GhTestSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GPtrArray *scopes;
  GhAccountRelays *relays;
  FakeResolver *resolver;
  GhDmSender *sender;
  GhClock *clock;
  gchar *data_dir;
  GhStore *store;
  FakeMonitor *network;
  GhOutbox *outbox;
} Fixture;

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void) data; (void) error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify) gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(npub_alice);
  info->label = g_strdup("Alice");
  g_ptr_array_add(ids, info);
  return ids;
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static GhStore *
store_open(const gchar *data_dir, GhClock *clock)
{
  guint8 key[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof key; i++)
    key[i] = (guint8) (0x3c ^ i);
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { data_dir, hex[KEY_ALICE], NULL, NULL, clock };
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&config, bytes, STORE_ID, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  return store;
}

/* Everything but the outbox; the sender publishes over the real gnostr
 * transport. */
static void
fixture_up(Fixture *f)
{
  memset(f, 0, sizeof *f);
  gh_test_signer_up(&shared_bus, &f->mock);
  f->settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { DISC, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub_alice);
  f->accounts = gh_account_controller_new_full(f->settings, shared_bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->scopes = g_ptr_array_new_with_free_func(scope_open_free);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &scope_transport, f->scopes);
  f->resolver = g_object_new(FAKE_TYPE_RESOLVER, NULL);
  f->clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  f->sender = gh_dm_sender_new(f->accounts, f->relays, GH_INBOX_RESOLVER(f->resolver), NULL, NULL);
  gh_dm_sender_set_publish_deadline(f->sender, 10);
  f->data_dir = g_dir_make_tmp("groundhog-multi-send-XXXXXX", NULL);
  g_assert_nonnull(f->data_dir);
  f->store = store_open(f->data_dir, f->clock);
  f->network = g_object_new(FAKE_TYPE_MONITOR, NULL);
}

static void
fixture_outbox(Fixture *f)
{
  GhOutboxConfig config = {
    .store = f->store,
    .accounts = f->accounts,
    .account_relays = f->relays,
    .inboxes = GH_INBOX_RESOLVER(f->resolver),
    .sender = f->sender,
    .network = G_NETWORK_MONITOR(f->network),
    .publish_deadline = 10, /* NULL transport: the real gnostr relays, with NIP-42 */
  };
  g_autoptr(GError) error = NULL;
  f->outbox = gh_outbox_new(&config, &error);
  g_assert_no_error(error);
  g_assert_nonnull(f->outbox);
}

static void
fixture_down(Fixture *f)
{
  if (f->outbox)
    gh_test_release(f->outbox);
  gh_test_release(f->sender);
  gh_test_release(f->relays);
  gh_test_release(f->accounts);
  GhTestSenders check = { &shared_bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  drain();
  g_ptr_array_unref(f->scopes);
  g_object_unref(f->resolver);
  g_object_unref(f->network);
  g_object_unref(f->settings);
  gh_store_close(f->store);
  gh_clock_unref(f->clock);
  gh_test_signer_down(&shared_bus, &f->mock);
  rm_rf(f->data_dir);
  g_free(f->data_dir);
}

/* The account's own 10050 lists these URLs (NULL-terminated). */
static G_GNUC_UNUSED void
settle_own(Fixture *f, ...)
{
  ScopeOpen *open = NULL;
  for (guint i = f->scopes->len; i > 0 && !open; i--) {
    ScopeOpen *candidate = g_ptr_array_index(f->scopes, i - 1);
    if (!candidate->closed && g_strcmp0(candidate->author, hex[KEY_ALICE]) == 0)
      open = candidate;
  }
  g_assert_nonnull(open);
  NostrTags *tags = nostr_tags_new(0);
  va_list args;
  va_start(args, f);
  for (const gchar *url = va_arg(args, const gchar *); url; url = va_arg(args, const gchar *))
    nostr_tags_append(tags, nostr_tag_new("relay", url, NULL));
  va_end(args);
  g_autofree gchar *inbox = signed_event(KEY_ALICE, 10050, 100, tags);
  gh_relay_scope_event(open->scope, DISC, inbox);
  gh_relay_scope_eose(open->scope, DISC);
  g_assert_cmpint(gh_account_relays_get_state(f->relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);
}

static GhOutboxItem *
send_room(Fixture *f, const gchar *text, ...)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  va_list args;
  va_start(args, text);
  for (const gchar *p = va_arg(args, const gchar *); p; p = va_arg(args, const gchar *))
    g_strv_builder_add(builder, p);
  va_end(args);
  g_auto(GStrv) recipients = g_strv_builder_end(builder);
  g_autoptr(GError) error = NULL;
  GhOutboxItem *item = gh_outbox_send_room(f->outbox, (const gchar *const *) recipients, text,
                                           &error);
  g_assert_no_error(error);
  g_assert_nonnull(item);
  return item;
}

static G_GNUC_UNUSED GhStoreOutboxEntry *
load_entry(Fixture *f, GhOutboxItem *item)
{
  g_autoptr(GError) error = NULL;
  GhStoreOutboxEntry *entry = gh_store_outbox_load(f->store, gh_outbox_item_get_outbox_id(item),
                                                   &error);
  g_assert_no_error(error);
  return entry;
}

static G_GNUC_UNUSED GhStoreOutboxEvent *
entry_wrap_for(GhStoreOutboxEntry *entry, const gchar *pubkey, GhStoreOutboxRole role)
{
  for (guint i = 0; i < entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, i);
    if (event->role == role && g_strcmp0(event->target_pubkey, pubkey) == 0)
      return event;
  }
  g_error("no stored wrap for %s", pubkey);
}

static G_GNUC_UNUSED const GhOutboxRecipient *
recipient_in(GPtrArray *recipients, const gchar *pubkey)
{
  for (guint i = 0; i < recipients->len; i++) {
    const GhOutboxRecipient *recipient = g_ptr_array_index(recipients, i);
    if (g_strcmp0(recipient->pubkey, pubkey) == 0)
      return recipient;
  }
  g_error("%s is not a recipient", pubkey);
}

static G_GNUC_UNUSED GhOutboxRecipientState
state_of(GhOutboxItem *item, guint key)
{
  g_autoptr(GPtrArray) recipients = gh_outbox_item_dup_recipients(item);
  return recipient_in(recipients, hex[key])->state;
}

/* ---- waits ------------------------------------------------------------------- */

typedef struct {
  Fixture *f;
  GhOutboxItem *item;
  GhMessageStatus status;
} Until;

static G_GNUC_UNUSED gboolean
round_over(GhOutboxItem *item)
{
  GhStoreOutboxState state = gh_outbox_item_get_state(item);
  return state != GH_STORE_OUTBOX_PUBLISHING && state != GH_STORE_OUTBOX_SEALED &&
         state != GH_STORE_OUTBOX_QUEUED && state != GH_STORE_OUTBOX_SEALING;
}

/* The status holds and the round is over. Meanwhile the fake clock releases
 * a room's spaced wraps (S4: at most 3 s apart) but nothing later: a retry
 * (>= 12 s) waits for the test. */
static G_GNUC_UNUSED gboolean
until_step(gpointer data)
{
  Until *until = data;
  if (gh_outbox_item_get_status(until->item) == until->status && round_over(until->item))
    return TRUE;
  gint64 next = gh_clock_fake_get_next_deadline(until->f->clock);
  gint64 now = gh_clock_get_monotonic_time(until->f->clock);
  if (next >= 0 && next - now <= 3 * G_USEC_PER_SEC)
    gh_clock_fake_advance(until->f->clock, MAX(next - now, 0));
  return FALSE;
}

static G_GNUC_UNUSED void
wait_until(Fixture *f, GhOutboxItem *item, GhMessageStatus status)
{
  Until until = { f, item, status };
  gh_test_spin_until(until_step, &until);
  drain();
}

static G_GNUC_UNUSED gboolean
status_is(gpointer data)
{
  Until *until = data;
  return gh_outbox_item_get_status(until->item) == until->status;
}

static G_GNUC_UNUSED void
wait_status(GhOutboxItem *item, GhMessageStatus status)
{
  Until until = { NULL, item, status };
  gh_test_spin_until(status_is, &until);
}

/* Advances the fake clock to its next timeout (a retry). */
static G_GNUC_UNUSED void
advance_to_next(Fixture *f)
{
  drain(); /* a message resumed or just evaluated schedules its retry first */
  gint64 next = gh_clock_fake_get_next_deadline(f->clock);
  g_assert_cmpint(next, >=, 0);
  gh_clock_fake_advance(f->clock, MAX(next - gh_clock_get_monotonic_time(f->clock), 0));
  drain();
}

/* ---- the rumor (no signer, no relay) ------------------------------------------ */

/* One rumor names every recipient once, in order; one recipient is exactly
 * the one-to-one rumor; the limits and the note-to-self rule hold. */
static void
test_room_rumor(void)
{
  const gchar *const room[] = { hex[KEY_BOB], hex[KEY_CAROL], hex[KEY_DAVE], NULL };
  g_autofree gchar *id = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *rumor = gh_nip17_rumor_new_room(hex[KEY_ALICE], room, "Hi all", T0, 0, &id,
                                                    &error);
  g_assert_no_error(error);
  g_assert_nonnull(id);
  g_auto(GStrv) p = rumor_p_tags(rumor);
  g_assert_cmpstrv(p, room);
  g_auto(GStrv) recipients = gh_nip17_rumor_dup_recipients(rumor, hex[KEY_ALICE]);
  g_assert_cmpstrv(recipients, room);
  g_assert_null(gh_nip17_rumor_get_recipient(rumor, hex[KEY_ALICE])); /* not one-to-one */
  g_assert_null(gh_nip17_rumor_dup_recipients(rumor, hex[KEY_BOB]));  /* not Bob's */

  /* One recipient: the one-to-one rumor, byte for byte (upper case in). */
  const gchar *const one[] = { hex[KEY_BOB], NULL };
  g_autofree gchar *upper = g_ascii_strup(hex[KEY_BOB], -1);
  g_autofree gchar *direct = gh_nip17_rumor_new_expiring(hex[KEY_ALICE], upper, "Hi", T0,
                                                         T0 + 60, NULL, &error);
  g_assert_no_error(error);
  g_autofree gchar *as_room = gh_nip17_rumor_new_room(hex[KEY_ALICE], one, "Hi", T0, T0 + 60,
                                                      NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(direct, ==, as_room);
  g_autofree gchar *single = gh_nip17_rumor_get_recipient(as_room, hex[KEY_ALICE]);
  g_assert_cmpstr(single, ==, hex[KEY_BOB]);

  /* A note to self is the account alone. */
  const gchar *const self[] = { hex[KEY_ALICE], NULL };
  g_autofree gchar *note = gh_nip17_rumor_new_room(hex[KEY_ALICE], self, "note", T0, 0, NULL,
                                                   &error);
  g_assert_no_error(error);
  g_auto(GStrv) note_recipients = gh_nip17_rumor_dup_recipients(note, hex[KEY_ALICE]);
  g_assert_cmpuint(g_strv_length(note_recipients), ==, 1);

  /* Refused: none, eleven, a repeat, yourself among others, a bad key. */
  const gchar *const none[] = { NULL };
  const gchar *const repeat[] = { hex[KEY_BOB], hex[KEY_CAROL], hex[KEY_BOB], NULL };
  const gchar *const with_self[] = { hex[KEY_BOB], hex[KEY_ALICE], NULL };
  const gchar *const bad[] = { hex[KEY_BOB], "not-a-key", NULL };
  const gchar *const *refused[] = { none, repeat, with_self, bad };
  for (guint i = 0; i < G_N_ELEMENTS(refused); i++) {
    g_autoptr(GError) refusal = NULL;
    g_assert_null(gh_nip17_rumor_new_room(hex[KEY_ALICE], refused[i], "x", T0, 0, NULL,
                                          &refusal));
    g_assert_error(refusal, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
  g_autoptr(GPtrArray) eleven = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i <= GH_NIP17_MAX_SEND_RECIPIENTS; i++)
    g_ptr_array_add(eleven, numbered_key("rumor", i));
  g_ptr_array_add(eleven, NULL);
  g_autoptr(GError) too_many = NULL;
  g_assert_null(gh_nip17_rumor_new_room(hex[KEY_ALICE], (const gchar *const *) eleven->pdata,
                                        "x", T0, 0, NULL, &too_many));
  g_assert_error(too_many, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_ptr_array_remove_index(eleven, GH_NIP17_MAX_SEND_RECIPIENTS); /* the extra key */
  g_autofree gchar *ten = gh_nip17_rumor_new_room(hex[KEY_ALICE],
                                                  (const gchar *const *) eleven->pdata, "x", T0,
                                                  0, NULL, &error);
  g_assert_no_error(error);
  g_auto(GStrv) ten_p = rumor_p_tags(ten);
  g_assert_cmpuint(g_strv_length(ten_p), ==, GH_NIP17_MAX_SEND_RECIPIENTS);
}

/* G21: a kind-15 file message to a room is built by the same room rumor
 * builder (the same "p" tags, limits and expiration), reads back through
 * the same recipient and expiration parsing the seal and the outbox use,
 * and is each receiver's message; one recipient is exactly the one-to-one
 * file rumor. */
static GhNip17File *
room_file(void)
{
  GhNip17File *file = g_new0(GhNip17File, 1);
  file->url = g_strdup("https://blossom.test.invalid/"
                       "7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730");
  file->file_type = g_strdup("image/jpeg");
  file->nonce_size = GH_NIP17_FILE_NONCE_SIZE;
  for (guint i = 0; i < sizeof file->key; i++)
    file->key[i] = (guint8) (i * 7 + 1);
  g_strlcpy(file->x, "7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730",
            sizeof file->x);
  file->size = 4096;
  file->width = 640;
  file->height = 480;
  return file;
}

static void
test_room_file_rumor(void)
{
  g_autoptr(GhNip17File) file = room_file();
  const gchar *const room[] = { hex[KEY_BOB], hex[KEY_CAROL], NULL };
  g_autofree gchar *id = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *rumor = gh_nip17_rumor_new_file_room(hex[KEY_ALICE], room, file, T0, T0 + 60,
                                                         &id, &error);
  g_assert_no_error(error);
  g_assert_nonnull(strstr(rumor, "\"kind\":15"));
  g_auto(GStrv) p = rumor_p_tags(rumor);
  g_assert_cmpstrv(p, room);
  g_auto(GStrv) recipients = gh_nip17_rumor_dup_recipients(rumor, hex[KEY_ALICE]);
  g_assert_cmpstrv(recipients, room);
  gint64 created_at = 0, expires_at = 0;
  g_assert_true(gh_nip17_rumor_get_expiration(rumor, &created_at, &expires_at));
  g_assert_cmpint(created_at, ==, T0);
  g_assert_cmpint(expires_at, ==, T0 + 60);
  g_autoptr(GhNip17File) back = gh_nip17_file_from_rumor(rumor, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(back->x, ==, file->x);
  g_assert_cmpmem(back->key, sizeof back->key, file->key, sizeof file->key);
  /* Each receiver's message: kind 15, the room of all three, the file. */
  const guint receivers[] = { KEY_BOB, KEY_CAROL };
  for (guint i = 0; i < G_N_ELEMENTS(receivers); i++) {
    g_autoptr(GhMessage) message = gh_message_new_from_rumor(hex[receivers[i]], rumor, &error);
    g_assert_no_error(error);
    g_assert_cmpint(gh_message_get_kind(message), ==, 15);
    g_assert_cmpuint(g_strv_length((gchar **) gh_message_get_participants(message)), ==, 3);
    g_autoptr(GhNip17File) theirs = gh_message_dup_file(message);
    g_assert_nonnull(theirs);
    g_assert_cmpstr(theirs->url, ==, file->url);
  }

  /* One recipient: the one-to-one file rumor, byte for byte. */
  const gchar *const one[] = { hex[KEY_BOB], NULL };
  g_autofree gchar *as_room = gh_nip17_rumor_new_file_room(hex[KEY_ALICE], one, file, T0, 0, NULL,
                                                           &error);
  g_assert_no_error(error);
  g_autofree gchar *direct = gh_nip17_file_rumor_new(hex[KEY_ALICE], hex[KEY_BOB], file, T0, 0,
                                                     NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(as_room, ==, direct);
  g_autofree gchar *single = gh_nip17_rumor_get_recipient(as_room, hex[KEY_ALICE]);
  g_assert_cmpstr(single, ==, hex[KEY_BOB]);

  /* The room rules, and an incomplete file, refuse before anything else. */
  g_autoptr(GPtrArray) eleven = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i <= GH_NIP17_MAX_SEND_RECIPIENTS; i++)
    g_ptr_array_add(eleven, numbered_key("file", i));
  g_ptr_array_add(eleven, NULL);
  const gchar *const repeat[] = { hex[KEY_BOB], hex[KEY_BOB], NULL };
  const gchar *const with_self[] = { hex[KEY_BOB], hex[KEY_ALICE], NULL };
  const gchar *const *refused[] = { (const gchar *const *) eleven->pdata, repeat, with_self };
  for (guint i = 0; i < G_N_ELEMENTS(refused); i++) {
    g_autoptr(GError) refusal = NULL;
    g_assert_null(gh_nip17_rumor_new_file_room(hex[KEY_ALICE], refused[i], file, T0, 0, NULL,
                                               &refusal));
    g_assert_error(refusal, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
  g_autoptr(GhNip17File) incomplete = gh_nip17_file_copy(file);
  incomplete->x[0] = 'z';
  g_autoptr(GError) refusal = NULL;
  g_assert_null(gh_nip17_rumor_new_file_room(hex[KEY_ALICE], room, incomplete, T0, 0, NULL,
                                             &refusal));
  g_assert_error(refusal, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

/* ---- the outbox without relays: limits, the conversation, a block ------------ */

/* T-enqueue only (offline): the 10-recipient limit, repeated or self
 * recipients, the room's conversation key, and one rumor for everyone. */
static void
test_limits(void)
{
  Fixture f;
  fixture_up(&f);
  f.network->available = FALSE;
  fixture_outbox(&f);
  g_autoptr(GPtrArray) people = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i <= GH_NIP17_MAX_SEND_RECIPIENTS; i++)
    g_ptr_array_add(people, numbered_key("person", i));
  g_ptr_array_add(people, NULL);
  const gchar *const *eleven = (const gchar *const *) people->pdata;
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_outbox_send_room(f.outbox, eleven, "too many", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  const gchar *const repeat[] = { hex[KEY_BOB], hex[KEY_BOB], NULL };
  g_assert_null(gh_outbox_send_room(f.outbox, repeat, "twice", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  const gchar *const with_self[] = { hex[KEY_BOB], hex[KEY_ALICE], NULL };
  g_assert_null(gh_outbox_send_room(f.outbox, with_self, "me too", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_autoptr(GPtrArray) items = gh_outbox_dup_items(f.outbox);
  g_assert_cmpuint(items->len, ==, 0); /* nothing was queued */

  /* Ten is the limit, and fine. */
  g_ptr_array_remove_index(people, GH_NIP17_MAX_SEND_RECIPIENTS);
  g_autoptr(GhOutboxItem) item = gh_outbox_send_room(f.outbox, eleven, "ten of you", &error);
  g_assert_no_error(error);
  g_auto(GStrv) p = rumor_p_tags(gh_outbox_item_get_rumor_json(item));
  g_assert_cmpuint(g_strv_length(p), ==, GH_NIP17_MAX_SEND_RECIPIENTS);
  g_autoptr(GPtrArray) recipients = gh_outbox_item_dup_recipients(item);
  g_assert_cmpuint(recipients->len, ==, GH_NIP17_MAX_SEND_RECIPIENTS);
  for (guint i = 0; i < recipients->len; i++)
    g_assert_cmpint(((GhOutboxRecipient *) g_ptr_array_index(recipients, i))->state, ==,
                    GH_OUTBOX_RECIPIENT_WAITING);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_QUEUED_OFFLINE);

  /* The room: every member, sorted, the account included (charter §3.3). */
  g_autoptr(GPtrArray) members = g_ptr_array_new();
  g_ptr_array_add(members, hex[KEY_ALICE]);
  for (guint i = 0; eleven[i]; i++)
    g_ptr_array_add(members, (gpointer) eleven[i]);
  g_ptr_array_add(members, NULL);
  g_autofree gchar *key = room_key((const gchar *const *) members->pdata);
  gint64 conversation = 0;
  g_assert_true(gh_store_find_conversation(f.store, GH_STORE_BACKEND_NIP17, key, &conversation,
                                           &error));
  g_assert_no_error(error);
  g_assert_cmpint(conversation, ==, gh_outbox_item_get_conversation_id(item));

  /* The text bound counts the room's "p" tags. */
  gsize long_length = 40960 - 700;
  g_autofree gchar *text = g_strnfill(long_length, 'a');
  g_assert_true(gh_outbox_text_fits(f.outbox, hex[KEY_BOB], text));
  g_assert_false(gh_outbox_text_fits_room(f.outbox, eleven, text));
  fixture_down(&f);
}

/* A block works in a room as in a one-to-one conversation: a room message
 * after the block is kept but hidden, and writing to the room (T-enqueue)
 * lifts the block; the local echo lands in the same room the outbox
 * stored, so replies are listed again. */
static void
test_room_block(void)
{
  Fixture f;
  fixture_up(&f);
  f.network->available = FALSE; /* only T-enqueue matters here */
  fixture_outbox(&f);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhConversationStore) model = gh_conversation_store_new();
  g_autoptr(GhStoreConversations) conversations = gh_store_conversations_new(f.store);
  g_assert_true(gh_store_conversations_attach(conversations, model, 0, &error));
  g_assert_no_error(error);

  const gchar *const to_alice_carol[] = { hex[KEY_ALICE], hex[KEY_CAROL], NULL };
  g_autofree gchar *hello = gh_nip17_rumor_new_room(hex[KEY_BOB], to_alice_carol, "hello all",
                                                    T0 - 100, 0, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) first = gh_message_new_from_rumor(hex[KEY_ALICE], hello, &error);
  g_assert_no_error(error);
  g_autofree gchar *wrap_1 = hex_of("wrap 1");
  g_assert_cmpint(gh_conversation_store_admit(model, first, wrap_1, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  const gchar *room_id = gh_message_get_room_id(first);
  g_assert_true(gh_store_conversations_set_blocked(conversations, room_id, TRUE, &error));
  g_assert_no_error(error);
  g_assert_null(gh_conversation_store_lookup(model, room_id));

  /* Carol writes in the blocked room: kept, not listed. */
  const gchar *const to_alice_bob[] = { hex[KEY_ALICE], hex[KEY_BOB], NULL };
  g_autofree gchar *later = gh_nip17_rumor_new_room(hex[KEY_CAROL], to_alice_bob, "still there?",
                                                    T0 - 50, 0, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) blocked = gh_message_new_from_rumor(hex[KEY_ALICE], later, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(gh_message_get_room_id(blocked), ==, room_id);
  g_autofree gchar *wrap_2 = hex_of("wrap 2");
  g_assert_cmpint(gh_conversation_store_admit(model, blocked, wrap_2, &error), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  gboolean is_blocked = FALSE;
  g_assert_true(gh_store_conversations_is_blocked(conversations, room_id, &is_blocked, &error));
  g_assert_true(is_blocked);

  /* Writing to the room lifts it; the echo is this room's. */
  g_autoptr(GhOutboxItem) item = send_room(&f, "sorry, I'm back", hex[KEY_CAROL], hex[KEY_BOB],
                                           NULL);
  g_assert_true(gh_store_conversations_is_blocked(conversations, room_id, &is_blocked, &error));
  g_assert_false(is_blocked);
  g_autoptr(GhMessage) echo = gh_message_new_from_rumor(hex[KEY_ALICE],
                                                        gh_outbox_item_get_rumor_json(item),
                                                        &error);
  g_assert_no_error(error);
  g_assert_cmpstr(gh_message_get_room_id(echo), ==, room_id);
  g_assert_cmpint(gh_conversation_store_add_message(model, echo, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  GhConversation *room = gh_conversation_store_lookup(model, room_id);
  g_assert_nonnull(room);
  g_autofree gchar *wrap_3 = hex_of("wrap 3");
  g_autofree gchar *reply = gh_nip17_rumor_new_room(hex[KEY_BOB], to_alice_carol, "welcome back",
                                                    T0 + 10, 0, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) welcome = gh_message_new_from_rumor(hex[KEY_ALICE], reply, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_conversation_store_admit(model, welcome, wrap_3, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  gh_store_conversations_close(conversations);
  fixture_down(&f);
}

#ifdef GROUNDHOG_TEST_WIRE
/* ---- over real relays -------------------------------------------------------- */

static void
store_relay(WireRelay *relay)
{
  relay->serve = TRUE;
  relay_init(relay);
}

/* The kind-1059 events a relay keeps for p (every one when p is NULL). */
static GPtrArray *
wraps_on(WireRelay *relay, const gchar *p)
{
  GPtrArray *wraps = g_ptr_array_new();
  for (guint i = 0; relay->stored && i < relay->stored->len; i++) {
    WireStored *stored = g_ptr_array_index(relay->stored, i);
    if (nostr_event_get_kind(stored->event) != 1059)
      continue;
    char *to = nostr_nip59_get_recipient(stored->event);
    if (!p || g_strcmp0(to, p) == 0)
      g_ptr_array_add(wraps, stored);
    free(to);
  }
  return wraps;
}

static WireStored *
only_wrap(WireRelay *relay, const gchar *p)
{
  g_autoptr(GPtrArray) wraps = wraps_on(relay, p);
  g_assert_cmpuint(wraps->len, ==, 1);
  return g_ptr_array_index(wraps, 0);
}

/* The rumor JSON a wrap carries for key, or NULL when key cannot open it:
 * the wrap (NIP-59) and then the seal (NIP-44 from Alice). The seal is
 * returned too when asked for. */
static gchar *
open_wrap(NostrEvent *wrap, guint key, NostrEvent **out_seal)
{
  NostrEvent *seal = nostr_nip59_unwrap(wrap, gh_test_secret[key]);
  if (!seal)
    return NULL;
  g_assert_cmpint(nostr_event_get_kind(seal), ==, 13);
  g_assert_cmpstr(nostr_event_get_pubkey(seal), ==, hex[KEY_ALICE]);
  g_assert_cmpint(nostr_event_validate(seal, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[key], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, hex[KEY_ALICE], sizeof pk));
  guint8 *plaintext = NULL;
  size_t length = 0;
  g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, nostr_event_get_content(seal), &plaintext,
                                         &length), ==, 0);
  gchar *rumor = g_strndup((const gchar *) plaintext, length);
  free(plaintext);
  if (out_seal)
    *out_seal = seal;
  else
    nostr_event_free(seal);
  return rumor;
}

/* Only receiver can open wrap, and it holds exactly the queued rumor. */
static NostrEvent *
assert_only_opens(NostrEvent *wrap, guint receiver, const gchar *rumor_json)
{
  const guint keys[] = { KEY_ALICE, KEY_BOB, KEY_CAROL, KEY_DAVE };
  NostrEvent *seal = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
    NostrEvent *opened_seal = NULL;
    g_autofree gchar *rumor = open_wrap(wrap, keys[i], &opened_seal);
    if (keys[i] != receiver) {
      g_assert_null(rumor);
      continue;
    }
    g_assert_nonnull(rumor);
    g_assert_cmpstr(rumor, ==, rumor_json); /* the stored rumor, byte for byte */
    seal = opened_seal;
  }
  return seal;
}

/* Three accounts over real relays. Bob's and Carol's lists share a relay,
 * which demands AUTH to write: each wrap reaches it on its own connection
 * after its own ephemeral AUTH. The own relay also demands AUTH: the
 * self-copy signs in as the account (the only place it does). Each receiver
 * opens only its own wrap; every wrap carries the one rumor; no two wraps
 * share an outer key or a created_at, nor two seals a created_at. */
static void
test_wire_room(void)
{
  WireRelay bob_only = { 0 }, carol_only = { 0 }, shared = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&carol_only);
  shared.auth_writes = TRUE;
  shared.record = TRUE;
  store_relay(&shared);
  own.auth_writes = TRUE;
  store_relay(&own);

  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, shared.url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_only.url, shared.url, NULL);
  settle_own(&f, own.url, NULL);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_room(&f, "Hi both", hex[KEY_BOB], hex[KEY_CAROL], NULL);
  wait_until(&f, item, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_SETTLED);
  g_assert_cmpint(state_of(item, KEY_BOB), ==, GH_OUTBOX_RECIPIENT_SENT);
  g_assert_cmpint(state_of(item, KEY_CAROL), ==, GH_OUTBOX_RECIPIENT_SENT);
  g_assert_false(gh_outbox_item_get_self_copy_missing(item));
  g_assert_nonnull(strstr(gh_outbox_item_get_detail(item), "each person"));

  /* Each wrap only where its receiver's list says. */
  const gchar *rumor = gh_outbox_item_get_rumor_json(item);
  WireStored *to_bob = only_wrap(&bob_only, hex[KEY_BOB]);
  WireStored *to_carol = only_wrap(&carol_only, hex[KEY_CAROL]);
  WireStored *to_self = only_wrap(&own, hex[KEY_ALICE]);
  g_assert_cmpuint(bob_only.stored->len, ==, 1);
  g_assert_cmpuint(carol_only.stored->len, ==, 1);
  g_assert_cmpuint(own.stored->len, ==, 1);
  /* The shared relay has both, separately: the same wraps, one each. */
  g_assert_cmpuint(shared.stored->len, ==, 2);
  g_assert_cmpstr(only_wrap(&shared, hex[KEY_BOB])->id, ==, to_bob->id);
  g_assert_cmpstr(only_wrap(&shared, hex[KEY_CAROL])->id, ==, to_carol->id);
  /* Each connection carried one wrap (sent again after its AUTH), and the
   * two wraps came on two connections. */
  g_autoptr(GHashTable) carried = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                                        g_free);
  for (guint i = 0; i < shared.frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(shared.frames, i);
    gchar *id = frame->inbound ? wire_event_frame_id(frame->text) : NULL;
    if (!id)
      continue;
    const gchar *known = g_hash_table_lookup(carried, GUINT_TO_POINTER(frame->connection));
    if (known) {
      g_assert_cmpstr(known, ==, id);
      g_free(id);
    } else {
      g_hash_table_insert(carried, GUINT_TO_POINTER(frame->connection), id);
    }
  }
  g_assert_cmpuint(g_hash_table_size(carried), ==, 2);
  /* One ephemeral AUTH per connection: neither the account nor each other. */
  g_assert_cmpuint(shared.auth_pubkeys->len, ==, 2);
  const gchar *auth_a = g_ptr_array_index(shared.auth_pubkeys, 0);
  const gchar *auth_b = g_ptr_array_index(shared.auth_pubkeys, 1);
  g_assert_cmpstr(auth_a, !=, auth_b);
  g_assert_cmpstr(auth_a, !=, hex[KEY_ALICE]);
  g_assert_cmpstr(auth_b, !=, hex[KEY_ALICE]);
  /* The self-copy signed in as the account, on the account's own relay. */
  g_assert_cmpuint(own.auth_pubkeys->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(own.auth_pubkeys, 0), ==, hex[KEY_ALICE]);
  g_assert_cmpuint(bob_only.auth_pubkeys->len + carol_only.auth_pubkeys->len, ==, 0);

  /* Each receiver opens only its own wrap, which holds the queued rumor. */
  NostrEvent *seals[3] = {
    assert_only_opens(to_bob->event, KEY_BOB, rumor),
    assert_only_opens(to_carol->event, KEY_CAROL, rumor),
    assert_only_opens(to_self->event, KEY_ALICE, rumor),
  };
  g_auto(GStrv) p = rumor_p_tags(rumor);
  const gchar *const room[] = { hex[KEY_BOB], hex[KEY_CAROL], NULL };
  g_assert_cmpstrv(p, room);

  /* Unlinkable (NIP-59): distinct outer keys, none the account's; distinct
   * wrap and seal times. */
  WireStored *wraps[3] = { to_bob, to_carol, to_self };
  for (guint i = 0; i < 3; i++) {
    g_assert_cmpstr(nostr_event_get_pubkey(wraps[i]->event), !=, hex[KEY_ALICE]);
    g_assert_cmpstr(nostr_event_get_pubkey(wraps[i]->event), !=, auth_a);
    g_assert_cmpstr(nostr_event_get_pubkey(wraps[i]->event), !=, auth_b);
    g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(seals[i])), ==, 0);
    for (guint j = i + 1; j < 3; j++) {
      g_assert_cmpstr(nostr_event_get_pubkey(wraps[i]->event), !=,
                      nostr_event_get_pubkey(wraps[j]->event));
      g_assert_cmpint(nostr_event_get_created_at(wraps[i]->event), !=,
                      nostr_event_get_created_at(wraps[j]->event));
      g_assert_cmpint(nostr_event_get_created_at(seals[i]), !=,
                      nostr_event_get_created_at(seals[j]));
    }
  }
  for (guint i = 0; i < 3; i++)
    nostr_event_free(seals[i]);

  /* Durable per recipient: one stored wrap each, with its own targets. */
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, item);
  g_assert_cmpuint(entry->events->len, ==, 3);
  g_assert_cmpstr(entry_wrap_for(entry, hex[KEY_BOB], GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->event_id,
                  ==, to_bob->id);
  g_assert_cmpuint(entry_wrap_for(entry, hex[KEY_CAROL],
                                  GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->targets->len, ==, 2);
  g_assert_cmpstr(entry_wrap_for(entry, hex[KEY_ALICE], GH_STORE_OUTBOX_ROLE_SELF_WRAP)->event_id,
                  ==, to_self->id);
  /* Signer: encrypt + seal per wrap, and the one account AUTH. */
  g_assert_cmpuint(f.mock.calls, ==, 3 * 2 + 1);
  fixture_down(&f);
  WireRelay *relays[] = { &bob_only, &carol_only, &shared, &own };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    relay_clear(relays[i]);
}

/* Partial failure, durability and retry per recipient: Carol's relay is
 * down, so the message is "Sent to some people" and retries; after a
 * restart (a new outbox over the same store) with her relay back, only her
 * stored wrap is published, never re-sealed, and Bob's is not sent again. */
static void
test_wire_partial_retry(void)
{
  WireRelay bob_only = { 0 }, carol_later = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&own);
  guint16 port = 0;
  g_autofree gchar *carol_url = unused_relay_url(&port);

  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_url, NULL);
  settle_own(&f, own.url, NULL);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_room(&f, "Anyone there?", hex[KEY_BOB], hex[KEY_CAROL],
                                           NULL);
  wait_until(&f, item, GH_MESSAGE_STATUS_PARTIALLY_SENT);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_WAITING_RETRY);
  g_assert_cmpint(state_of(item, KEY_BOB), ==, GH_OUTBOX_RECIPIENT_SENT);
  g_assert_cmpint(state_of(item, KEY_CAROL), ==, GH_OUTBOX_RECIPIENT_RETRYING);
  g_assert_cmpstr(gh_outbox_item_get_label(item), ==, "Sent to some people");
  g_assert_cmpstr(gh_outbox_item_get_detail(item), ==, "Accepted for 1 of 2 recipients.");
  g_assert_cmpint(gh_outbox_item_get_next_attempt_at(item), >, T0);
  g_assert_true(gh_outbox_item_get_can_retry(item));
  g_assert_cmpuint(bob_only.events, ==, 1);
  guint calls = f.mock.calls;
  g_autoptr(GhStoreOutboxEntry) before = load_entry(&f, item);
  g_autofree gchar *carol_wrap =
    g_strdup(entry_wrap_for(before, hex[KEY_CAROL], GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->event_id);
  gint64 outbox_id = gh_outbox_item_get_outbox_id(item);

  /* A restart: the state is the store's. Carol's relay is back. */
  gh_test_release(f.outbox);
  f.outbox = NULL;
  carol_later.url = g_strdup(carol_url);
  carol_later.serve = TRUE;
  relay_init_port(&carol_later, port);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) resumed = gh_outbox_lookup(f.outbox, outbox_id);
  g_assert_nonnull(resumed);
  g_assert_cmpint(gh_outbox_item_get_status(resumed), ==, GH_MESSAGE_STATUS_PARTIALLY_SENT);
  advance_to_next(&f); /* its retry is due */
  wait_until(&f, resumed, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpint(gh_outbox_item_get_state(resumed), ==, GH_STORE_OUTBOX_SETTLED);
  g_assert_cmpstr(only_wrap(&carol_later, hex[KEY_CAROL])->id, ==, carol_wrap);
  g_assert_cmpuint(bob_only.events, ==, 1); /* accepted targets are never republished */
  g_assert_cmpuint(own.events, ==, 1);
  g_assert_cmpuint(f.mock.calls, ==, calls); /* never re-sealed */
  fixture_down(&f);
  relay_clear(&bob_only);
  relay_clear(&carol_later);
  relay_clear(&own);
}

/* Carol has no 10050: her wrap is sealed and stored with no target, and
 * nothing is published for her on any relay (not Bob's, not the account's
 * own). The message is "Sent to some people" and waits for the user, with
 * Carol's state "no inbox". Once she sets one up, Retry publishes the same
 * stored wrap to it, without the signer. */
static void
test_wire_missing_inbox(void)
{
  WireRelay bob_only = { 0 }, carol_new = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&carol_new);
  store_relay(&own);

  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, NULL);
  settle_own(&f, own.url, NULL); /* Carol: NOT_FOUND */
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_room(&f, "Carol, set up your inbox", hex[KEY_BOB],
                                           hex[KEY_CAROL], NULL);
  wait_until(&f, item, GH_MESSAGE_STATUS_PARTIALLY_SENT);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_NEEDS_ATTENTION);
  g_assert_true(gh_outbox_item_get_can_retry(item));
  g_autoptr(GPtrArray) recipients = gh_outbox_item_dup_recipients(item);
  const GhOutboxRecipient *carol = recipient_in(recipients, hex[KEY_CAROL]);
  g_assert_cmpint(carol->state, ==, GH_OUTBOX_RECIPIENT_NO_INBOX);
  g_assert_cmpuint(carol->relays, ==, 0);
  const GhOutboxRecipient *bob = recipient_in(recipients, hex[KEY_BOB]);
  g_assert_cmpint(bob->state, ==, GH_OUTBOX_RECIPIENT_SENT);
  g_assert_cmpuint(bob->accepted, ==, 1);
  g_assert_cmpuint(bob->relays, ==, 1);
  /* Nothing for Carol anywhere. */
  g_autoptr(GPtrArray) bob_carol = wraps_on(&bob_only, hex[KEY_CAROL]);
  g_autoptr(GPtrArray) own_carol = wraps_on(&own, hex[KEY_CAROL]);
  g_assert_cmpuint(bob_carol->len + own_carol->len + carol_new.events, ==, 0);
  g_assert_cmpuint(bob_only.events, ==, 1);
  g_assert_cmpuint(own.events, ==, 1);
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, item);
  GhStoreOutboxEvent *carol_event =
    entry_wrap_for(entry, hex[KEY_CAROL], GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  g_assert_cmpuint(carol_event->targets->len, ==, 0);
  guint calls = f.mock.calls;
  g_assert_cmpuint(calls, ==, 3 * 2);

  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_new.url, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_outbox_retry(f.outbox, gh_outbox_item_get_outbox_id(item), &error));
  g_assert_no_error(error);
  wait_until(&f, item, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpstr(only_wrap(&carol_new, hex[KEY_CAROL])->id, ==, carol_event->event_id);
  g_assert_cmpint(state_of(item, KEY_CAROL), ==, GH_OUTBOX_RECIPIENT_SENT);
  g_assert_cmpuint(bob_only.events, ==, 1);
  g_assert_cmpuint(f.mock.calls, ==, calls);
  fixture_down(&f);
  relay_clear(&bob_only);
  relay_clear(&carol_new);
  relay_clear(&own);
}

/* nostrc-9cho: what the lookups found about recipients without a 10050
 * outlives a restart. In a message that needs attention Carol stays "no
 * inbox" (not "not sent"): her stored wrap without targets is marked so;
 * and when nobody had one (nothing sealed) every recipient stays "no inbox".
 * Once Carol sets one up, Retry reaches her and the mark goes. */
static void
test_wire_no_inbox_restart(void)
{
  WireRelay bob_only = { 0 }, carol_new = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&carol_new);
  store_relay(&own);
  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, NULL);
  settle_own(&f, own.url, NULL); /* Carol and Dave: NOT_FOUND */
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_room(&f, "Carol has none", hex[KEY_BOB], hex[KEY_CAROL],
                                           NULL);
  wait_until(&f, item, GH_MESSAGE_STATUS_PARTIALLY_SENT);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_NEEDS_ATTENTION);
  g_assert_cmpint(state_of(item, KEY_CAROL), ==, GH_OUTBOX_RECIPIENT_NO_INBOX);
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, item);
  g_assert_true(entry_wrap_for(entry, hex[KEY_CAROL], GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->no_inbox);
  g_assert_false(entry_wrap_for(entry, hex[KEY_BOB], GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->no_inbox);
  g_autofree gchar *stranger = hex_of("stranger");
  g_autoptr(GhOutboxItem) nobody = send_room(&f, "Anyone?", hex[KEY_DAVE], stranger, NULL);
  wait_until(&f, nobody, GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX);
  gint64 item_id = gh_outbox_item_get_outbox_id(item);
  gint64 nobody_id = gh_outbox_item_get_outbox_id(nobody);

  /* A restart: a new outbox over the same store. */
  gh_test_release(f.outbox);
  f.outbox = NULL;
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) resumed = gh_outbox_lookup(f.outbox, item_id);
  g_assert_nonnull(resumed);
  g_assert_cmpint(gh_outbox_item_get_status(resumed), ==, GH_MESSAGE_STATUS_PARTIALLY_SENT);
  g_assert_cmpint(state_of(resumed, KEY_CAROL), ==, GH_OUTBOX_RECIPIENT_NO_INBOX);
  g_assert_cmpint(state_of(resumed, KEY_BOB), ==, GH_OUTBOX_RECIPIENT_SENT);
  g_autoptr(GhOutboxItem) resumed_nobody = gh_outbox_lookup(f.outbox, nobody_id);
  g_assert_nonnull(resumed_nobody);
  g_assert_cmpint(gh_outbox_item_get_status(resumed_nobody), ==,
                  GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX);
  g_assert_cmpint(state_of(resumed_nobody, KEY_DAVE), ==, GH_OUTBOX_RECIPIENT_NO_INBOX);
  /* nostrc-lff5: its accessible description speaks of everyone. */
  g_assert_cmpstr(gh_outbox_item_get_accessible_description(resumed_nobody), ==,
                  "Can't send. No one in this conversation has set up private messaging yet.");
  g_autoptr(GPtrArray) strangers = gh_outbox_item_dup_recipients(resumed_nobody);
  g_assert_cmpint(recipient_in(strangers, stranger)->state, ==, GH_OUTBOX_RECIPIENT_NO_INBOX);

  /* Carol sets one up: Retry publishes her stored wrap and drops the mark. */
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_new.url, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_outbox_retry(f.outbox, item_id, &error));
  g_assert_no_error(error);
  wait_until(&f, resumed, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpint(state_of(resumed, KEY_CAROL), ==, GH_OUTBOX_RECIPIENT_SENT);
  g_autoptr(GhStoreOutboxEntry) after = load_entry(&f, resumed);
  g_assert_false(entry_wrap_for(after, hex[KEY_CAROL], GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->no_inbox);
  g_assert_cmpuint(carol_new.events, ==, 1);
  fixture_down(&f);
  relay_clear(&bob_only);
  relay_clear(&carol_new);
  relay_clear(&own);
}

/* A lookup that failed is not "no inbox": Carol's wrap waits, the message
 * retries on its own, and the next round finds her list. When nobody can
 * be reached nothing is sealed at all: "Can't send" (no list anywhere, no
 * signer call) or a retried lookup (none could be asked). */
static void
test_wire_lookup_and_nobody(void)
{
  WireRelay bob_only = { 0 }, carol_found = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&carol_found);
  store_relay(&own);

  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_UNREACHABLE);
  settle_own(&f, own.url, NULL);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_room(&f, "Lookups", hex[KEY_BOB], hex[KEY_CAROL], NULL);
  wait_until(&f, item, GH_MESSAGE_STATUS_PARTIALLY_SENT);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_WAITING_RETRY);
  g_assert_cmpint(state_of(item, KEY_CAROL), ==, GH_OUTBOX_RECIPIENT_RETRYING);
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_found.url, NULL);
  advance_to_next(&f);
  wait_until(&f, item, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(carol_found.events, ==, 1);
  g_assert_cmpuint(bob_only.events, ==, 1);

  /* Nobody has a list: nothing sealed, nothing signed, nothing sent. */
  guint calls = f.mock.calls;
  g_autofree gchar *stranger = hex_of("stranger");
  g_autoptr(GhOutboxItem) nobody = send_room(&f, "Hello?", hex[KEY_DAVE], stranger, NULL);
  wait_until(&f, nobody, GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX);
  g_assert_cmpuint(f.mock.calls, ==, calls);
  g_assert_cmpint(state_of(nobody, KEY_DAVE), ==, GH_OUTBOX_RECIPIENT_NO_INBOX);
  g_assert_nonnull(strstr(gh_outbox_item_get_detail(nobody), "No one in this conversation"));

  /* One unknown, one without: the lookup is tried again later. */
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_UNREACHABLE);
  g_autoptr(GhOutboxItem) unknown = send_room(&f, "Later", hex[KEY_DAVE], hex[KEY_CAROL], NULL);
  wait_status(unknown, GH_MESSAGE_STATUS_RETRYING); /* unsealed: its lookup waits */
  g_assert_cmpint(gh_outbox_item_get_state(unknown), ==, GH_STORE_OUTBOX_QUEUED);
  g_assert_cmpint(gh_outbox_item_get_next_attempt_at(unknown), >, 0);
  g_assert_cmpuint(f.mock.calls, ==, calls);
  g_assert_cmpuint(own.events, ==, 1);
  fixture_down(&f);
  relay_clear(&bob_only);
  relay_clear(&carol_found);
  relay_clear(&own);
}

/* The disappearing timer (charter §3.7, G07) in a room: the rumor carries
 * the exact expiration; every seal and every wrap (each recipient's and
 * the self-copy) carries its own, never earlier, a whole hour, within the
 * jitter bound. */
static void
test_wire_disappearing(void)
{
  WireRelay bob_only = { 0 }, carol_only = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&carol_only);
  store_relay(&own);

  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_only.url, NULL);
  settle_own(&f, own.url, NULL);
  fixture_outbox(&f);
  const gchar *const members[] = { hex[KEY_ALICE], hex[KEY_BOB], hex[KEY_CAROL], NULL };
  g_autofree gchar *key = room_key(members);
  gint64 conversation = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_ensure_conversation(f.store, GH_STORE_BACKEND_NIP17, key,
                                             GH_STORE_REQUEST_ACCEPTED, &conversation, &error));
  const gint64 day = 86400;
  g_assert_true(gh_store_set_disappearing(f.store, conversation, day, &error));
  g_assert_no_error(error);

  g_autoptr(GhOutboxItem) item = send_room(&f, "gone tomorrow", hex[KEY_BOB], hex[KEY_CAROL],
                                           NULL);
  wait_until(&f, item, GH_MESSAGE_STATUS_SENT);
  const gchar *rumor = gh_outbox_item_get_rumor_json(item);
  gint64 created_at = 0, expires_at = 0;
  g_assert_true(gh_nip17_rumor_get_expiration(rumor, &created_at, &expires_at));
  g_assert_cmpint(created_at, ==, T0);
  g_assert_cmpint(expires_at, ==, T0 + day);
  struct { WireRelay *relay; guint key; } legs[] = {
    { &bob_only, KEY_BOB }, { &carol_only, KEY_CAROL }, { &own, KEY_ALICE },
  };
  for (guint i = 0; i < G_N_ELEMENTS(legs); i++) {
    WireStored *wrap = only_wrap(legs[i].relay, hex[legs[i].key]);
    NostrEvent *seal = NULL;
    g_autofree gchar *inside = open_wrap(wrap->event, legs[i].key, &seal);
    g_assert_cmpstr(inside, ==, rumor);
    gint64 layers[] = { expiration_of(wrap->event), expiration_of(seal) };
    for (guint j = 0; j < G_N_ELEMENTS(layers); j++) {
      g_assert_cmpint(layers[j], >=, expires_at);
      g_assert_cmpint(layers[j], <=, expires_at + day + 3600);
      g_assert_cmpint(layers[j] % 3600, ==, 0);
    }
    g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(seal)), ==, 1); /* expiration only */
    nostr_event_free(seal);
  }
  fixture_down(&f);
  relay_clear(&bob_only);
  relay_clear(&carol_only);
  relay_clear(&own);
}

/* §4.5 S4: a room's wraps go out in a random order, U(0, 3) s apart. With
 * scripted draws the order is Carol, Dave (+2 s), Bob (+5 s): each relay
 * gets its wrap only when the fake clock reaches its time. */
static void
test_wire_spacing(void)
{
  WireRelay bob_only = { 0 }, carol_only = { 0 }, dave_only = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&carol_only);
  store_relay(&dave_only);
  store_relay(&own);

  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_only.url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_DAVE], GH_INBOX_FOUND, dave_only.url, NULL);
  settle_own(&f, own.url, NULL);
  fixture_outbox(&f);
  /* The shuffle (Fisher-Yates from the end: j = 0, then 0) and two gaps. */
  gh_clock_fake_push_random(f.clock, 0);
  gh_clock_fake_push_random(f.clock, 0);
  gh_clock_fake_push_random(f.clock, 2);
  gh_clock_fake_push_random(f.clock, 3);
  g_autoptr(GhOutboxItem) item = send_room(&f, "one at a time", hex[KEY_BOB], hex[KEY_CAROL],
                                           hex[KEY_DAVE], NULL);
  wait_for_count(&carol_only.events, 1);
  wait_for_count(&own.events, 1);
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, item);
  g_assert_cmpint(entry_wrap_for(entry, hex[KEY_CAROL],
                                 GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->not_before, ==, 0);
  g_assert_cmpint(entry_wrap_for(entry, hex[KEY_DAVE],
                                 GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->not_before, ==, T0 + 2);
  g_assert_cmpint(entry_wrap_for(entry, hex[KEY_BOB],
                                 GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->not_before, ==, T0 + 5);
  drain();
  g_assert_cmpuint(dave_only.events + bob_only.events, ==, 0);
  wait_status(item, GH_MESSAGE_STATUS_PARTIALLY_SENT); /* Carol's OK, as the client reads it */
  g_assert_cmpuint(dave_only.events + bob_only.events, ==, 0);
  gh_clock_fake_advance(f.clock, 2 * G_USEC_PER_SEC);
  wait_for_count(&dave_only.events, 1);
  drain();
  g_assert_cmpuint(bob_only.events, ==, 0);
  gh_clock_fake_advance(f.clock, 3 * G_USEC_PER_SEC);
  wait_for_count(&bob_only.events, 1);
  wait_until(&f, item, GH_MESSAGE_STATUS_SENT);
  fixture_down(&f);
  WireRelay *relays[] = { &bob_only, &carol_only, &dave_only, &own };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    relay_clear(relays[i]);
}

/* nostrc-yp69: S4 on every round, not only the first. All three
 * recipients' relays are down for the first round (which runs on the
 * spacing drawn at T-seal, all at once here); for the retry they are back,
 * and the round draws its own random order and spacing in memory: with
 * scripted draws Dave at once, Bob 1 s later, Carol 2 s after him, never all
 * three back to back in "p" order. The stored not_before stays as sealed. */
static void
test_wire_retry_spacing(void)
{
  guint16 ports[3] = { 0 };
  g_autofree gchar *bob_url = unused_relay_url(&ports[0]);
  g_autofree gchar *carol_url = unused_relay_url(&ports[1]);
  g_autofree gchar *dave_url = unused_relay_url(&ports[2]);
  WireRelay bob_back = { 0 }, carol_back = { 0 }, dave_back = { 0 }, own = { 0 };
  store_relay(&own);

  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_DAVE], GH_INBOX_FOUND, dave_url, NULL);
  settle_own(&f, own.url, NULL);
  fixture_outbox(&f);
  /* T-seal's S4 draws: no reordering, no gaps. */
  for (guint i = 0; i < 4; i++)
    gh_clock_fake_push_random(f.clock, 0);
  g_autoptr(GhOutboxItem) item = send_room(&f, "spaced again", hex[KEY_BOB], hex[KEY_CAROL],
                                           hex[KEY_DAVE], NULL);
  wait_until(&f, item, GH_MESSAGE_STATUS_RETRYING);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_WAITING_RETRY);
  g_autoptr(GhStoreOutboxEntry) sealed = load_entry(&f, item);

  WireRelay *back[] = { &bob_back, &carol_back, &dave_back };
  const gchar *urls[] = { bob_url, carol_url, dave_url };
  for (guint i = 0; i < G_N_ELEMENTS(back); i++) {
    back[i]->url = g_strdup(urls[i]);
    back[i]->serve = TRUE;
    relay_init_port(back[i], ports[i]);
  }
  /* The retry round's draws: the shuffle of Bob, Carol, Dave (Fisher-Yates
   * from the end: j = 1, then 0: Dave, Bob, Carol) and two gaps. */
  const guint32 draws[] = { 1, 0, 1, 2 };
  for (guint i = 0; i < G_N_ELEMENTS(draws); i++)
    gh_clock_fake_push_random(f.clock, draws[i]);
  advance_to_next(&f); /* the retry is due */
  wait_for_count(&dave_back.events, 1);
  drain();
  g_assert_cmpuint(bob_back.events + carol_back.events, ==, 0);
  gh_clock_fake_advance(f.clock, 1 * G_USEC_PER_SEC);
  wait_for_count(&bob_back.events, 1);
  drain();
  g_assert_cmpuint(carol_back.events, ==, 0);
  gh_clock_fake_advance(f.clock, 2 * G_USEC_PER_SEC);
  wait_for_count(&carol_back.events, 1);
  wait_until(&f, item, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_SETTLED);
  /* In memory only: what T-seal stored is unchanged. */
  g_autoptr(GhStoreOutboxEntry) settled = load_entry(&f, item);
  const guint keys[] = { KEY_BOB, KEY_CAROL, KEY_DAVE };
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++)
    g_assert_cmpint(entry_wrap_for(settled, hex[keys[i]],
                                   GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->not_before, ==,
                    entry_wrap_for(sealed, hex[keys[i]],
                                   GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->not_before);
  g_assert_cmpuint(own.events, ==, 1);
  fixture_down(&f);
  WireRelay *relays[] = { &bob_back, &carol_back, &dave_back, &own };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    relay_clear(relays[i]);
}

/* ---- GhDmSender directly ------------------------------------------------------ */

typedef struct {
  GhDmSend *send;
} DoneWait;

static gboolean
send_done(gpointer data)
{
  return gh_dm_send_is_done(((DoneWait *) data)->send);
}

static const GhDmSendStatus *
send_and_wait(Fixture *f, GhDmSend **out, const gchar *const *recipients, const gchar *text)
{
  *out = gh_dm_sender_send_room(f->sender, recipients, text, NULL);
  DoneWait wait = { *out };
  gh_test_spin_until(send_done, &wait);
  drain();
  return gh_dm_send_get_status(*out);
}

/* The pipeline itself: one wrap per recipient, a recipient without a list
 * reported (FLAG_RECIPIENT_NO_INBOX, no targets, PARTIALLY_SENT), nobody
 * reachable before any signer call, and the limit. */
static void
test_wire_dm_sender(void)
{
  WireRelay bob_only = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&own);
  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, NULL);
  settle_own(&f, own.url, NULL);

  const gchar *const bob_carol[] = { hex[KEY_BOB], hex[KEY_CAROL], NULL };
  g_autoptr(GhDmSend) send = NULL;
  const GhDmSendStatus *status = send_and_wait(&f, &send, bob_carol, "direct");
  g_assert_cmpint(status->result, ==, GH_DM_SEND_RESULT_PARTIALLY_SENT);
  g_assert_true(status->flags & GH_DM_SEND_FLAG_RECIPIENT_NO_INBOX);
  g_assert_cmpuint(status->recipients->len, ==, 2);
  GhDmSendLeg *bob = g_ptr_array_index(status->recipients, 0);
  GhDmSendLeg *carol = g_ptr_array_index(status->recipients, 1);
  g_assert_cmpstr(bob->pubkey, ==, hex[KEY_BOB]);
  g_assert_cmpuint(bob->accepted, ==, 1);
  g_assert_cmpint(carol->inbox, ==, GH_INBOX_NOT_FOUND);
  g_assert_cmpuint(carol->relays->len, ==, 0);
  g_assert_nonnull(carol->wrap_json); /* sealed, never published */
  g_assert_cmpstr(carol->wrap_id, !=, bob->wrap_id);
  g_assert_cmpint(gh_dm_send_status_get_message_status(status), ==,
                  GH_MESSAGE_STATUS_PARTIALLY_SENT);
  g_assert_true(gh_dm_send_status_self_copy_stored(status));
  g_assert_cmpuint(bob_only.events, ==, 1);
  g_assert_cmpuint(own.events, ==, 1);

  /* Republishing the sealed status keeps Carol targetless. */
  g_autoptr(GhDmSend) again = gh_dm_sender_publish(f.sender, status, NULL);
  DoneWait wait = { again };
  gh_test_spin_until(send_done, &wait);
  g_assert_cmpint(gh_dm_send_get_status(again)->result, ==, GH_DM_SEND_RESULT_PARTIALLY_SENT);

  guint calls = f.mock.calls;
  const gchar *const nobody[] = { hex[KEY_CAROL], hex[KEY_DAVE], NULL };
  g_autoptr(GhDmSend) none = NULL;
  g_assert_cmpint(send_and_wait(&f, &none, nobody, "hello?")->result, ==,
                  GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX);
  fake_resolver_set(f.resolver, hex[KEY_DAVE], GH_INBOX_UNREACHABLE);
  g_autoptr(GhDmSend) unknown = NULL;
  g_assert_cmpint(send_and_wait(&f, &unknown, nobody, "hello?")->result, ==,
                  GH_DM_SEND_RESULT_INBOX_UNKNOWN);
  g_assert_cmpuint(f.mock.calls, ==, calls);

  g_autoptr(GPtrArray) eleven = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i <= GH_NIP17_MAX_SEND_RECIPIENTS; i++)
    g_ptr_array_add(eleven, numbered_key("many", i));
  g_ptr_array_add(eleven, NULL);
  g_autoptr(GhDmSend) too_many =
    gh_dm_sender_send_room(f.sender, (const gchar *const *) eleven->pdata, "x", NULL);
  g_assert_true(gh_dm_send_is_done(too_many));
  g_assert_cmpint(gh_dm_send_get_status(too_many)->failure, ==, GH_DM_SEND_FAILURE_INVALID);
  fixture_down(&f);
  relay_clear(&bob_only);
  relay_clear(&own);
}

/* nostrc-yp69, the direct path: gh_dm_sender_send_room() spaces a room's
 * wraps too, on the sender's clock. With scripted draws Carol goes at once
 * and Bob 2 s later; the self-copy goes at once. */
static void
test_wire_dm_sender_spacing(void)
{
  WireRelay bob_only = { 0 }, carol_only = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&carol_only);
  store_relay(&own);
  Fixture f;
  fixture_up(&f);
  gh_dm_sender_set_clock(f.sender, f.clock);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_only.url, NULL);
  settle_own(&f, own.url, NULL);
  /* The shuffle of Bob, Carol (j = 0: Carol first) and the gap. */
  gh_clock_fake_push_random(f.clock, 0);
  gh_clock_fake_push_random(f.clock, 2);
  const gchar *const bob_carol[] = { hex[KEY_BOB], hex[KEY_CAROL], NULL };
  g_autoptr(GhDmSend) send = gh_dm_sender_send_room(f.sender, bob_carol, "spaced directly",
                                                    NULL);
  wait_for_count(&carol_only.events, 1);
  wait_for_count(&own.events, 1);
  drain();
  g_assert_cmpuint(bob_only.events, ==, 0);
  g_assert_false(gh_dm_send_is_done(send));
  gh_clock_fake_advance(f.clock, 2 * G_USEC_PER_SEC);
  wait_for_count(&bob_only.events, 1);
  DoneWait wait = { send };
  gh_test_spin_until(send_done, &wait);
  g_assert_cmpint(gh_dm_send_get_status(send)->result, ==, GH_DM_SEND_RESULT_SENT);
  /* Cancelled while a wrap waits: nothing more goes out. */
  gh_clock_fake_push_random(f.clock, 0);
  gh_clock_fake_push_random(f.clock, 3);
  g_autoptr(GhDmSend) cancelled = gh_dm_sender_send_room(f.sender, bob_carol, "never both",
                                                         NULL);
  wait_for_count(&carol_only.events, 2);
  gh_dm_send_cancel(cancelled);
  gh_clock_fake_advance(f.clock, 3 * G_USEC_PER_SEC);
  drain();
  g_assert_cmpuint(bob_only.events, ==, 1);
  fixture_down(&f);
  relay_clear(&bob_only);
  relay_clear(&carol_only);
  relay_clear(&own);
}

#if GROUNDHOG_TEST_ATTACHMENTS
/* A small JPEG whose APP1 carries a GPS canary (stripped before sending)
 * and whose image data carries a marker that survives. */
static GBytes *
room_jpeg(void)
{
  GByteArray *out = g_byte_array_new();
  static const guint8 head[] = { 0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x18, 'E', 'x', 'i', 'f', 0, 0,
                                 'G', 'P', 'S', '-', 'R', 'O', 'O', 'M', '-', 'C', 'A', 'N',
                                 'A', 'R', 'Y', '!' };
  g_byte_array_append(out, head, sizeof head);
  static const guint8 sof[] = { 0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 0x10, 0x00, 0x10, 1, 1, 0x11,
                                0 };
  g_byte_array_append(out, sof, sizeof sof);
  static const guint8 sos[] = { 0xFF, 0xDA, 0x00, 0x08, 1, 1, 0, 0, 63, 0 };
  g_byte_array_append(out, sos, sizeof sos);
  for (guint i = 0; i < 64; i++)
    g_byte_array_append(out, (const guint8 *) "ROOM-FILE-PIXELS", 16);
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
  return ((Transfer *) data)->done;
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  (void) source;
  Transfer *t = data;
  t->file = gh_attachment_upload_finish(result, &t->error);
  t->done = TRUE;
}

static void
on_downloaded(GObject *source, GAsyncResult *result, gpointer data)
{
  (void) source;
  Transfer *t = data;
  t->bytes = gh_attachment_download_finish(result, NULL, &t->error);
  t->done = TRUE;
}

/* G21 in a room: Alice uploads one encrypted file to a local Blossom server
 * and sends it to Bob and Carol with gh_outbox_send_file_room(). It is sealed
 * and wrapped per recipient exactly like a text (one kind-15 rumor, one wrap
 * each on that recipient's relays only, the self-copy, per-recipient state
 * and "Sent"); nothing downloads it on the way. Each receiver opens only
 * their own wrap, reads the file message as the inbox would (GhMessage) and,
 * on their own Download, fetches, verifies and decrypts the same stripped
 * file with their own client. */
static void
test_wire_room_file(void)
{
  WireRelay bob_only = { 0 }, carol_only = { 0 }, own = { 0 };
  store_relay(&bob_only);
  store_relay(&carol_only);
  store_relay(&own);
  BlossomFixture *blossom = blossom_fixture_new();

  Fixture f;
  fixture_up(&f);
  fake_resolver_set(f.resolver, hex[KEY_BOB], GH_INBOX_FOUND, bob_only.url, NULL);
  fake_resolver_set(f.resolver, hex[KEY_CAROL], GH_INBOX_FOUND, carol_only.url, NULL);
  settle_own(&f, own.url, NULL);
  fixture_outbox(&f);
  g_settings_set_string(f.settings, "network-mode", "none"); /* the loopback server, directly */
  GhNetHttp *http = gh_net_http_new(f.settings);
  GhBlossomClient *alice_media = gh_blossom_client_new(NULL, http);
  const gchar *servers[] = { blossom_fixture_url(blossom), NULL };
  gh_blossom_client_set_servers(alice_media, servers);

  g_autoptr(GBytes) jpeg = room_jpeg();
  g_autoptr(GhAttachmentPrepared) stripped = gh_attachment_prepare(jpeg, NULL, 1 << 20, NULL);
  g_assert_nonnull(stripped);
  Transfer t = { 0 };
  gh_attachment_upload_async(alice_media, jpeg, NULL, NULL, on_uploaded, &t);
  gh_test_spin_until(transfer_done, &t);
  g_assert_no_error(t.error);
  g_autoptr(GhNip17File) sent = g_steal_pointer(&t.file);
  g_assert_cmpuint(blossom_fixture_count(blossom, "PUT"), ==, 1);

  const gchar *const room[] = { hex[KEY_BOB], hex[KEY_CAROL], NULL };
  g_autoptr(GError) error = NULL;
  g_autoptr(GhOutboxItem) item = gh_outbox_send_file_room(f.outbox, room, sent, &error);
  g_assert_no_error(error);
  wait_until(&f, item, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_SETTLED);
  g_assert_cmpint(state_of(item, KEY_BOB), ==, GH_OUTBOX_RECIPIENT_SENT);
  g_assert_cmpint(state_of(item, KEY_CAROL), ==, GH_OUTBOX_RECIPIENT_SENT);
  g_assert_false(gh_outbox_item_get_self_copy_missing(item));

  /* One kind-15 rumor for the room; the stored message is the file's. */
  const gchar *rumor = gh_outbox_item_get_rumor_json(item);
  g_assert_nonnull(strstr(rumor, "\"kind\":15"));
  g_auto(GStrv) p = rumor_p_tags(rumor);
  g_assert_cmpstrv(p, room);
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, item);
  g_assert_cmpuint(entry->events->len, ==, 3); /* Bob's, Carol's, the self-copy */
  g_assert_cmpstr(entry_wrap_for(entry, hex[KEY_BOB], GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)
                    ->event_id, ==, only_wrap(&bob_only, hex[KEY_BOB])->id);
  g_assert_cmpstr(entry_wrap_for(entry, hex[KEY_CAROL], GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)
                    ->event_id, ==, only_wrap(&carol_only, hex[KEY_CAROL])->id);
  g_assert_cmpuint(bob_only.stored->len, ==, 1);
  g_assert_cmpuint(carol_only.stored->len, ==, 1);
  NostrEvent *self_seal = assert_only_opens(only_wrap(&own, hex[KEY_ALICE])->event, KEY_ALICE,
                                            rumor);
  nostr_event_free(self_seal);
  /* Sending (and relaying) fetched nothing. */
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 0);

  /* Bob and Carol: only their own wrap, then their own Download. */
  struct { guint key; WireRelay *relay; } receivers[] = {
    { KEY_BOB, &bob_only }, { KEY_CAROL, &carol_only },
  };
  for (guint i = 0; i < G_N_ELEMENTS(receivers); i++) {
    guint key = receivers[i].key;
    NostrEvent *wrap = only_wrap(receivers[i].relay, hex[key])->event;
    NostrEvent *seal = assert_only_opens(wrap, key, rumor);
    nostr_event_free(seal);
    g_autofree gchar *opened = open_wrap(wrap, key, NULL);
    g_autoptr(GhMessage) message = gh_message_new_from_rumor(hex[key], opened, &error);
    g_assert_no_error(error);
    g_assert_cmpint(gh_message_get_kind(message), ==, 15);
    g_autoptr(GhNip17File) file = gh_message_dup_file(message);
    g_assert_nonnull(file);
    GhBlossomClient *media = gh_blossom_client_new(NULL, http);
    gh_blossom_client_set_allow_private_hosts(media, TRUE); /* the loopback fixture */
    memset(&t, 0, sizeof t);
    gh_attachment_download_async(media, NULL, file, NULL, on_downloaded, &t);
    gh_test_spin_until(transfer_done, &t);
    g_assert_no_error(t.error);
    g_assert_true(g_bytes_equal(t.bytes, stripped->plaintext));
    g_assert_null(g_strstr_len(g_bytes_get_data(t.bytes, NULL), (gssize) g_bytes_get_size(t.bytes),
                               "GPS-ROOM-CANARY"));
    g_clear_pointer(&t.bytes, g_bytes_unref);
    g_object_unref(media);
    g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, i + 1);
  }

  g_object_unref(alice_media);
  g_object_unref(http);
  g_settings_reset(f.settings, "network-mode");
  fixture_down(&f);
  blossom_fixture_free(blossom);
  WireRelay *relays[] = { &bob_only, &carol_only, &own };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    relay_clear(relays[i]);
}
#endif
#endif

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  for (guint key = 1; key < GH_TEST_KEYS; key++)
    hex[key] = gh_test_pub(key);
  npub_alice = gh_test_npub(KEY_ALICE);
  gh_test_bus_up(&shared_bus);

  g_test_add_func("/groundhog/multi-send/rumor", test_room_rumor);
  g_test_add_func("/groundhog/multi-send/file-rumor", test_room_file_rumor);
  g_test_add_func("/groundhog/multi-send/limits", test_limits);
  g_test_add_func("/groundhog/multi-send/block", test_room_block);
#ifdef GROUNDHOG_TEST_WIRE
  g_test_add_func("/groundhog/multi-send/wire/room", test_wire_room);
  g_test_add_func("/groundhog/multi-send/wire/partial-retry", test_wire_partial_retry);
  g_test_add_func("/groundhog/multi-send/wire/missing-inbox", test_wire_missing_inbox);
  g_test_add_func("/groundhog/multi-send/wire/no-inbox-restart", test_wire_no_inbox_restart);
  g_test_add_func("/groundhog/multi-send/wire/lookup-and-nobody", test_wire_lookup_and_nobody);
  g_test_add_func("/groundhog/multi-send/wire/disappearing", test_wire_disappearing);
  g_test_add_func("/groundhog/multi-send/wire/spacing", test_wire_spacing);
  g_test_add_func("/groundhog/multi-send/wire/retry-spacing", test_wire_retry_spacing);
  g_test_add_func("/groundhog/multi-send/wire/dm-sender-spacing", test_wire_dm_sender_spacing);
  g_test_add_func("/groundhog/multi-send/wire/dm-sender", test_wire_dm_sender);
#if GROUNDHOG_TEST_ATTACHMENTS
  g_test_add_func("/groundhog/multi-send/wire/room-file", test_wire_room_file);
#endif
#endif
  int result = g_test_run();
  gh_test_bus_down(&shared_bus);
  return result;
}
