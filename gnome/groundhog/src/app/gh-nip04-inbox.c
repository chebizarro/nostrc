#include "gh-nip04-inbox.h"
#include "gh-message.h"
#include "gh-identity.h"
#include "gh-signer.h"
#include <nostr-event.h>
#include <nostr-json.h>
#include <nostr-tag.h>
#include <nostr-filter.h>
#include <string.h>

#define KIND_NIP04 4
#define MAX_URLS 16
#define REQ_LIMIT 500

typedef struct {
  gchar *id;
  gchar *json;
  gchar *url;
} Pending;

static void
pending_free(gpointer data)
{
  Pending *p = data;
  g_free(p->id);
  g_free(p->json);
  g_free(p->url);
  g_free(p);
}

struct _GhNip04Inbox {
  GObject parent_instance;
  GhAccountController *accounts; /* weak refs kept by connections */
  GhAccountRelays *relays;
  GhConversationStore *store;
  GhDmInbox *dm_inbox;
  const GhRelayTransport *transport;
  gpointer transport_data;
  GhRelayScope *scope;      /* kind 4 addressed to the account */
  GhRelayScope *scope_out;  /* kind 4 written by the account */
  guint64 generation;
  gchar *pubkey;            /* the account's, hex */
  gchar *urls_key;          /* the relay set the scope was made for */
  GQueue queue;             /* Pending, decrypted one at a time */
  GHashTable *queued;       /* ids queued or decrypting */
  GCancellable *cancel;
  gboolean decrypting;
  gboolean paused;          /* a decryption was denied: not again this session */
  gboolean syncing;         /* starting a scope can re-enter through "changed" */
  guint admitted;
};
G_DEFINE_FINAL_TYPE(GhNip04Inbox, gh_nip04_inbox, G_TYPE_OBJECT)

static void pump(GhNip04Inbox *self);

static gboolean
lower_hex64(const gchar *s)
{
  if (!s || strlen(s) != 64)
    return FALSE;
  for (const gchar *c = s; *c; c++)
    if (!g_ascii_isxdigit(*c) || g_ascii_isupper(*c))
      return FALSE;
  return TRUE;
}

static void
stop(GhNip04Inbox *self)
{
  GhRelayScope **scopes[] = { &self->scope, &self->scope_out };
  for (guint i = 0; i < G_N_ELEMENTS(scopes); i++)
    if (*scopes[i]) {
      gh_relay_scope_cancel(*scopes[i]);
      g_clear_pointer(scopes[i], gh_relay_scope_unref);
    }
  if (self->cancel) {
    g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel);
  }
  g_queue_clear_full(&self->queue, pending_free);
  g_hash_table_remove_all(self->queued);
  self->decrypting = FALSE;
  g_clear_pointer(&self->urls_key, g_free);
}

/* The peer of a kind-4 event: its p when the account wrote it, else its
 * author when it is addressed to the account; NULL otherwise. */
static const gchar *
peer_of(GhNip04Inbox *self, NostrEvent *event, const gchar **recipient)
{
  const gchar *author = nostr_event_get_pubkey(event);
  const gchar *p = NULL;
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags) && !p; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (tag && nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), "p") == 0)
      p = nostr_tag_get(tag, 1);
  }
  if (!lower_hex64(author) || !lower_hex64(p))
    return NULL;
  *recipient = p;
  if (g_str_equal(author, self->pubkey))
    return g_str_equal(p, self->pubkey) ? NULL : p;
  return g_str_equal(p, self->pubkey) ? author : NULL;
}

static void
on_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhNip04Inbox *self = data;
  if ((scope != self->scope && scope != self->scope_out) || update->notice != GH_RELAY_NOTICE_EVENT || !update->event_json ||
      !update->event_id || !lower_hex64(update->event_id) || self->paused)
    return;
  if (g_hash_table_contains(self->queued, update->event_id) ||
      gh_conversation_store_has_wrap(self->store, update->event_id) ||
      gh_conversation_store_has_rejected(self->store, update->event_id))
    return;
  Pending *p = g_new0(Pending, 1);
  p->id = g_strdup(update->event_id);
  p->json = g_strdup(update->event_json);
  p->url = g_strdup(update->url);
  g_hash_table_add(self->queued, g_strdup(p->id));
  g_queue_push_tail(&self->queue, p);
  pump(self);
}

/* A local kind-14 rumor for the decrypted message (never published). */
static gchar *
rumor_for(NostrEvent *event, const gchar *recipient, const gchar *plaintext, const gchar *id)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_pubkey(rumor, nostr_event_get_pubkey(event));
  nostr_event_set_created_at(rumor, nostr_event_get_created_at(event));
  nostr_event_set_kind(rumor, 14);
  nostr_event_set_content(rumor, plaintext);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", recipient, NULL));
  nostr_tags_append(tags, nostr_tag_new(GH_MESSAGE_LEGACY_TAG, "nip04", id, NULL));
  nostr_event_set_tags(rumor, tags);
  gchar *json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);
  return json;
}

typedef struct {
  GhNip04Inbox *self; /* ref */
  Pending *pending;
  NostrEvent *event;
  gchar *recipient;
  guint64 generation;
} Call;

static void
call_free(Call *call)
{
  g_object_unref(call->self);
  pending_free(call->pending);
  if (call->event) nostr_event_free(call->event);
  g_free(call->recipient);
  g_free(call);
}

static void
decrypted(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Call *call = data;
  GhNip04Inbox *self = call->self;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *plaintext = gh_account_controller_nip44_finish(result, &error);
  if (call->generation != self->generation || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    call_free(call);
    return;
  }
  self->decrypting = FALSE;
  g_hash_table_remove(self->queued, call->pending->id);
  if (!plaintext) {
    /* Denied (or the signer is away): not again this session; a later one
     * asks again. A message that cannot be decrypted at all is recorded so
     * it is never asked about again. */
    if (error && g_error_matches(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT)) {
      gh_conversation_store_record_rejected(self->store, call->pending->id, NULL);
    } else {
      g_message("Groundhog: older NIP-04 messages are not shown this session: %s",
                error ? error->message : "no answer");
      self->paused = TRUE;
      g_queue_clear_full(&self->queue, pending_free);
      g_hash_table_remove_all(self->queued);
    }
    call_free(call);
    return;
  }
  g_autofree gchar *rumor = rumor_for(call->event, call->recipient, plaintext, call->pending->id);
  g_autoptr(GError) parse_error = NULL;
  g_autoptr(GhMessage) message = rumor ? gh_message_new_from_rumor(self->pubkey, rumor, &parse_error) : NULL;
  if (message) {
    gh_message_add_relay(message, call->pending->url);
    g_autoptr(GError) admit_error = NULL;
    GhConversationAddResult r = gh_conversation_store_admit(self->store, message, call->pending->id,
                                                            &admit_error);
    if (r == GH_CONVERSATION_ADD_NEW)
      self->admitted++;
    else if (r == GH_CONVERSATION_ADD_FAILED)
      g_message("Groundhog could not store a NIP-04 message: %s",
                admit_error ? admit_error->message : "?");
  } else {
    gh_conversation_store_record_rejected(self->store, call->pending->id, NULL);
  }
  call_free(call);
  pump(self);
}

static void
pump(GhNip04Inbox *self)
{
  while (!self->decrypting && !self->paused) {
    Pending *p = g_queue_pop_head(&self->queue);
    if (!p)
      return;
    gchar id[65] = { 0 };
    NostrEvent *event = nostr_event_new();
    const gchar *recipient = NULL, *peer = NULL;
    if (nostr_event_deserialize_signed(event, p->json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
        nostr_event_validate(event, id) != NOSTR_EVENT_VALIDATION_OK ||
        nostr_event_get_kind(event) != KIND_NIP04 || g_strcmp0(id, p->id) != 0 ||
        !nostr_event_get_content(event) ||
        !(peer = peer_of(self, event, &recipient))) {
      nostr_event_free(event);
      g_hash_table_remove(self->queued, p->id);
      pending_free(p);
      continue;
    }
    Call *call = g_new0(Call, 1);
    call->self = g_object_ref(self);
    call->pending = p;
    call->event = event;
    call->recipient = g_strdup(recipient);
    call->generation = self->generation;
    self->decrypting = TRUE;
    gh_account_controller_nip04_decrypt_async(self->accounts, nostr_event_get_content(event), peer,
                                              self->cancel, decrypted, call);
  }
}

/* One filter per scope (one REQ each): kind 4 addressed to (#p) or written
 * by (authors) the account. */
static NostrFilters *
filters_for(const gchar *pubkey, gint64 since, gboolean outgoing)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  const int kinds[] = { KIND_NIP04 };
  nostr_filter_set_kinds(filter, kinds, 1);
  if (outgoing) {
    const gchar *authors[] = { pubkey };
    nostr_filter_set_authors(filter, authors, 1);
  } else {
    nostr_filter_tags_append(filter, "p", pubkey, NULL);
  }
  nostr_filter_set_since_i64(filter, since);
  nostr_filter_set_limit(filter, REQ_LIMIT);
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  return filters;
}

/* Runs while the NIP-17 inbox runs, on the account's NIP-65 relays. */
static void nip04_sync_inner(GhNip04Inbox *self);

/* Starting a scope can emit the inbox's or relays' "changed" synchronously;
 * a re-entered sync would tear down and recreate the scope endlessly. */
static void
nip04_sync(GhNip04Inbox *self)
{
  if (self->syncing)
    return;
  self->syncing = TRUE;
  nip04_sync_inner(self);
  self->syncing = FALSE;
}

static void
nip04_sync_inner(GhNip04Inbox *self)
{
  GhDmInboxState state = gh_dm_inbox_get_state(self->dm_inbox);
  gboolean running = state == GH_DM_INBOX_CONNECTING || state == GH_DM_INBOX_BACKFILLING ||
                     state == GH_DM_INBOX_LIVE;
  guint64 generation = gh_account_controller_get_generation(self->accounts);
  const gchar *npub = gh_account_controller_get_active_npub(self->accounts);
  if (generation != self->generation) {
    stop(self);
    self->generation = generation;
    self->paused = FALSE;
    g_clear_pointer(&self->pubkey, g_free);
  }
  if (!running || !npub || gh_account_relays_get_generation(self->relays) != generation) {
    stop(self);
    return;
  }
  if (!self->pubkey)
    self->pubkey = gh_identity_pubkey_hex(npub);
  if (!self->pubkey)
    return;
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  const gchar *const *lists[] = { gh_account_relays_get_read_relays(self->relays),
                                  gh_account_relays_get_write_relays(self->relays) };
  for (guint l = 0; l < G_N_ELEMENTS(lists); l++)
    for (guint i = 0; lists[l] && lists[l][i] && urls->len < MAX_URLS; i++)
      if (!g_ptr_array_find_with_equal_func(urls, lists[l][i], g_str_equal, NULL))
        g_ptr_array_add(urls, g_strdup(lists[l][i]));
  g_ptr_array_add(urls, NULL);
  g_autofree gchar *key = g_strjoinv("\n", (gchar **)urls->pdata);
  if (self->scope && g_strcmp0(key, self->urls_key) == 0)
    return;
  stop(self);
  if (urls->len <= 1)
    return;
  self->cancel = g_cancellable_new();
  gint64 since = g_get_real_time() / G_USEC_PER_SEC - GH_NIP04_INBOX_BACKFILL;
  self->urls_key = g_steal_pointer(&key);
  for (guint dir = 0; dir < 2; dir++) {
    NostrFilters *filters = filters_for(self->pubkey, since, dir == 1);
    GhRelayScope *scope = self->transport
      ? gh_relay_scope_new_with_transport(generation, filters, self->transport,
                                          self->transport_data, on_update, self)
      : gh_relay_scope_new(generation, filters, on_update, self); /* owns filters */
    for (guint i = 0; i + 1 < urls->len; i++)
      gh_relay_scope_add_url(scope, g_ptr_array_index(urls, i), NULL);
    if (dir == 0) self->scope = scope; else self->scope_out = scope;
    gh_relay_scope_start(scope);
  }
}

guint
gh_nip04_inbox_get_admitted(GhNip04Inbox *self)
{
  g_return_val_if_fail(GH_IS_NIP04_INBOX(self), 0);
  return self->admitted;
}

static void
gh_nip04_inbox_dispose(GObject *object)
{
  GhNip04Inbox *self = GH_NIP04_INBOX(object);
  stop(self);
  g_clear_object(&self->dm_inbox);
  g_clear_object(&self->relays);
  g_clear_object(&self->accounts);
  g_clear_object(&self->store);
  G_OBJECT_CLASS(gh_nip04_inbox_parent_class)->dispose(object);
}

static void
gh_nip04_inbox_finalize(GObject *object)
{
  GhNip04Inbox *self = GH_NIP04_INBOX(object);
  g_hash_table_unref(self->queued);
  g_free(self->pubkey);
  G_OBJECT_CLASS(gh_nip04_inbox_parent_class)->finalize(object);
}

static void
gh_nip04_inbox_class_init(GhNip04InboxClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_nip04_inbox_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_nip04_inbox_finalize;
}

static void
gh_nip04_inbox_init(GhNip04Inbox *self)
{
  g_queue_init(&self->queue);
  self->queued = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

GhNip04Inbox *
gh_nip04_inbox_new(GhAccountController *accounts, GhAccountRelays *relays,
                   GhConversationStore *store, GhDmInbox *dm_inbox,
                   const GhRelayTransport *transport, gpointer transport_data)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  GhNip04Inbox *self = g_object_new(GH_TYPE_NIP04_INBOX, NULL);
  self->accounts = g_object_ref(accounts);
  self->relays = g_object_ref(relays);
  self->store = g_object_ref(store);
  self->dm_inbox = g_object_ref(dm_inbox);
  self->transport = transport;
  self->transport_data = transport_data;
  g_signal_connect_object(dm_inbox, "changed", G_CALLBACK(nip04_sync), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(relays, "changed", G_CALLBACK(nip04_sync), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(accounts, "changed", G_CALLBACK(nip04_sync), self, G_CONNECT_SWAPPED);
  nip04_sync(self);
  return self;
}
