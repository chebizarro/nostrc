/* NIP-37 draft synchronization (plan §3.7, nostrc-9lwl7.8): two GhDraftSync
 * instances ("devices") of one account over an in-test relay that stores
 * addressable events, answers every REQ with what it holds and EOSE, accepts
 * every EVENT and forwards it to the other device's live REQ. The real
 * account controller, mock org.nostr.Signer on a private bus (real NIP-44
 * and signatures), SQLCipher stores on fake clocks and the real social list
 * service carry the kind-10013 private relay list. No sleeps: every wait
 * is a bounded main-context spin on a condition, and every timer of the
 * service runs on the fake clock. */
#include "gh-draft-sync.h"
#include "gh-draft-sync-backend.h"
#include "gh-preferences-dialog.h"
#include "gh-test-signer.h"
#include "nostrc-test-gdk-frame.h"
void groundhog_register_resource(void);
#include <glib/gstdio.h>
#include <jansson.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <nostr/nip37/nip37.h>

#define T0 1700000000
#define DISC "wss://discovery.test.invalid"
#define WRITE_A "wss://write-a.test.invalid"
#define PRIVATE_A "wss://private-a.test.invalid"
#define PRIVATE_B "wss://private-b.test.invalid"
#define BLOCKED "wss://blocked.test.invalid"
#define PEER "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define PEER2 "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

static GhTestBus bus;
static GhTestSigner signer;
static gchar *account, *npub, *other_npub, *room, *room2;
static gchar *seed; /* the account's kind 10002, on every relay from the start */

/* ---- the in-test relay ------------------------------------------------------------- */

typedef struct { GhRelayScope *scope; gchar *url; gint kind; gboolean closed, eose; } Read;
typedef struct { GhRelayPublish *publish; gchar *url, *json, *id; gint kind; gboolean closed, answered; } Write;
static struct {
  GPtrArray *reads, *writes, *stored; /* stored: signed JSON, one per address */
  gboolean auto_eose, auto_ok, fail_writes; /* fail_writes: every EVENT fails at once */
} relay;

static void drain(void) { while (g_main_context_iteration(NULL, FALSE)); }
static void read_free(gpointer p) { Read *r = p; g_free(r->url); g_free(r); }
static void write_free(gpointer p) { Write *w = p; g_free(w->url); g_free(w->json); g_free(w->id); g_free(w); }

static void
relay_reset(void)
{
  if (relay.reads) g_ptr_array_unref(relay.reads);
  if (relay.writes) g_ptr_array_unref(relay.writes);
  if (relay.stored) g_ptr_array_unref(relay.stored);
  relay.reads = g_ptr_array_new_with_free_func(read_free);
  relay.writes = g_ptr_array_new_with_free_func(write_free);
  relay.stored = g_ptr_array_new_with_free_func(g_free);
  relay.auto_eose = relay.auto_ok = TRUE;
  relay.fail_writes = FALSE;
  if (seed) g_ptr_array_add(relay.stored, g_strdup(seed));
}

static gchar *
event_address(const gchar *json, gint *out_kind)
{
  json_t *root = json_loads(json, 0, NULL);
  gint kind = json_integer_value(json_object_get(root, "kind"));
  const gchar *d = "";
  json_t *tags = json_object_get(root, "tags");
  for (gsize i = 0; i < json_array_size(tags); i++) {
    json_t *tag = json_array_get(tags, i);
    if (!g_strcmp0(json_string_value(json_array_get(tag, 0)), "d"))
      d = json_string_value(json_array_get(tag, 1));
  }
  gchar *address = (kind >= 10000 && kind < 20000) || (kind >= 30000 && kind < 40000)
    ? g_strdup_printf("%d:%s:%s", kind, json_string_value(json_object_get(root, "pubkey")), d)
    : g_strdup(json_string_value(json_object_get(root, "id")));
  if (out_kind) *out_kind = kind;
  json_decref(root);
  return address;
}

/* Keeps the newest event per address (NIP-01 ordering). */
static void
relay_store(const gchar *json)
{
  g_autofree gchar *address = event_address(json, NULL);
  for (guint i = 0; i < relay.stored->len; i++) {
    g_autofree gchar *other = event_address(relay.stored->pdata[i], NULL);
    if (!g_strcmp0(other, address)) { g_ptr_array_remove_index(relay.stored, i); break; }
  }
  g_ptr_array_add(relay.stored, g_strdup(json));
}

static gboolean
deliver_read(gpointer data)
{
  Read *r = data;
  if (r->closed) return G_SOURCE_REMOVE;
  for (guint i = 0; i < relay.stored->len; i++) {
    gint kind = 0;
    g_autofree gchar *address = event_address(relay.stored->pdata[i], &kind);
    if (kind == r->kind) gh_relay_scope_event(r->scope, r->url, relay.stored->pdata[i]);
  }
  /* Only the draft REQs can be held before EOSE (relay_eose()). */
  if ((relay.auto_eose || r->kind != NOSTR_KIND_DRAFT_WRAP) && !r->eose) {
    r->eose = TRUE; gh_relay_scope_eose(r->scope, r->url);
  }
  return G_SOURCE_REMOVE;
}

static gpointer
read_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data, GError **error)
{
  (void) data; (void) error;
  g_assert_cmpuint(filters->count, ==, 1);
  Read *r = g_new0(Read, 1);
  r->scope = scope; r->url = g_strdup(url); r->kind = filters->filters[0].kinds.data[0];
  if (r->kind == NOSTR_KIND_DRAFT_WRAP) {
    /* The draft REQ names the account and k=14, and nothing else. */
    char *raw = nostr_filter_serialize_compact(&filters->filters[0]);
    g_assert_nonnull(strstr(raw, account));
    g_assert_nonnull(strstr(raw, "\"#k\":[\"14\"]"));
    free(raw);
  }
  g_ptr_array_add(relay.reads, r);
  g_idle_add(deliver_read, r);
  return r;
}
static void read_close(gpointer handle, gpointer data) { (void) data; ((Read *) handle)->closed = TRUE; }

static gboolean
fail_write(gpointer data)
{
  Write *w = data;
  if (w->closed || w->answered) return G_SOURCE_REMOVE;
  w->answered = TRUE;
  gh_relay_publish_failed(w->publish, w->url, "connection refused");
  return G_SOURCE_REMOVE;
}

static gboolean
deliver_write(gpointer data)
{
  Write *w = data;
  if (w->closed || w->answered) return G_SOURCE_REMOVE;
  w->answered = TRUE;
  relay_store(w->json);
  gh_relay_publish_ok(w->publish, w->url, w->id, TRUE, "");
  /* Live forwarding to every open REQ of that kind on that relay. */
  for (guint i = 0; i < relay.reads->len; i++) {
    Read *r = relay.reads->pdata[i];
    if (!r->closed && r->kind == w->kind && !g_strcmp0(r->url, w->url))
      gh_relay_scope_event(r->scope, r->url, w->json);
  }
  return G_SOURCE_REMOVE;
}

static gpointer
write_open(GhRelayPublish *publish, const gchar *url, const gchar *json, gpointer data, GError **error)
{
  (void) data; (void) error;
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  Write *w = g_new0(Write, 1);
  w->publish = publish; w->url = g_strdup(url); w->json = g_strdup(json);
  w->id = nostr_event_get_id(event); w->kind = nostr_event_get_kind(event);
  nostr_event_free(event);
  g_ptr_array_add(relay.writes, w);
  if (relay.fail_writes) g_idle_add(fail_write, w);
  else if (relay.auto_ok) g_idle_add(deliver_write, w);
  return w;
}
static void write_close(gpointer handle, gpointer data) { (void) data; ((Write *) handle)->closed = TRUE; }
static gboolean write_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{ (void) handle; (void) json; (void) data; (void) error; return TRUE; }
static gboolean write_resend(gpointer handle, gpointer data, GError **error)
{ (void) handle; (void) data; (void) error; return TRUE; }
static const GhRelayTransport read_transport = { read_open, read_close };
static const GhRelayPublishTransport write_transport = { write_open, write_close };
static const GhRelayPublishAuthTransport write_auth_transport = { write_auth, write_resend };

/* Writes of kind on url (any url: NULL), in order. */
static GPtrArray *
writes_of(gint kind, const gchar *url)
{
  GPtrArray *result = g_ptr_array_new();
  for (guint i = 0; i < relay.writes->len; i++) {
    Write *w = relay.writes->pdata[i];
    if (w->kind == kind && (!url || !g_strcmp0(w->url, url))) g_ptr_array_add(result, w);
  }
  return result;
}
static Write *
write_at(guint index)
{
  g_autoptr(GPtrArray) writes = writes_of(NOSTR_KIND_DRAFT_WRAP, NULL);
  g_assert_cmpuint(index, <, writes->len);
  return writes->pdata[index]; /* Borrowed from relay.writes. */
}
static guint drafts_written(void) { g_autoptr(GPtrArray) w = writes_of(NOSTR_KIND_DRAFT_WRAP, NULL); return w->len; }
static gboolean
all_writes_answered(gpointer data)
{
  (void) data;
  for (guint i = 0; i < relay.writes->len; i++)
    if (!((Write *) relay.writes->pdata[i])->answered) return FALSE;
  return TRUE;
}
static gboolean
private_list_synced(gpointer data)
{
  GNostrListService *service = gh_social_lists_get_service(data);
  return gnostr_list_service_get_state(service, NOSTR_KIND_PRIVATE_RELAY_LIST, "") == GNOSTR_LIST_SYNCED;
}
/* Wait for the publication's terminal state, not an instant with no new
 * writes: a signer D-Bus reply may still be in flight before the first write. */
static void
settle(GhSocialLists *social)
{
  gh_test_spin_until(private_list_synced, social);
  gh_test_spin_until(all_writes_answered, NULL);
  drain();
}
static Read *
read_for(gint kind, const gchar *url)
{
  for (guint i = relay.reads->len; i > 0; i--) {
    Read *r = relay.reads->pdata[i - 1];
    if (!r->closed && r->kind == kind && !g_strcmp0(r->url, url)) return r;
  }
  return NULL;
}
static void
relay_eose(gint kind, const gchar *url)
{
  Read *r = read_for(kind, url);
  g_assert_nonnull(r);
  if (!r->eose) { r->eose = TRUE; gh_relay_scope_eose(r->scope, r->url); }
  drain();
}

/* ---- fixture events ---------------------------------------------------------------- */

static gchar *
sign_json(gint kind, gint64 at, NostrTags *tags, const gchar *content)
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

static gchar *
encrypt_to_self(const gchar *plain)
{
  guint8 secret[32], pubkey[32]; char *cipher = NULL;
  g_assert_true(nostr_hex2bin(secret, gh_test_secret[1], 32));
  g_assert_true(nostr_hex2bin(pubkey, account, 32));
  g_assert_cmpint(nostr_nip44_encrypt_v2(secret, pubkey, (const guint8 *) plain, strlen(plain), &cipher), ==, 0);
  gchar *result = g_strdup(cipher); free(cipher); return result;
}

/* A draft wrap as another NIP-37 client would publish it: an unsigned
 * kind-14 to peer (the account for a note to self) under identifier d. */
static gchar *
remote_draft(const gchar *d, const gchar *peer, const gchar *text, gint64 at, const gchar *cipher_override)
{
  NostrEvent *inner = nostr_event_new();
  nostr_event_set_kind(inner, NOSTR_KIND_DIRECT_MESSAGE); nostr_event_set_pubkey(inner, account);
  nostr_event_set_created_at(inner, at); nostr_event_set_content(inner, text);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", peer, NULL));
  nostr_event_set_tags(inner, tags);
  char *plain = nostr_nip37_serialize_draft(inner);
  g_assert_nonnull(plain);
  nostr_event_free(inner);
  g_autofree gchar *cipher = cipher_override ? g_strdup(cipher_override) : encrypt_to_self(plain);
  free(plain);
  gint64 expiration = at + GH_DRAFT_SYNC_EXPIRATION_SECONDS;
  NostrEvent *wrap = nostr_nip37_build_envelope(account, at, d, NOSTR_KIND_DIRECT_MESSAGE, cipher, &expiration);
  g_assert_nonnull(wrap);
  g_assert_cmpint(nostr_event_sign(wrap, gh_test_secret[1]), ==, 0);
  char *raw = nostr_event_serialize_compact(wrap);
  gchar *json = g_strdup(raw); free(raw); nostr_event_free(wrap);
  return json;
}

static gchar *
remote_blank(const gchar *d, gint64 at)
{
  gint64 expiration = at + GH_DRAFT_SYNC_EXPIRATION_SECONDS;
  NostrEvent *wrap = nostr_nip37_build_envelope(account, at, d, NOSTR_KIND_DIRECT_MESSAGE, "", &expiration);
  g_assert_cmpint(nostr_event_sign(wrap, gh_test_secret[1]), ==, 0);
  char *raw = nostr_event_serialize_compact(wrap);
  gchar *json = g_strdup(raw); free(raw); nostr_event_free(wrap);
  return json;
}

/* Feeds an event to a device's live draft REQ on PRIVATE_A, as the relay would. */
static void
feed(const gchar *json)
{
  relay_store(json);
  for (guint i = 0; i < relay.reads->len; i++) {
    Read *r = relay.reads->pdata[i];
    if (!r->closed && r->kind == NOSTR_KIND_DRAFT_WRAP) gh_relay_scope_event(r->scope, r->url, json);
  }
  drain();
}

/* ---- devices --------------------------------------------------------------------------- */

typedef struct {
  GSettings *settings;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhSocialLists *social;
  GhClock *clock;
  GhStore *store;
  GhStoreConversations *conversations;
  GhDraftSync *sync;
  gchar *dir;
  gboolean own_dir, focused;
  GPtrArray *applied; /* "room\ntext" per applier call */
  guint status_changes;
} Device;

static GPtrArray *
identities(gpointer data, GError **error)
{
  (void) data; (void) error;
  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify) gh_identity_info_free);
  const gchar *keys[] = { npub, other_npub };
  for (guint i = 0; i < 2; i++) {
    GhIdentityInfo *id = g_new0(GhIdentityInfo, 1); id->npub = g_strdup(keys[i]);
    g_ptr_array_add(result, id);
  }
  return result;
}
static gboolean active(gpointer data) { return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE; }
static gboolean discovered(gpointer data) { return gh_account_relays_get_state(data) == GH_ACCOUNT_RELAYS_COMPLETE; }
static gboolean applier_focused(const gchar *room_key, gpointer data)
{ (void) room_key; return ((Device *) data)->focused; }
static void
applier_apply(const gchar *room_key, const gchar *text, gpointer data)
{
  Device *d = data;
  g_ptr_array_add(d->applied, g_strdup_printf("%s\n%s", room_key, text ? text : ""));
}
static void status_changed(GhDraftSync *sync, Device *d) { (void) sync; d->status_changes++; }
/* The index-th applier call was for room_key with text (NULL: cleared). */
static void
assert_applied(Device *d, guint index, const gchar *room_key, const gchar *text)
{
  g_assert_cmpuint(d->applied->len, >, index);
  g_autofree gchar *want = g_strdup_printf("%s\n%s", room_key, text ? text : "");
  g_assert_cmpstr(d->applied->pdata[index], ==, want);
}

static void
open_store(Device *d)
{
  guint8 key[GH_STORE_KEY_SIZE] = { 7 };
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { d->dir, account, NULL, NULL, d->clock };
  g_autoptr(GError) error = NULL;
  d->store = gh_store_open_with_key(&config, bytes, "4c1a0c3e-6b4f-4a6e-9a3b-2d9f0c2f9c11", GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  d->conversations = gh_store_conversations_new(d->store);
}

static void
bind(Device *d)
{
  d->social = gh_social_lists_new(NULL);
  GhListAdapterNetwork network = { .accounts = d->accounts, .scope_transport = &read_transport,
    .publish_transport = &write_transport, .publish_auth_transport = &write_auth_transport,
    .publish_deadline = 10 };
  gh_social_lists_set_network(d->social, &network, d->relays);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_social_lists_bind_store(d->social, d->store, gh_account_controller_get_generation(d->accounts), &error));
  g_assert_no_error(error);
  d->sync = gh_draft_sync_new(d->social);
  gh_draft_sync_set_network(d->sync, &network);
  GhDraftSyncApplier applier = { applier_focused, applier_apply };
  gh_draft_sync_set_applier(d->sync, &applier, d);
  g_signal_connect(d->sync, "changed", G_CALLBACK(status_changed), d);
  g_assert_true(gh_draft_sync_bind_store(d->sync, d->store, d->conversations,
                                         gh_account_controller_get_generation(d->accounts), &error));
  g_assert_no_error(error);
  drain();
}

static void
device_up(Device *d, const gchar *dir)
{
  memset(d, 0, sizeof *d);
  d->applied = g_ptr_array_new_with_free_func(g_free);
  d->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(d->settings, "signer-method", "auto");
  g_settings_set_string(d->settings, "current-npub", npub);
  const gchar *discovery[] = { DISC, NULL };
  g_settings_set_strv(d->settings, "discovery-relays", discovery);
  d->accounts = gh_account_controller_new_full(d->settings, bus.client, identities, NULL);
  gh_test_spin_until(active, d->accounts);
  d->relays = gh_account_relays_new(d->accounts, d->settings, &read_transport, NULL);
  gh_test_spin_until(discovered, d->relays);
  d->clock = gh_clock_new_fake((gint64) T0 * G_USEC_PER_SEC);
  if (dir) d->dir = g_strdup(dir);
  else { d->dir = g_dir_make_tmp("groundhog-draft-sync-XXXXXX", NULL); d->own_dir = TRUE; }
  open_store(d);
  bind(d);
}

static void
remove_tree(const gchar *path)
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

/* Closes the services and the store (a restart keeps the directory). */
static void
device_close(Device *d)
{
  gh_draft_sync_close(d->sync);
  gh_social_lists_close(d->social);
  gh_test_release(g_steal_pointer(&d->sync));
  gh_test_release(g_steal_pointer(&d->social));
  drain();
  gh_store_conversations_close(d->conversations);
  g_clear_object(&d->conversations);
  if (d->store) { gh_store_close(d->store); d->store = NULL; }
}

static void
device_down(Device *d)
{
  if (d->sync) device_close(d);
  gh_test_release(g_steal_pointer(&d->relays));
  gh_test_release(g_steal_pointer(&d->accounts));
  GhTestSenders senders = { &bus, &signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &senders);
  g_object_unref(d->settings);
  g_clear_pointer(&d->clock, gh_clock_unref);
  g_ptr_array_unref(d->applied);
  if (d->own_dir) remove_tree(d->dir);
  g_free(d->dir);
}

static void
advance(Device *d, gint seconds)
{
  gh_clock_fake_advance(d->clock, (gint64) seconds * G_USEC_PER_SEC);
  drain();
}

static void
set_private_relays(Device *d, const gchar *const *urls, gboolean blocked_list)
{
  gint kind = blocked_list ? NOSTR_KIND_BLOCKED_RELAY_LIST : NOSTR_KIND_PRIVATE_RELAY_LIST;
  g_autoptr(GError) error = NULL;
  g_autoptr(GNostrListDocument) snapshot = gh_social_lists_ensure(d->social, kind, "", &error);
  g_assert_no_error(error);
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify) gnostr_list_operation_unref);
  for (guint i = 0; urls && urls[i]; i++) {
    const gchar *fields[] = { "relay", urls[i] };
    g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, TRUE, fields, 2, 0, 0));
  }
  g_autoptr(GNostrListDocument) saved = gh_social_lists_apply(d->social, kind, "",
    gnostr_list_document_get_local_revision(snapshot), ops, &error);
  g_assert_no_error(error); g_assert_nonnull(saved); drain();
}

static void
enable(Device *d, gboolean value)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_social_lists_set_sync_enabled(d->social, value, &error)); g_assert_no_error(error);
  drain();
}

/* A device with sync on and PRIVATE_A as its private relay. */
static void
device_up_synced(Device *d)
{
  device_up(d, NULL);
  const gchar *private[] = { PRIVATE_A, NULL };
  set_private_relays(d, private, FALSE);
  enable(d, TRUE);
  g_assert_true(gh_draft_sync_is_active(d->sync));
}

static void
set_draft(Device *d, const gchar *room_key, const gchar *text)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_set_draft(d->conversations, room_key, text, &error));
  g_assert_no_error(error);
  drain();
}

static gchar *
get_draft(Device *d, const gchar *room_key)
{
  g_autoptr(GError) error = NULL;
  gchar *text = NULL;
  g_assert_true(gh_store_conversations_get_draft(d->conversations, room_key, &text, &error));
  g_assert_no_error(error);
  return text;
}

static GhStoreDraftState *
state(Device *d, const gchar *room_key)
{
  g_autoptr(GError) error = NULL;
  GhStoreDraftState *s = gh_store_drafts_get(d->store, room_key, &error);
  g_assert_no_error(error);
  return s;
}

static gboolean
status_saved_locally(gpointer data)
{
  const gchar *status = gh_draft_sync_get_status(((Device *) data)->sync);
  return status && strstr(status, "Saved on This Device") && strstr(status, PRIVATE_A);
}
static gboolean import_complete(gpointer data)
{
  gboolean complete = FALSE;
  gh_draft_sync_get_import_state(((Device *) data)->sync, &complete, NULL, NULL);
  return complete;
}
typedef struct { guint count; } WriteCount;
static gboolean drafts_written_at_least(gpointer data) { return drafts_written() >= ((WriteCount *) data)->count; }
static void
await_drafts(guint count)
{
  WriteCount want = { count };
  gh_test_spin_until(drafts_written_at_least, &want);
  drain();
}
typedef struct { Device *d; const gchar *room; const gchar *text; } DraftWait;
static gboolean
draft_is(gpointer data)
{
  DraftWait *wait = data;
  g_autofree gchar *text = get_draft(wait->d, wait->room);
  return g_strcmp0(text, wait->text) == 0;
}
static gboolean
room_clean(gpointer data)
{
  DraftWait *wait = data;
  g_autoptr(GhStoreDraftState) s = state(wait->d, wait->room);
  return s && !s->dirty && s->last_event_id;
}

/* Edits and lets the local cadence (30 s after the last edit) publish. */
static void
edit_and_publish(Device *d, const gchar *room_key, const gchar *text)
{
  guint before = drafts_written();
  set_draft(d, room_key, text);
  advance(d, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  await_drafts(before + 1);
  DraftWait wait = { d, room_key, text };
  gh_test_spin_until(room_clean, &wait);
}

static json_t *
parse_write(Write *w)
{
  json_t *root = json_loads(w->json, 0, NULL);
  g_assert_nonnull(root);
  return root;
}

static const gchar *
tag_value(json_t *root, const gchar *name)
{
  json_t *tags = json_object_get(root, "tags");
  for (gsize i = 0; i < json_array_size(tags); i++) {
    json_t *tag = json_array_get(tags, i);
    if (!g_strcmp0(json_string_value(json_array_get(tag, 0)), name))
      return json_string_value(json_array_get(tag, 1));
  }
  return NULL;
}

/* ---- tests ----------------------------------------------------------------------------- */

/* The outer event carries nothing but a random identifier, k=14 and an
 * expiration: no participant, no hash of them, no client marker. */
static void
test_opaque_identifiers(void)
{
  relay_reset();
  Device a; device_up_synced(&a);
  gh_test_spin_until(import_complete, &a);
  edit_and_publish(&a, room, "hello there");
  g_autoptr(GPtrArray) writes = writes_of(NOSTR_KIND_DRAFT_WRAP, NULL);
  g_assert_cmpuint(writes->len, ==, 1);
  Write *w = writes->pdata[0];
  g_assert_cmpstr(w->url, ==, PRIVATE_A);
  json_t *root = parse_write(w);
  json_t *tags = json_object_get(root, "tags");
  g_assert_cmpuint(json_array_size(tags), ==, 3);
  const gchar *d = tag_value(root, "d");
  g_assert_cmpuint(strlen(d), ==, 64);
  for (guint i = 0; i < 64; i++) g_assert_true(g_ascii_isxdigit(d[i]) && !g_ascii_isupper(d[i]));
  g_assert_cmpstr(tag_value(root, "k"), ==, "14");
  gint64 created_at = json_integer_value(json_object_get(root, "created_at"));
  g_assert_cmpint(g_ascii_strtoll(tag_value(root, "expiration"), NULL, 10), ==, created_at + GH_DRAFT_SYNC_EXPIRATION_SECONDS);
  /* Not derived from the room: neither a participant nor their hash. */
  g_autofree gchar *room_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, room, -1);
  g_autofree gchar *peer_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, PEER, -1);
  g_assert_cmpstr(d, !=, room_hash); g_assert_cmpstr(d, !=, peer_hash);
  g_assert_null(strstr(w->json, PEER)); g_assert_null(strstr(w->json, "hello there"));
  g_autofree gchar *lower = g_ascii_strdown(w->json, -1);
  g_assert_null(strstr(lower, "groundhog"));
  g_autoptr(GhStoreDraftState) s = state(&a, room);
  g_assert_cmpstr(s->draft_d, ==, d); g_assert_cmpstr(s->last_event_id, ==, w->id);
  g_assert_cmpint(s->expires_at, ==, created_at + GH_DRAFT_SYNC_EXPIRATION_SECONDS);
  /* The second identifier of another room differs. */
  edit_and_publish(&a, room2, "second room");
  g_autoptr(GhStoreDraftState) s2 = state(&a, room2);
  g_assert_cmpstr(s2->draft_d, !=, d);
  json_decref(root);
  device_down(&a);
}

/* Only configured, unblocked kind-10013 relays carry drafts; a blocked one
 * is skipped without fallback, and none at all means a clear status. */
static void
test_private_relays_only(void)
{
  relay_reset();
  Device a; device_up(&a, NULL);
  enable(&a, TRUE);
  g_assert_false(gh_draft_sync_is_active(a.sync));
  g_assert_cmpstr(gh_draft_sync_get_status(a.sync), ==, "Choose a Private Relay to Sync Drafts");
  set_draft(&a, room, "nowhere to go");
  advance(&a, 120);
  g_assert_cmpuint(drafts_written(), ==, 0);
  g_assert_null(read_for(NOSTR_KIND_DRAFT_WRAP, WRITE_A));
  const gchar *blocked[] = { BLOCKED, NULL };
  set_private_relays(&a, blocked, TRUE);
  const gchar *private[] = { PRIVATE_A, BLOCKED, PRIVATE_B, NULL };
  set_private_relays(&a, private, FALSE);
  g_assert_true(gh_draft_sync_is_active(a.sync));
  g_assert_null(gh_draft_sync_get_status(a.sync));
  g_assert_nonnull(read_for(NOSTR_KIND_DRAFT_WRAP, PRIVATE_A));
  g_assert_nonnull(read_for(NOSTR_KIND_DRAFT_WRAP, PRIVATE_B));
  g_assert_null(read_for(NOSTR_KIND_DRAFT_WRAP, BLOCKED));
  g_assert_null(read_for(NOSTR_KIND_DRAFT_WRAP, WRITE_A));
  g_assert_null(read_for(NOSTR_KIND_DRAFT_WRAP, DISC));
  gh_test_spin_until(import_complete, &a);
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  await_drafts(2);
  g_autoptr(GPtrArray) writes = writes_of(NOSTR_KIND_DRAFT_WRAP, NULL);
  for (guint i = 0; i < writes->len; i++) {
    Write *w = writes->pdata[i];
    g_assert_true(!g_strcmp0(w->url, PRIVATE_A) || !g_strcmp0(w->url, PRIVATE_B));
  }
  g_autoptr(GPtrArray) elsewhere = writes_of(NOSTR_KIND_DRAFT_WRAP, WRITE_A);
  g_assert_cmpuint(elsewhere->len, ==, 0);
  device_down(&a);
}

/* 30 s after the last edit, at most once per room per 60 s, and leaving the
 * room schedules the latest revision inside that limit. */
static void
test_cadence(void)
{
  relay_reset();
  Device a; device_up_synced(&a);
  gh_test_spin_until(import_complete, &a);
  settle(a.social);
  gh_draft_sync_set_shown_room(a.sync, room);
  guint calls = signer.calls;
  set_draft(&a, room, "one");
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS - 1);
  gh_test_signer_assert_no_calls_since(&signer, calls); /* no encrypt, no sign yet */
  g_assert_cmpuint(drafts_written(), ==, 0);
  set_draft(&a, room, "one two"); /* the debounce restarts */
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS - 1);
  gh_test_signer_assert_no_calls_since(&signer, calls);
  g_assert_cmpuint(drafts_written(), ==, 0);
  advance(&a, 1);
  await_drafts(1);
  DraftWait wait = { &a, room, "one two" };
  gh_test_spin_until(room_clean, &wait);
  g_autofree gchar *first_id = g_strdup(((Write *) write_at(0))->id);
  /* An edit right after: the rate limit, not the debounce, decides. */
  settle(a.social);
  calls = signer.calls;
  set_draft(&a, room, "one two three");
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  gh_test_signer_assert_no_calls_since(&signer, calls);
  g_assert_cmpuint(drafts_written(), ==, 1);
  advance(&a, GH_DRAFT_SYNC_RATE_SECONDS - GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  await_drafts(2);
  /* Leaving the room publishes the latest saved revision at once, but not a
   * second one inside the rate limit. */
  settle(a.social);
  calls = signer.calls;
  set_draft(&a, room, "leaving");
  gh_draft_sync_set_shown_room(a.sync, room2);
  drain();
  gh_test_signer_assert_no_calls_since(&signer, calls);
  g_assert_cmpuint(drafts_written(), ==, 2);
  advance(&a, GH_DRAFT_SYNC_RATE_SECONDS);
  await_drafts(3);
  g_autoptr(GPtrArray) writes = writes_of(NOSTR_KIND_DRAFT_WRAP, NULL);
  gint64 previous = 0;
  json_t *first = parse_write(writes->pdata[0]);
  for (guint i = 0; i < writes->len; i++) {
    json_t *root = parse_write(writes->pdata[i]);
    gint64 at = json_integer_value(json_object_get(root, "created_at"));
    g_assert_cmpint(at, >, previous); previous = at; /* every revision supersedes the last */
    g_assert_cmpstr(tag_value(root, "d"), ==, tag_value(first, "d"));
    json_decref(root);
  }
  json_decref(first);
  g_assert_cmpstr(((Write *) writes->pdata[0])->id, ==, first_id);
  device_down(&a);
}

/* No identifier is created before every private relay reached EOSE. */
static void
test_no_identifier_before_eose(void)
{
  relay_reset();
  relay.auto_eose = FALSE;
  Device a; device_up(&a, NULL);
  const gchar *private[] = { PRIVATE_A, PRIVATE_B, NULL };
  set_private_relays(&a, private, FALSE);
  enable(&a, TRUE);
  g_assert_false(import_complete(&a));
  set_draft(&a, room, "waiting");
  advance(&a, 10 * GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  g_assert_cmpuint(drafts_written(), ==, 0);
  g_autoptr(GhStoreDraftState) s = state(&a, room);
  g_assert_null(s->draft_d);
  relay_eose(NOSTR_KIND_DRAFT_WRAP, PRIVATE_A);
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  g_assert_cmpuint(drafts_written(), ==, 0); /* one relay is not every relay */
  relay_eose(NOSTR_KIND_DRAFT_WRAP, PRIVATE_B);
  g_assert_true(import_complete(&a));
  await_drafts(2); /* both private relays */
  g_autoptr(GhStoreDraftState) after = state(&a, room);
  g_assert_nonnull(after->draft_d);
  device_down(&a);
}

/* Two devices converge: a revision published here is applied there, an
 * edit there comes back under the same identifier, and a clear at send
 * empties the other device's draft. */
static void
test_two_devices_converge(void)
{
  relay_reset();
  Device a, b;
  device_up_synced(&a); gh_test_spin_until(import_complete, &a);
  device_up_synced(&b); gh_test_spin_until(import_complete, &b);
  edit_and_publish(&a, room, "from a");
  DraftWait on_b = { &b, room, "from a" };
  gh_test_spin_until(draft_is, &on_b);
  g_assert_cmpuint(b.applied->len, ==, 1);
  assert_applied(&b, 0, room, "from a");
  g_autoptr(GhStoreDraftState) sa = state(&a, room);
  g_autoptr(GhStoreDraftState) sb = state(&b, room);
  g_assert_cmpstr(sa->draft_d, ==, sb->draft_d);
  g_assert_cmpstr(sa->last_event_id, ==, sb->last_event_id);
  g_assert_false(sb->dirty); g_assert_cmpint(sb->local_revision, ==, 0);
  /* B edits: the same identifier, a newer revision; A follows. */
  advance(&b, 5);
  edit_and_publish(&b, room, "from b");
  DraftWait on_a = { &a, room, "from b" };
  gh_test_spin_until(draft_is, &on_a);
  g_autoptr(GPtrArray) writes = writes_of(NOSTR_KIND_DRAFT_WRAP, NULL);
  g_assert_cmpuint(writes->len, ==, 2);
  json_t *first = parse_write(writes->pdata[0]), *second = parse_write(writes->pdata[1]);
  g_assert_cmpstr(tag_value(first, "d"), ==, tag_value(second, "d"));
  g_assert_cmpint(json_integer_value(json_object_get(second, "created_at")), >,
                  json_integer_value(json_object_get(first, "created_at")));
  json_decref(first); json_decref(second);
  assert_applied(&a, 0, room, "from b");
  /* A sends: the enqueue clears the draft and a blank goes out; B empties. */
  gint64 id = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_find_conversation(a.store, GH_STORE_BACKEND_NIP17, room, &id, &error));
  GhStoreOutgoing outgoing = { .conversation_id = id, .op_id = "00000000000000000000000000000001",
    .backend_msg_id = "0000000000000000000000000000000100000000000000000000000000000001",
    .sender_pubkey = account, .kind = 14, .created_at = T0 + 1000, .body = "sent", .rumor_json = "{\"kind\":14}" };
  g_assert_true(gh_store_enqueue(a.store, &outgoing, NULL, NULL, &error));
  g_autofree gchar *cleared = get_draft(&a, room);
  g_assert_null(cleared);
  gh_draft_sync_wake(a.sync);
  advance(&a, GH_DRAFT_SYNC_RATE_SECONDS);
  await_drafts(3);
  Write *blank = ((Write *) write_at(2));
  json_t *root = parse_write(blank);
  g_assert_cmpstr(json_string_value(json_object_get(root, "content")), ==, "");
  g_assert_cmpstr(tag_value(root, "k"), ==, "14");
  g_assert_cmpstr(tag_value(root, "d"), ==, sa->draft_d);
  json_decref(root);
  DraftWait b_cleared = { &b, room, NULL };
  gh_test_spin_until(draft_is, &b_cleared);
  assert_applied(&b, b.applied->len - 1, room, NULL);
  /* A late, older revision cannot resurrect it on either device. */
  g_autofree gchar *late = remote_draft(sa->draft_d, PEER, "stale", T0 + 2, NULL);
  guint calls = signer.calls;
  feed(late);
  drain();
  g_autofree gchar *still_a = get_draft(&a, room), *still_b = get_draft(&b, room);
  g_assert_null(still_a); g_assert_null(still_b);
  gh_test_signer_assert_no_calls_since(&signer, calls); /* not even decrypted */
  device_down(&b);
  device_down(&a);
}

/* Both devices create a first identifier at once: the lowest one carries
 * the next revision and the other is blanked once that head is accepted. */
static void
test_concurrent_first_creation(void)
{
  relay_reset();
  relay.auto_ok = FALSE;
  Device a, b;
  device_up_synced(&a); gh_test_spin_until(import_complete, &a);
  device_up_synced(&b); gh_test_spin_until(import_complete, &b);
  set_draft(&a, room, "a first"); set_draft(&b, room, "b first");
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS); advance(&b, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  await_drafts(2);
  /* The relay answers both at once, so each sees the other's identifier. */
  relay.auto_ok = TRUE;
  for (guint i = 0; i < relay.writes->len; i++) g_idle_add(deliver_write, relay.writes->pdata[i]);
  drain();
  g_autoptr(GhStoreDraftState) sa = state(&a, room);
  g_autoptr(GhStoreDraftState) sb = state(&b, room);
  gboolean converged = FALSE;
  for (guint spins = 0; spins < 200 && !converged; spins++) {
    gh_test_spin_until(import_complete, &a);
    drain();
    g_autoptr(GhStoreDraftState) ta = state(&a, room);
    g_autoptr(GhStoreDraftState) tb = state(&b, room);
    converged = ta && tb && !g_strcmp0(ta->draft_d, tb->draft_d) && ta->aliases[0] && tb->aliases[0];
  }
  g_assert_true(converged);
  g_autoptr(GhStoreDraftState) ua = state(&a, room);
  g_autoptr(GhStoreDraftState) ub = state(&b, room);
  g_assert_cmpstr(ua->draft_d, ==, ub->draft_d);
  g_assert_cmpint(strcmp(ua->draft_d, ua->aliases[0]), <, 0); /* the lowest is the head */
  /* The newest revision (NIP-01: the lowest id of the two) won on both. */
  g_autofree gchar *text_a = get_draft(&a, room), *text_b = get_draft(&b, room);
  g_assert_cmpstr(text_a, ==, text_b);
  /* A newer head accepted blanks the alias on whoever publishes next. */
  advance(&a, GH_DRAFT_SYNC_RATE_SECONDS);
  edit_and_publish(&a, room, "a again");
  advance(&a, 1);
  WriteCount want = { 4 };
  gh_test_spin_until(drafts_written_at_least, &want);
  g_autoptr(GPtrArray) writes = writes_of(NOSTR_KIND_DRAFT_WRAP, NULL);
  Write *head = writes->pdata[2], *alias_blank = writes->pdata[3];
  json_t *h = parse_write(head), *ab = parse_write(alias_blank);
  g_assert_cmpstr(tag_value(h, "d"), ==, ua->draft_d);
  g_assert_cmpstr(tag_value(ab, "d"), ==, ua->aliases[0]);
  g_assert_cmpstr(json_string_value(json_object_get(ab, "content")), ==, "");
  json_decref(h); json_decref(ab);
  DraftWait wait = { &a, room, "a again" };
  gh_test_spin_until(room_clean, &wait);
  for (guint spins = 0; spins < 100; spins++) {
    drain();
    g_autoptr(GhStoreDraftState) va = state(&a, room);
    if (!va->aliases[0]) break;
  }
  g_autoptr(GhStoreDraftState) final_a = state(&a, room);
  g_assert_null(final_a->aliases[0]);
  device_down(&b); device_down(&a);
}

/* The import rules: expired, undecryptable and over-bound events are
 * counted, never applied or blanked; a blocked room's revision is ignored;
 * a newer revision for a dirty room becomes a conflict copy. */
static void
test_import_rules(void)
{
  relay_reset();
  Device a; device_up_synced(&a);
  gh_test_spin_until(import_complete, &a);
  const gchar *d1 = "1111111111111111111111111111111111111111111111111111111111111111";
  const gchar *d2 = "2222222222222222222222222222222222222222222222222222222222222222";
  const gchar *d3 = "3333333333333333333333333333333333333333333333333333333333333333";
  /* Expired, or a blank for an identifier nothing here knows: ignored
   * without a signer call. */
  guint calls = signer.calls;
  g_autofree gchar *expired = remote_draft(d1, PEER, "expired", T0 - GH_DRAFT_SYNC_EXPIRATION_SECONDS - 1, NULL);
  feed(expired);
  g_autofree gchar *unknown = remote_blank("5555555555555555555555555555555555555555555555555555555555555555", T0 + 1);
  feed(unknown);
  gh_test_signer_assert_no_calls_since(&signer, calls);
  g_autofree gchar *none = get_draft(&a, room);
  g_assert_null(none);
  /* Undecryptable: counted as incomplete, nothing written. */
  g_autofree gchar *garbage = remote_draft(d2, PEER, "x", T0 + 1, "not-a-ciphertext");
  feed(garbage);
  guint incomplete = 0, decrypted = 0;
  for (guint spins = 0; spins < 100 && !incomplete; spins++) {
    drain(); gh_draft_sync_get_import_state(a.sync, NULL, &decrypted, &incomplete);
    if (!incomplete) g_main_context_iteration(NULL, TRUE);
  }
  g_assert_cmpuint(incomplete, ==, 1);
  g_assert_cmpuint(drafts_written(), ==, 0);
  g_assert_cmpstr(gh_draft_sync_get_status(a.sync), ==, "1 draft could not be synced");
  /* A blocked room: ignored. */
  set_draft(&a, room2, "so that it is stored"); set_draft(&a, room2, "");
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_set_blocked(a.conversations, room2, TRUE, &error));
  g_assert_no_error(error);
  g_autofree gchar *blocked = remote_draft(d3, PEER2, "for a blocked room", T0 + 2, NULL);
  feed(blocked);
  for (guint spins = 0; spins < 100; spins++) {
    drain(); gh_draft_sync_get_import_state(a.sync, NULL, &decrypted, NULL);
    if (decrypted >= 2) break;
    g_main_context_iteration(NULL, TRUE);
  }
  g_autofree gchar *blocked_draft = get_draft(&a, room2);
  g_assert_null(blocked_draft);
  g_assert_cmpuint(a.applied->len, ==, 0);
  /* A dirty local draft keeps its text; the remote one is a conflict copy. */
  set_draft(&a, room, "typed here");
  g_autofree gchar *theirs = remote_draft(d1, PEER, "typed elsewhere", T0 + 3, NULL);
  feed(theirs);
  for (guint spins = 0; spins < 100; spins++) {
    drain(); gh_draft_sync_get_import_state(a.sync, NULL, &decrypted, NULL);
    if (decrypted >= 3) break;
    g_main_context_iteration(NULL, TRUE);
  }
  g_autofree gchar *mine = get_draft(&a, room);
  g_assert_cmpstr(mine, ==, "typed here");
  g_autoptr(GPtrArray) conflicts = gh_store_drafts_list_conflicts(a.store, room, &error);
  g_assert_cmpuint(conflicts->len, ==, 1);
  g_assert_cmpstr(((GhStoreDraftConflict *) conflicts->pdata[0])->text, ==, "typed elsewhere");
  g_assert_cmpuint(a.applied->len, ==, 0);
  /* The local revision then supersedes the remote one (created_at + 1). */
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  await_drafts(1);
  Write *w = write_at(0);
  json_t *root = parse_write(w);
  g_assert_cmpstr(tag_value(root, "d"), ==, d1); /* the identifier learned from the relay */
  g_assert_cmpint(json_integer_value(json_object_get(root, "created_at")), >, T0 + 3);
  json_decref(root);
  /* The composer focused with other text keeps it too (clean room). */
  DraftWait wait = { &a, room, "typed here" };
  gh_test_spin_until(room_clean, &wait);
  a.focused = TRUE;
  gh_draft_sync_set_shown_room(a.sync, room);
  g_autofree gchar *focused = remote_draft(d1, PEER, "while focused", T0 + 100, NULL);
  feed(focused);
  for (guint spins = 0; spins < 100; spins++) {
    drain(); gh_draft_sync_get_import_state(a.sync, NULL, &decrypted, NULL);
    if (decrypted >= 4) break;
    g_main_context_iteration(NULL, TRUE);
  }
  g_autofree gchar *kept = get_draft(&a, room);
  g_assert_cmpstr(kept, ==, "typed here");
  g_autoptr(GPtrArray) conflicts2 = gh_store_drafts_list_conflicts(a.store, room, &error);
  g_assert_cmpuint(conflicts2->len, ==, 2);
  device_down(&a);
}

/* At most 64 revisions are decrypted per binding; the rest are reported. */
static void
test_import_bound(void)
{
  relay_reset();
  Device a; device_up(&a, NULL);
  for (guint i = 0; i < GH_DRAFT_SYNC_MAX_IMPORT + 2; i++) {
    g_autofree gchar *d = g_strdup_printf("%064x", i + 1);
    g_autofree gchar *peer = g_strdup_printf("%064x", 0x10000 + i);
    g_autofree gchar *text = g_strdup_printf("draft %u", i);
    g_autofree gchar *json = remote_draft(d, peer, text, T0 + i, NULL);
    relay_store(json);
  }
  const gchar *private[] = { PRIVATE_A, NULL };
  set_private_relays(&a, private, FALSE);
  enable(&a, TRUE);
  guint decrypted = 0, incomplete = 0;
  for (guint spins = 0; spins < 2000; spins++) {
    g_main_context_iteration(NULL, TRUE);
    gh_draft_sync_get_import_state(a.sync, NULL, &decrypted, &incomplete);
    if (decrypted == GH_DRAFT_SYNC_MAX_IMPORT && incomplete == 2) break;
  }
  g_assert_cmpuint(decrypted, ==, GH_DRAFT_SYNC_MAX_IMPORT);
  g_assert_cmpuint(incomplete, ==, 2);
  g_assert_cmpuint(a.applied->len, ==, GH_DRAFT_SYNC_MAX_IMPORT);
  /* Newest first: the two oldest were left out. */
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) locals = gh_store_drafts_list_local(a.store, &error);
  g_assert_cmpuint(locals->len, ==, GH_DRAFT_SYNC_MAX_IMPORT);
  g_assert_cmpstr(gh_draft_sync_get_status(a.sync), ==, "2 drafts could not be synced");
  g_assert_cmpuint(drafts_written(), ==, 0);
  device_down(&a);
}

/* Enabling with a draft of unknown age: nothing published before EOSE; a
 * newer remote revision wins with the local text kept as a copy; a room
 * the relays know nothing about is published normally. */
static void
test_enable_reconciliation(void)
{
  relay_reset();
  relay.auto_eose = FALSE;
  Device a; device_up(&a, NULL);
  set_draft(&a, room, "old local");
  set_draft(&a, room2, "only here");
  const gchar *d = "4444444444444444444444444444444444444444444444444444444444444444";
  g_autofree gchar *remote = remote_draft(d, PEER, "newer elsewhere", T0 + 5, NULL);
  relay_store(remote);
  const gchar *private[] = { PRIVATE_A, NULL };
  set_private_relays(&a, private, FALSE);
  enable(&a, TRUE);
  advance(&a, 10 * GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  g_assert_cmpuint(drafts_written(), ==, 0); /* nothing before the import finished */
  DraftWait wait = { &a, room, "newer elsewhere" };
  gh_test_spin_until(draft_is, &wait);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) conflicts = gh_store_drafts_list_conflicts(a.store, room, &error);
  g_assert_cmpuint(conflicts->len, ==, 1);
  g_assert_cmpstr(((GhStoreDraftConflict *) conflicts->pdata[0])->text, ==, "old local");
  g_assert_cmpuint(a.applied->len, ==, 1);
  relay_eose(NOSTR_KIND_DRAFT_WRAP, PRIVATE_A);
  await_drafts(1);
  json_t *root = parse_write(write_at(0));
  g_assert_cmpstr(tag_value(root, "d"), !=, d); /* room2, a fresh identifier */
  json_decref(root);
  DraftWait second = { &a, room2, "only here" };
  gh_test_spin_until(room_clean, &second);
  g_autoptr(GhStoreDraftState) s = state(&a, room);
  g_assert_false(s->dirty); /* the old local text is never published as newest */
  g_assert_cmpuint(drafts_written(), ==, 1);
  device_down(&a);
}

/* Forgetting a conversation blanks its identifier and refuses revisions
 * older than the tombstone. */
static void
test_forget(void)
{
  relay_reset();
  Device a; device_up_synced(&a);
  gh_test_spin_until(import_complete, &a);
  edit_and_publish(&a, room, "to be forgotten");
  g_autoptr(GhStoreDraftState) before = state(&a, room);
  g_autofree gchar *d = g_strdup(before->draft_d);
  advance(&a, GH_DRAFT_SYNC_RATE_SECONDS);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_forget(a.conversations, room, &error));
  g_assert_no_error(error);
  await_drafts(2);
  json_t *root = parse_write(write_at(1));
  g_assert_cmpstr(json_string_value(json_object_get(root, "content")), ==, "");
  g_assert_cmpstr(tag_value(root, "d"), ==, d);
  json_decref(root);
  DraftWait wait = { &a, room, NULL };
  gh_test_spin_until(room_clean, &wait);
  gboolean blocked = FALSE; gint64 forgotten = 0;
  g_assert_true(gh_store_drafts_get_room(a.store, room, &blocked, &forgotten, NULL, &error));
  g_assert_cmpint(forgotten, ==, T0 + GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS + GH_DRAFT_SYNC_RATE_SECONDS);
  guint calls = signer.calls;
  g_autofree gchar *older = remote_draft(d, PEER, "before the forget", forgotten - 1, NULL);
  feed(older);
  g_autofree gchar *text = get_draft(&a, room);
  g_assert_null(text);
  gh_test_signer_assert_no_calls_since(&signer, calls);
  device_down(&a);
}

/* A signer denial stops attempts for that revision until Retry or an edit. */
static void
test_signer_denial(void)
{
  relay_reset();
  Device a; device_up_synced(&a);
  gh_test_spin_until(import_complete, &a);
  signer.deny = TRUE;
  set_draft(&a, room, "declined");
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  for (guint spins = 0; spins < 200 && g_strcmp0(gh_draft_sync_get_status(a.sync), "Saved on This Device · Signer Declined"); spins++)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpstr(gh_draft_sync_get_status(a.sync), ==, "Saved on This Device · Signer Declined");
  guint calls = signer.calls;
  advance(&a, 10 * GH_DRAFT_SYNC_RATE_SECONDS);
  gh_test_signer_assert_no_calls_since(&signer, calls); /* no automatic retry */
  g_assert_cmpuint(drafts_written(), ==, 0);
  signer.deny = FALSE;
  gh_draft_sync_retry(a.sync);
  await_drafts(1);
  g_assert_null(gh_draft_sync_get_status(a.sync));
  device_down(&a);
}

/* A relay failure retries the stored signed event without a new signature,
 * and so does a restart; a switched account publishes nothing. */
static void
test_offline_restart_and_switch(void)
{
  relay_reset();
  Device a; device_up_synced(&a);
  gh_test_spin_until(import_complete, &a);
  settle(a.social);
  relay.fail_writes = TRUE; /* the private relay is unreachable */
  set_draft(&a, room, "offline");
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  await_drafts(1);
  Write *w = write_at(0);
  g_autofree gchar *signed_json = g_strdup(w->json);
  gh_test_spin_until(status_saved_locally, &a);
  guint calls = signer.calls;
  advance(&a, GH_DRAFT_SYNC_RETRY_SECONDS);
  await_drafts(2);
  g_assert_cmpstr(((Write *) write_at(1))->json, ==, signed_json);
  gh_test_spin_until(status_saved_locally, &a);
  gh_test_signer_assert_no_calls_since(&signer, calls);
  /* A restart resends the same stored event, again without the signer. */
  device_close(&a);
  for (guint i = 0; i < relay.writes->len; i++) g_assert_true(((Write *) relay.writes->pdata[i])->closed);
  open_store(&a); bind(&a);
  gh_test_spin_until(import_complete, &a);
  calls = signer.calls;
  await_drafts(3);
  g_assert_cmpstr(((Write *) write_at(2))->json, ==, signed_json);
  gh_test_spin_until(status_saved_locally, &a);
  gh_test_signer_assert_no_calls_since(&signer, calls);
  /* Reachable again: accepted, the room is clean, nothing further is sent. */
  relay.fail_writes = FALSE;
  advance(&a, GH_DRAFT_SYNC_RETRY_SECONDS);
  await_drafts(4);
  g_assert_cmpstr(((Write *) write_at(3))->json, ==, signed_json);
  DraftWait wait = { &a, room, "offline" };
  gh_test_spin_until(room_clean, &wait);
  g_assert_null(gh_draft_sync_get_status(a.sync));
  gh_test_signer_assert_no_calls_since(&signer, calls);
  advance(&a, 10 * GH_DRAFT_SYNC_RATE_SECONDS);
  g_assert_cmpuint(drafts_written(), ==, 4);
  /* The account switches away: the binding is gone, nothing is published. */
  set_draft(&a, room, "switching");
  g_settings_set_string(a.settings, "current-npub", other_npub);
  drain();
  g_assert_false(gh_draft_sync_is_active(a.sync));
  advance(&a, 10 * GH_DRAFT_SYNC_RATE_SECONDS);
  g_assert_cmpuint(drafts_written(), ==, 4);
  device_down(&a);
}

/* Sync off keeps editing local (no signer, no relay) and records the clear
 * at send as a tombstone that a later enable publishes. */
static void
test_switch_off_and_on(void)
{
  relay_reset();
  Device a; device_up_synced(&a);
  gh_test_spin_until(import_complete, &a);
  edit_and_publish(&a, room, "while on");
  g_autoptr(GhStoreDraftState) s = state(&a, room);
  g_autofree gchar *d = g_strdup(s->draft_d);
  enable(&a, FALSE);
  g_assert_false(gh_draft_sync_is_active(a.sync));
  g_assert_null(gh_draft_sync_get_status(a.sync));
  guint calls = signer.calls;
  set_draft(&a, room, "while off");
  advance(&a, 10 * GH_DRAFT_SYNC_RATE_SECONDS);
  gh_test_signer_assert_no_calls_since(&signer, calls);
  g_assert_cmpuint(drafts_written(), ==, 1);
  g_assert_null(read_for(NOSTR_KIND_DRAFT_WRAP, PRIVATE_A));
  gint64 id = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_find_conversation(a.store, GH_STORE_BACKEND_NIP17, room, &id, &error));
  GhStoreOutgoing outgoing = { .conversation_id = id, .op_id = "00000000000000000000000000000002",
    .backend_msg_id = "0000000000000000000000000000000200000000000000000000000000000002",
    .sender_pubkey = account, .kind = 14, .created_at = T0 + 1000, .body = "sent", .rumor_json = "{\"kind\":14}" };
  g_assert_true(gh_store_enqueue(a.store, &outgoing, NULL, NULL, &error));
  g_autoptr(GhStoreDraftState) off = state(&a, room);
  g_assert_true(off->dirty); g_assert_cmpint(off->cleared_at, >, 0);
  enable(&a, TRUE);
  gh_test_spin_until(import_complete, &a);
  await_drafts(2);
  json_t *root = parse_write(write_at(1));
  g_assert_cmpstr(json_string_value(json_object_get(root, "content")), ==, "");
  g_assert_cmpstr(tag_value(root, "d"), ==, d);
  json_decref(root);
  device_down(&a);
}

/* ---- the Drafts preferences page (--gui) ------------------------------------------- */

static GtkWidget *
find_by_type(GtkWidget *widget, GType type)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_by_type(child, type);
    if (found) return found;
  }
  return NULL;
}
/* The first button under widget with that icon (the row's Discard). */
static GtkWidget *
find_icon_button(GtkWidget *widget, const gchar *icon)
{
  if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_icon_name(GTK_BUTTON(widget)), icon)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_icon_button(child, icon);
    if (found) return found;
  }
  return NULL;
}
typedef struct { Device *d; guint count; } ConflictCount;
static gboolean
conflicts_are(gpointer data)
{
  ConflictCount *want = data;
  g_autoptr(GPtrArray) list = gh_draft_sync_list_conflicts(want->d->sync, NULL);
  return list && list->len == want->count;
}
static guint
rows_of(GtkListBox *list)
{
  guint n = 0;
  for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(list)); c; c = gtk_widget_get_next_sibling(c)) n++;
  return n;
}
static gboolean page_pushed(gpointer data) { return find_by_type(data, GH_TYPE_DRAFTS_PAGE) != NULL; }
static gboolean
status_declined(gpointer data)
{
  return !g_strcmp0(gh_draft_sync_get_status(((Device *) data)->sync), "Saved on This Device · Signer Declined");
}

/* Preferences › Messages › Drafts: the row counts the copies; the page lists
 * them with Restore (back into the conversation, the displaced text kept in
 * turn) and Discard (undone by keeping it again); Retry resumes a sync the
 * signer declined. */
static void
test_drafts_page(void)
{
  relay_reset();
  Device a; device_up_synced(&a);
  gh_test_spin_until(import_complete, &a);
  settle(a.social);
  set_draft(&a, room, "typed here");
  const gchar *d1 = "1111111111111111111111111111111111111111111111111111111111111111";
  g_autofree gchar *theirs = remote_draft(d1, PEER, "typed elsewhere", T0 + 3, NULL);
  feed(theirs);
  ConflictCount one = { &a, 1 };
  gh_test_spin_until(conflicts_are, &one);

  GhPreferencesDialog *prefs = g_object_ref_sink(gh_preferences_dialog_new(a.settings, 0));
  GhDraftsBackend backend = gh_draft_sync_backend(a.sync);
  gh_preferences_dialog_set_drafts_backend(prefs, &backend);
  GtkWindow *window = GTK_WINDOW(adw_window_new());
  gtk_window_set_default_size(window, 920, 640); gtk_window_present(window);
  adw_dialog_present(ADW_DIALOG(prefs), GTK_WIDGET(window));
  adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(prefs), "messages");
  GtkWidget *row = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "drafts_row"));
  GtkWidget *group = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "drafts_group"));
  g_assert_true(gtk_widget_get_visible(group));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), ==, "1 conflict copy");
  g_signal_emit_by_name(row, "activated");
  gh_test_spin_until(page_pushed, prefs);
  GtkWidget *page = find_by_type(GTK_WIDGET(prefs), GH_TYPE_DRAFTS_PAGE);
  GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_template_child(page, GH_TYPE_DRAFTS_PAGE, "conflicts_list"));
  GtkWidget *empty = GTK_WIDGET(gtk_widget_get_template_child(page, GH_TYPE_DRAFTS_PAGE, "empty"));
  GtkWidget *retry = GTK_WIDGET(gtk_widget_get_template_child(page, GH_TYPE_DRAFTS_PAGE, "retry_button"));
  g_assert_cmpuint(rows_of(list), ==, 1);
  g_assert_false(gtk_widget_get_visible(empty));
  g_assert_false(gtk_widget_get_visible(retry));
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(list, 0);
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(first)), ==, "typed elsewhere");
  g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(first)));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(first)), "npub1"));

  /* Restore: the copy becomes the draft (and the composer's text); the text
   * it replaced is kept in turn. */
  guint applied = a.applied->len;
  g_signal_emit_by_name(adw_action_row_get_activatable_widget(ADW_ACTION_ROW(first)), "clicked");
  drain();
  g_autofree gchar *restored = get_draft(&a, room);
  g_assert_cmpstr(restored, ==, "typed elsewhere");
  assert_applied(&a, applied, room, "typed elsewhere");
  g_assert_cmpuint(rows_of(list), ==, 1);
  first = gtk_list_box_get_row_at_index(list, 0);
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(first)), ==, "typed here");
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), ==, "1 conflict copy");

  /* Discard, then its Undo's path. */
  GtkWidget *discard = find_icon_button(GTK_WIDGET(first), "user-trash-symbolic");
  g_assert_nonnull(discard);
  g_signal_emit_by_name(discard, "clicked");
  drain();
  g_assert_cmpuint(rows_of(list), ==, 0);
  g_assert_true(gtk_widget_get_visible(empty));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), ==, "Up to date");
  g_autoptr(GError) error = NULL;
  g_assert_true(backend.keep(backend.owner, room, "typed here", &error)); g_assert_no_error(error);
  drain();
  g_assert_cmpuint(rows_of(list), ==, 1);

  /* A declined signature stops the sync; the page's Retry resumes it. */
  settle(a.social);
  signer.deny = TRUE;
  set_draft(&a, room, "declined");
  advance(&a, GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS);
  gh_test_spin_until(status_declined, &a);
  drain();
  g_assert_true(gtk_widget_get_visible(retry));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), ==, "Saved on This Device · Signer Declined");
  signer.deny = FALSE;
  guint before = drafts_written();
  g_signal_emit_by_name(retry, "clicked");
  await_drafts(before + 1);
  DraftWait wait = { &a, room, "declined" };
  gh_test_spin_until(room_clean, &wait);
  drain();
  g_assert_false(gtk_widget_get_visible(retry));
  g_assert_null(gh_draft_sync_get_status(a.sync));

  adw_dialog_force_close(ADW_DIALOG(prefs)); gtk_window_destroy(window); g_object_unref(prefs); drain();
  device_down(&a);
}

int
main(int argc, char **argv)
{
  gboolean gui = argc > 1 && !g_strcmp0(argv[1], "--gui");
  if (gui) { argv[1] = NULL; argc = 1; }
  g_test_init(&argc, &argv, NULL);
  account = gh_test_pub(1); npub = gh_test_npub(1); other_npub = gh_test_npub(2);
  room = g_strdup_printf("%s,%s", account, PEER);
  room2 = g_strdup_printf("%s,%s", account, PEER2);
  if (gui) {
    nostrc_test_tolerate_gdk_frame_warning();
    if (!gtk_init_check()) {
      g_free(account); g_free(npub); g_free(other_npub); g_free(room); g_free(room2);
      return 77;
    }
    adw_init();
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    groundhog_register_resource();
    gh_test_bus_up_beside_gtk(&bus);
  } else
    gh_test_bus_up(&bus);
  gh_test_signer_up(&bus, &signer);
  /* The account's NIP-65 write relay, found on the discovery relay. */
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("r", WRITE_A, "write", NULL));
  seed = sign_json(NOSTR_KIND_RELAY_LIST_METADATA, T0 - 60, tags, "");
  relay_reset();
  if (gui) {
    g_test_add_func("/groundhog/draft-sync-gui/drafts-page", test_drafts_page);
    gint gui_result = g_test_run();
    gh_test_signer_down(&bus, &signer);
    gh_test_bus_down(&bus);
    g_free(seed); g_free(account); g_free(npub); g_free(other_npub); g_free(room); g_free(room2);
    return gui_result;
  }
  g_test_add_func("/privacy/drafts/opaque-identifiers", test_opaque_identifiers);
  g_test_add_func("/privacy/drafts/private-relays-only", test_private_relays_only);
  g_test_add_func("/groundhog/draft-sync/cadence", test_cadence);
  g_test_add_func("/groundhog/draft-sync/no-identifier-before-eose", test_no_identifier_before_eose);
  g_test_add_func("/groundhog/draft-sync/two-devices-converge", test_two_devices_converge);
  g_test_add_func("/groundhog/draft-sync/concurrent-first-creation", test_concurrent_first_creation);
  g_test_add_func("/groundhog/draft-sync/import-rules", test_import_rules);
  g_test_add_func("/groundhog/draft-sync/import-bound", test_import_bound);
  g_test_add_func("/groundhog/draft-sync/enable-reconciliation", test_enable_reconciliation);
  g_test_add_func("/groundhog/draft-sync/forget", test_forget);
  g_test_add_func("/groundhog/draft-sync/signer-denial", test_signer_denial);
  g_test_add_func("/groundhog/draft-sync/offline-restart-switch", test_offline_restart_and_switch);
  g_test_add_func("/groundhog/draft-sync/switch-off-and-on", test_switch_off_and_on);
  gint result = g_test_run();
  gh_test_signer_down(&bus, &signer);
  gh_test_bus_down(&bus);
  g_free(seed); g_free(account); g_free(npub); g_free(other_npub); g_free(room); g_free(room2);
  return result;
}
