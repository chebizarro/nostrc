/* NIP-37 draft synchronization (plan §3.7, nostrc-9lwl7.8). See the header
 * for the contract. Nothing here logs draft text, identifiers, ciphertext or
 * signer payloads (charter §3.4 failure semantics). */
#include "gh-draft-sync.h"

#include "gh-auth-policy.h"
#include "gh-outbox.h"
#include "gh-relay-deny-policy.h"
#include "gh-relay-publish.h"
#include "gh-relay-scope.h"

#include <glib/gi18n.h>
#include <nostr-event.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <nostr/nip37/nip37.h>
#include <string.h>

/* Remote revisions buffered for decryption, and blanks remembered for
 * identifiers no room here knows yet. */
#define MAX_INCOMING 256
#define MAX_ORPHAN_BLANKS 256

/* Per-room scheduling memory (the durable state is the store's). */
typedef struct {
  gint64 last_edit_us;      /* monotonic; 0 = unknown (pending from before this binding) */
  gint64 last_publish_us;   /* monotonic; 0 = none this binding */
  gint64 retry_at_us;       /* monotonic; after a relay failure */
  gint64 denied_revision;   /* the signer declined this local revision */
  gboolean flush;           /* the composer left the room: due now */
  gboolean reconcile;       /* a draft of unknown age awaiting the import */
} Room;

/* A remote revision awaiting decryption (newest first). While its decryption
 * is in flight it belongs to the callback, which checks the binding epoch. */
typedef struct {
  GhDraftSync *self;        /* ref'd while in flight */
  guint epoch;
  gchar *identifier, *event_id, *content;
  gint64 created_at, expires_at;
} Incoming;

/* A blank seen for an identifier before any room was known for it. */
typedef struct {
  gchar *event_id;
  gint64 created_at, expires_at;
} OrphanBlank;

/* One encrypt/sign pipeline: the room's head text, or blanks for ids. It is
 * owned by whichever signer callback is in flight; close detaches it. */
typedef struct {
  GhDraftSync *self;        /* ref'd */
  gchar *room_key;
  gint64 revision;
  GStrv ids;                /* identifiers to publish under */
  guint index;
  gchar *text;              /* NULL: blanks */
  gchar *cipher;
  gboolean alias_blanks;    /* blanking aliases after an accepted head */
  guint n_head;             /* ids[0 .. n_head) are the head identifier; the rest aliases */
  gint64 last_created_at;
  gboolean detached;        /* the binding went away: the callback frees it */
} Job;

/* A signed event on its way to the private relays. */
typedef struct {
  GhDraftSync *self;        /* ref'd */
  GhRelayPublish *publish;
  gchar *room_key, *d, *event_id;
  gint64 revision, created_at, expires_at;
  gboolean alias;           /* an alias blank: remove the alias on acceptance */
  gboolean accepted;
  GString *outcomes;        /* {"url":"outcome",...} */
  gchar *failure;           /* first destination-level failure, for status */
  guint cleanup_id;         /* the idle that frees it after done */
} Publication;

struct _GhDraftSync {
  GObject parent_instance;
  GhSocialLists *social;
  GhListAdapterNetwork network;
  GhAccountStore *account_store;
  GhStore *store;                      /* borrowed while bound */
  GhStoreConversations *conversations; /* ref'd; "draft-changed" */
  GObject *outbox;                     /* ref'd; "enqueued" */
  GhClock *clock;                      /* ref'd from the store */
  gchar *account;
  guint64 generation;
  guint epoch;                         /* bumped by close and by the switch going off */
  gboolean enabled;                    /* the sync switch as last seen */
  GCancellable *cancel;                /* this binding's signer calls */
  /* Reading */
  GhRelayScope *scope;
  GStrv targets;                       /* current, unblocked kind-10013 relays */
  GHashTable *eose_pending;            /* url set */
  gboolean eose_all;
  GPtrArray *incoming;                 /* Incoming, newest first */
  gboolean decrypting;
  guint decrypted, incomplete;
  GHashTable *seen;                    /* event ids handled or queued this binding */
  GHashTable *orphan_blanks;           /* identifier -> OrphanBlank */
  /* Writing */
  GHashTable *rooms;                   /* room_key -> Room */
  guint timer;
  Job *job;
  GPtrArray *publications;             /* Publication */
  gchar *shown_room;
  gboolean signer_waiting;
  gchar *failure;                      /* last destination-level failure */
  /* Window */
  GhDraftSyncApplier applier;
  gpointer applier_data;
  gchar *status;
};

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhDraftSync, gh_draft_sync, G_TYPE_OBJECT)

static void schedule(GhDraftSync *self);
static void pump_decrypt(GhDraftSync *self);
static void refresh_status(GhDraftSync *self);
static void open_scope(GhDraftSync *self);
static void handle_blank(GhDraftSync *self, const gchar *identifier, const gchar *event_id,
                         gint64 created_at, gint64 expires_at);

/* ---- helpers -------------------------------------------------------------------------- */

static gboolean
bound(GhDraftSync *self)
{
  return self->store != NULL && self->network.accounts != NULL &&
         gh_account_controller_is_current(self->network.accounts, self->generation);
}

static gboolean
sync_enabled(GhDraftSync *self)
{
  return bound(self) && gh_social_lists_get_sync_enabled(self->social) &&
         !gh_store_is_ephemeral(self->store) && !gh_store_is_read_only(self->store);
}

static gboolean
active(GhDraftSync *self)
{
  return sync_enabled(self) && self->targets && self->targets[0];
}

/* Every relay answered EOSE and every stored revision was decrypted: only
 * then is what the relays hold known (new identifiers, reconciliation). */
static gboolean
import_settled(GhDraftSync *self)
{
  return self->eose_all && !self->decrypting && self->incoming->len == 0;
}

static gboolean
remote_signer(GhDraftSync *self)
{
  return self->network.accounts &&
         gh_account_controller_get_active_backend(self->network.accounts) == GH_SIGNER_BACKEND_NIP46;
}

static gint64
now_unix(GhDraftSync *self)
{
  return gh_clock_get_unix(self->clock);
}

static gint64
now_mono(GhDraftSync *self)
{
  return gh_clock_get_monotonic_time(self->clock);
}

static Room *
room_get(GhDraftSync *self, const gchar *room_key)
{
  Room *room = g_hash_table_lookup(self->rooms, room_key);
  if (!room) {
    room = g_new0(Room, 1);
    g_hash_table_insert(self->rooms, g_strdup(room_key), room);
  }
  return room;
}

/* NIP-01 ordering: greatest created_at, then lowest id, wins. */
static gboolean
newer(gint64 created_at, const gchar *id, gint64 than_created_at, const gchar *than_id)
{
  if (created_at != than_created_at)
    return created_at > than_created_at;
  return than_id != NULL && g_strcmp0(id, than_id) < 0;
}

static void
incoming_free(gpointer data)
{
  Incoming *in = data;
  g_clear_object(&in->self);
  g_free(in->identifier);
  g_free(in->event_id);
  g_free(in->content);
  g_free(in);
}

static void
orphan_blank_free(gpointer data)
{
  OrphanBlank *blank = data;
  g_free(blank->event_id);
  g_free(blank);
}

static void
job_free(Job *job)
{
  if (!job)
    return;
  g_clear_object(&job->self);
  g_free(job->room_key);
  g_strfreev(job->ids);
  g_free(job->text);
  g_free(job->cipher);
  g_free(job);
}

/* The binding is gone or the job is superseded: whichever signer callback is
 * in flight frees it. */
static void
detach_job(GhDraftSync *self)
{
  if (!self->job)
    return;
  self->job->detached = TRUE;
  self->job = NULL;
}

static void
publication_free(Publication *pub)
{
  if (pub->cleanup_id)
    g_source_remove(pub->cleanup_id);
  g_clear_pointer(&pub->publish, gh_relay_publish_unref);
  g_free(pub->room_key);
  g_free(pub->d);
  g_free(pub->event_id);
  g_free(pub->failure);
  if (pub->outcomes)
    g_string_free(pub->outcomes, TRUE);
  g_clear_object(&pub->self);
  g_free(pub);
}

static void
set_failure(GhDraftSync *self, const gchar *failure)
{
  if (!g_strcmp0(self->failure, failure))
    return;
  g_free(self->failure);
  self->failure = g_strdup(failure);
}

/* Queued revisions go, and their ids may come again from a relay. */
static void
drop_incoming(GhDraftSync *self)
{
  for (guint i = 0; i < self->incoming->len; i++)
    g_hash_table_remove(self->seen, ((Incoming *) self->incoming->pdata[i])->event_id);
  g_ptr_array_set_size(self->incoming, 0);
}

/* ---- targets: the current, unblocked kind-10013 relays ------------------------------ */

static GStrv
current_targets(GhDraftSync *self)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  GNostrListService *service = sync_enabled(self) ? gh_social_lists_get_service(self->social) : NULL;
  g_autoptr(GNostrListDocument) snapshot =
    service ? gnostr_list_service_lookup(service, NOSTR_KIND_PRIVATE_RELAY_LIST, "") : NULL;
  if (snapshot && gnostr_list_document_get_private_state(snapshot) != GNOSTR_LIST_PRIVATE_UNREADABLE) {
    const NostrListDocument *codec = gnostr_list_document_get_codec(snapshot);
    for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, TRUE); i++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, TRUE, i);
      const gchar *url = nostr_nip51_tag_field(tag, 1);
      if (g_strcmp0(nostr_nip51_tag_field(tag, 0), "relay") || !url || !*url)
        continue;
      /* A relay the account blocked (kind 10006) is never a draft
       * destination, and never a reason to fall back elsewhere (PD-4). */
      if (!gh_relay_deny_policy_check(self->generation, url, FALSE, NULL))
        continue;
      g_strv_builder_add(builder, url);
    }
  }
  return g_strv_builder_end(builder);
}

/* ---- reading --------------------------------------------------------------------------- */

static void
close_scope(GhDraftSync *self)
{
  if (self->scope) {
    gh_relay_scope_cancel(self->scope);
    g_clear_pointer(&self->scope, gh_relay_scope_unref);
  }
  g_hash_table_remove_all(self->eose_pending);
}

/* The import is complete: drafts of unknown age the relays knew nothing
 * about are published normally (§3.7 "Enabling sync"). */
static void
settle_import(GhDraftSync *self)
{
  if (!import_settled(self))
    return;
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->rooms);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    Room *room = value;
    if (!room->reconcile)
      continue;
    room->reconcile = FALSE;
    g_autoptr(GError) error = NULL;
    if (!gh_store_drafts_mark_dirty(self->store, key, &error))
      g_warning("Groundhog could not schedule a draft for sync: %s", error->message);
  }
  schedule(self);
  refresh_status(self);
}

static void
scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhDraftSync *self = data;
  if (scope != self->scope || !active(self))
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT: {
    if (!update->event_id || g_hash_table_contains(self->seen, update->event_id))
      return;
    NostrEvent *event = nostr_event_new();
    NostrNip37Envelope envelope = { 0 };
    gboolean ok = nostr_event_deserialize_signed(event, update->event_json, NULL) ==
                    NOSTR_EVENT_VALIDATION_OK &&
                  nostr_event_get_kind(event) == NOSTR_KIND_DRAFT_WRAP &&
                  !g_strcmp0(nostr_event_get_pubkey(event), self->account) &&
                  nostr_nip37_parse_envelope(event, &envelope) == 0 &&
                  envelope.draft_kind == NOSTR_KIND_DIRECT_MESSAGE &&
                  envelope.identifier && *envelope.identifier;
    if (!ok) {
      nostr_event_free(event);
      return; /* not an own draft wrap: nothing to count or report */
    }
    gint64 created_at = nostr_event_get_created_at(event);
    gint64 expires_at = envelope.has_expiration ? envelope.expiration : 0;
    if (envelope.has_expiration && envelope.expiration <= now_unix(self)) {
      nostr_event_free(event);
      return; /* expired: ignored, never blanked */
    }
    g_hash_table_add(self->seen, g_strdup(update->event_id));
    if (envelope.deleted) {
      handle_blank(self, envelope.identifier, update->event_id, created_at, expires_at);
      nostr_event_free(event);
      return;
    }
    Incoming *in = g_new0(Incoming, 1);
    in->identifier = g_strdup(envelope.identifier);
    in->event_id = g_strdup(update->event_id);
    in->content = g_strdup(nostr_event_get_content(event));
    in->created_at = created_at;
    in->expires_at = expires_at;
    nostr_event_free(event);
    guint at = 0;
    while (at < self->incoming->len) {
      Incoming *other = self->incoming->pdata[at];
      if (newer(in->created_at, in->event_id, other->created_at, other->event_id))
        break;
      at++;
    }
    if (self->incoming->len >= MAX_INCOMING) {
      /* Bounded buffering: the oldest waiting revision is reported, not kept. */
      Incoming *oldest = self->incoming->pdata[self->incoming->len - 1];
      g_hash_table_remove(self->seen, oldest->event_id);
      g_ptr_array_remove_index(self->incoming, self->incoming->len - 1);
      self->incomplete++;
      if (at > self->incoming->len)
        at = self->incoming->len;
    }
    g_ptr_array_insert(self->incoming, at, in);
    pump_decrypt(self);
    return;
  }
  case GH_RELAY_NOTICE_EOSE:
    if (g_hash_table_remove(self->eose_pending, update->url) &&
        g_hash_table_size(self->eose_pending) == 0 && !self->eose_all) {
      self->eose_all = TRUE;
      settle_import(self);
    }
    return;
  case GH_RELAY_NOTICE_AUTH:
    return; /* a challenge, answered by the policy's signer; not a failure */
  case GH_RELAY_NOTICE_CLOSED:
  case GH_RELAY_NOTICE_ERROR: {
    /* A private relay refusing the subscription (e.g. AUTH declined) is
     * reported as its exact destination-level failure; drafts stay local
     * (the import never completes without it) until Retry. */
    g_autofree gchar *failure = g_strdup_printf("%s: %s", update->url,
      update->detail && *update->detail ? update->detail : "subscription refused");
    set_failure(self, failure);
    refresh_status(self);
    return;
  }
  default:
    return;
  }
}

static void
open_scope(GhDraftSync *self)
{
  close_scope(self);
  self->eose_all = FALSE;
  if (!active(self))
    return;
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  const char *authors[] = { self->account };
  int kinds[] = { NOSTR_KIND_DRAFT_WRAP };
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_set_kinds(filter, kinds, 1);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("k", "14", NULL));
  nostr_filter_set_tags(filter, tags);
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  self->scope = self->network.scope_transport
    ? gh_relay_scope_new_with_transport(self->generation, filters, self->network.scope_transport,
                                        self->network.scope_data, scope_update, self)
    : gh_relay_scope_new(self->generation, filters, scope_update, self);
  if (self->network.scope_auth_transport)
    gh_relay_scope_set_auth_transport(self->scope, self->network.scope_auth_transport);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->network.accounts);
  for (guint i = 0; self->targets[i]; i++) {
    g_autoptr(GError) error = NULL;
    /* The caller guarantees these are current account kind-10013 relays
     * (gh-auth-policy.h, the SELF_WRAP precedent): account AUTH on them only. */
    if (!gh_relay_scope_add_url(self->scope, self->targets[i], &error) ||
        !gh_auth_policy_apply_scope(policy, self->scope, GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_READ,
                                    self->targets[i], &error)) {
      g_warning("Groundhog could not subscribe to a private draft relay: %s", error->message);
      continue;
    }
    g_hash_table_add(self->eose_pending, g_strdup(self->targets[i]));
  }
  if (g_hash_table_size(self->eose_pending) == 0) {
    close_scope(self);
    return;
  }
  gh_relay_scope_start(self->scope);
}

/* ---- import rules ---------------------------------------------------------------------- */

static gboolean
room_excluded(GhDraftSync *self, const gchar *room_key, gint64 created_at, gchar **out_draft)
{
  gboolean blocked = FALSE;
  gint64 forgotten_before = 0;
  g_autoptr(GError) error = NULL;
  if (!gh_store_drafts_get_room(self->store, room_key, &blocked, &forgotten_before, out_draft, &error)) {
    g_warning("Groundhog could not read a room for draft sync: %s", error->message);
    return TRUE;
  }
  return blocked || created_at <= forgotten_before;
}

static gboolean
composer_focused(GhDraftSync *self, const gchar *room_key)
{
  return self->applier.composer_focused && !g_strcmp0(self->shown_room, room_key) &&
         self->applier.composer_focused(room_key, self->applier_data);
}

static void
apply_to_window(GhDraftSync *self, const gchar *room_key, const gchar *text)
{
  if (self->applier.apply)
    self->applier.apply(room_key, text, self->applier_data);
}

/* The import table of §3.7 for a revision (text NULL: a clear) that is
 * already known to be newer than the room's last known revision. */
static void
apply_revision(GhDraftSync *self, const gchar *room_key, const gchar *text, const gchar *event_id,
               gint64 created_at, gint64 expires_at, const gchar *local, gboolean dirty)
{
  g_autoptr(GError) error = NULL;
  Room *room = room_get(self, room_key);
  gboolean same = !g_strcmp0(local, text);
  gboolean ok;
  if (same) {
    ok = gh_store_drafts_apply_remote(self->store, room_key, text, event_id, created_at, expires_at,
                                      FALSE, &error);
    if (ok && room->reconcile) {
      room->reconcile = FALSE;
      ok = gh_store_drafts_mark_clean(self->store, room_key, &error); /* the relays hold it */
    }
  } else if (room->reconcile) {
    /* Enable-time reconciliation: the local draft's age is unknown, so the
     * remote revision wins and the local text is kept, never published. */
    room->reconcile = FALSE;
    ok = (!local || gh_store_drafts_add_conflict(self->store, room_key, local, event_id,
                                                  now_unix(self), &error)) &&
         gh_store_drafts_apply_remote(self->store, room_key, text, event_id, created_at, expires_at,
                                      TRUE, &error) &&
         gh_store_drafts_mark_clean(self->store, room_key, &error);
    if (ok)
      apply_to_window(self, room_key, text);
  } else if ((dirty && local) || composer_focused(self, room_key)) {
    /* Keep what is being written here; the remote text becomes a copy. */
    ok = (!text || gh_store_drafts_add_conflict(self->store, room_key, text, event_id,
                                                 now_unix(self), &error)) &&
         gh_store_drafts_apply_remote(self->store, room_key, text, event_id, created_at, expires_at,
                                      FALSE, &error);
  } else {
    ok = gh_store_drafts_apply_remote(self->store, room_key, text, event_id, created_at, expires_at,
                                      TRUE, &error);
    if (ok)
      apply_to_window(self, room_key, text);
  }
  if (!ok)
    g_warning("Groundhog could not apply a synced draft: %s", error->message);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static void
remember_orphan_blank(GhDraftSync *self, const gchar *identifier, const gchar *event_id,
                      gint64 created_at, gint64 expires_at)
{
  OrphanBlank *known = g_hash_table_lookup(self->orphan_blanks, identifier);
  if (known && !newer(created_at, event_id, known->created_at, known->event_id))
    return;
  if (!known && g_hash_table_size(self->orphan_blanks) >= MAX_ORPHAN_BLANKS)
    return;
  OrphanBlank *blank = g_new0(OrphanBlank, 1);
  blank->event_id = g_strdup(event_id);
  blank->created_at = created_at;
  blank->expires_at = expires_at;
  g_hash_table_replace(self->orphan_blanks, g_strdup(identifier), blank);
}

static void
handle_blank(GhDraftSync *self, const gchar *identifier, const gchar *event_id, gint64 created_at,
             gint64 expires_at)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *room_key = gh_store_drafts_find_room(self->store, identifier, &error);
  if (!room_key) {
    /* Nothing here knows the identifier yet; a revision under it may still
     * be decrypting, so the blank is kept to order against it. */
    remember_orphan_blank(self, identifier, event_id, created_at, expires_at);
    return;
  }
  g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(self->store, room_key, &error);
  if (!state)
    return;
  if (g_strcmp0(state->draft_d, identifier)) {
    /* Another device retired one of the room's identifiers after a newer
     * head was accepted (§3.7 rule 6): not a clear of the room. */
    if (!gh_store_drafts_remove_alias(self->store, room_key, identifier, &error))
      g_warning("Groundhog could not retire a draft identifier: %s", error->message);
    return;
  }
  if (!newer(created_at, event_id, state->last_created_at, state->last_event_id) ||
      created_at <= state->cleared_at)
    return;
  g_autofree gchar *local = NULL;
  if (room_excluded(self, room_key, created_at, &local))
    return;
  apply_revision(self, room_key, NULL, event_id, created_at, expires_at, local, state->dirty);
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *) a, *(const gchar *const *) b);
}

/* The canonical NIP-17 room key of an unsigned kind-14 event of the account:
 * its author and p tags, sorted, unique, ','-joined; NULL when invalid. */
static gchar *
room_key_of_draft(GhDraftSync *self, NostrEvent *draft)
{
  if (nostr_event_get_kind(draft) != NOSTR_KIND_DIRECT_MESSAGE ||
      g_strcmp0(nostr_event_get_pubkey(draft), self->account))
    return NULL;
  g_autoptr(GPtrArray) members = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(members, g_strdup(self->account));
  NostrTags *tags = nostr_event_get_tags(draft);
  gboolean has_recipient = FALSE;
  for (gsize i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get(tag, 0), "p"))
      continue;
    const gchar *pubkey = nostr_tag_get(tag, 1);
    if (!pubkey || strlen(pubkey) != 64)
      return NULL;
    for (guint j = 0; j < 64; j++)
      if (!g_ascii_isxdigit(pubkey[j]) || g_ascii_isupper(pubkey[j]))
        return NULL;
    has_recipient = TRUE;
    gboolean known = FALSE;
    for (guint j = 0; j < members->len; j++)
      known = known || !strcmp(members->pdata[j], pubkey);
    if (!known)
      g_ptr_array_add(members, g_strdup(pubkey));
  }
  if (!has_recipient || members->len > GH_CONVERSATION_MAX_PEERS + 1)
    return NULL;
  g_ptr_array_sort(members, compare_strings);
  g_ptr_array_add(members, NULL);
  return g_strjoinv(",", (gchar **) members->pdata);
}

static void
handle_text(GhDraftSync *self, Incoming *in, const gchar *plaintext, gsize length)
{
  NostrEvent *draft = nostr_nip37_parse_draft(plaintext, length);
  g_autofree gchar *room_key = draft ? room_key_of_draft(self, draft) : NULL;
  if (!room_key) {
    self->incomplete++; /* malformed, signed, foreign or roomless: never blanked */
    nostr_event_free(draft);
    return;
  }
  g_autofree gchar *text = g_strdup(nostr_event_get_content(draft));
  nostr_event_free(draft);
  if (!text || !*text) {
    self->incomplete++;
    return;
  }
  g_autofree gchar *local = NULL;
  if (room_excluded(self, room_key, in->created_at, &local))
    return;
  g_autoptr(GError) error = NULL;
  /* The identifier is the room's from now on, whatever the revision's age:
   * a second one (concurrent first creation) becomes an alias of the lowest,
   * which carries the next local revision, and is blanked once that head
   * is accepted (§3.7 rule 6). */
  if (!gh_store_drafts_add_identifier(self->store, room_key, in->identifier, &error)) {
    g_warning("Groundhog could not record a draft identifier: %s", error->message);
    return;
  }
  /* A blank for this identifier that arrived before any room was known for
   * it orders against this revision now. */
  OrphanBlank *orphan = g_hash_table_lookup(self->orphan_blanks, in->identifier);
  if (orphan) {
    g_autofree gchar *blank_id = g_strdup(orphan->event_id);
    gint64 blank_at = orphan->created_at, blank_expires = orphan->expires_at;
    g_hash_table_remove(self->orphan_blanks, in->identifier);
    if (newer(blank_at, blank_id, in->created_at, in->event_id)) {
      handle_blank(self, in->identifier, blank_id, blank_at, blank_expires);
      return;
    }
  }
  g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(self->store, room_key, &error);
  if (!state) {
    if (error)
      g_warning("Groundhog could not read draft sync state: %s", error->message);
    return;
  }
  if (!newer(in->created_at, in->event_id, state->last_created_at, state->last_event_id) ||
      in->created_at <= state->cleared_at)
    return;
  apply_revision(self, room_key, text, in->event_id, in->created_at, in->expires_at, local,
                 state->dirty);
}

static void
decrypted(GObject *source, GAsyncResult *result, gpointer data)
{
  (void) source;
  Incoming *in = data;
  GhDraftSync *self = in->self;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *plaintext = gh_account_controller_nip44_finish(result, &error);
  if (in->epoch != self->epoch) {
    incoming_free(in); /* an earlier binding's: nothing of it is used */
    return;
  }
  self->decrypting = FALSE;
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
      g_error_matches(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED) || !bound(self)) {
    incoming_free(in);
    return;
  }
  self->decrypted++;
  if (plaintext)
    handle_text(self, in, plaintext, strlen(plaintext));
  else
    self->incomplete++; /* undecryptable: left on the relay as it is */
  incoming_free(in);
  pump_decrypt(self);
  settle_import(self);
  refresh_status(self);
}

static void
pump_decrypt(GhDraftSync *self)
{
  if (self->decrypting || self->incoming->len == 0 || !active(self))
    return;
  if (self->decrypted >= GH_DRAFT_SYNC_MAX_IMPORT) {
    /* Beyond the bound the rest is reported, not read: nothing is blanked. */
    self->incomplete += self->incoming->len;
    g_ptr_array_set_size(self->incoming, 0);
    settle_import(self);
    refresh_status(self);
    return;
  }
  /* The item travels with its callback: a newer revision arriving meanwhile
   * cannot take its place at the head of the queue. */
  Incoming *in = g_ptr_array_steal_index(self->incoming, 0);
  in->self = g_object_ref(self);
  in->epoch = self->epoch;
  self->decrypting = TRUE;
  gh_account_controller_nip44_decrypt_with_cancellable_async(self->network.accounts, in->content,
    self->account, self->cancel, decrypted, in);
}

/* ---- publishing ------------------------------------------------------------------------ */

static gboolean
publication_cleanup_idle(gpointer data)
{
  Publication *pub = data;
  GhDraftSync *self = g_object_ref(pub->self);
  pub->cleanup_id = 0;
  g_ptr_array_remove_fast(self->publications, pub);
  publication_free(pub);
  schedule(self);
  refresh_status(self);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

/* A cancelled publish reports nothing more (gh-relay-publish.h): its record
 * goes here, with any cleanup idle it had queued. Never from inside one of
 * its own callbacks. */
static void
cancel_publications(GhDraftSync *self)
{
  while (self->publications->len) {
    Publication *pub = g_ptr_array_steal_index_fast(self->publications, 0);
    gh_relay_publish_cancel(pub->publish);
    publication_free(pub);
  }
}

static const gchar *
outcome_name(const GhRelayPublishResult *result)
{
  switch (result->outcome) {
  case GH_RELAY_PUBLISH_ACCEPTED: return "accepted";
  case GH_RELAY_PUBLISH_REJECTED: return "rejected";
  case GH_RELAY_PUBLISH_AUTH_REQUIRED: return "auth-required";
  case GH_RELAY_PUBLISH_CONNECTION_FAILED: return "connection-failed";
  case GH_RELAY_PUBLISH_BLOCKED_BY_LOCAL_POLICY: return "blocked";
  case GH_RELAY_PUBLISH_CANCELLED: return "cancelled";
  default: return "pending";
  }
}

static void
publication_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  (void) publish;
  Publication *pub = data;
  GhDraftSync *self = pub->self;
  g_string_append_printf(pub->outcomes, "%s\"%s\":\"%s\"", pub->outcomes->len > 1 ? "," : "",
                         result->url, outcome_name(result));
  if (result->outcome == GH_RELAY_PUBLISH_ACCEPTED) {
    if (pub->accepted || !bound(self))
      return;
    pub->accepted = TRUE;
    g_autoptr(GError) error = NULL;
    gboolean ok;
    if (pub->alias)
      ok = gh_store_drafts_delete_publication(self->store, pub->room_key, pub->d, &error) &&
           gh_store_drafts_remove_alias(self->store, pub->room_key, pub->d, &error);
    else {
      g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(self->store, pub->room_key, &error);
      if (state && g_strcmp0(state->draft_d, pub->d)) {
        /* A lower identifier was learned meanwhile: this went out under what
         * is now an alias. The room stays dirty, so the text is published
         * again under the head, after which the alias is blanked. */
        ok = gh_store_drafts_delete_publication(self->store, pub->room_key, pub->d, &error);
      } else {
        gboolean current = FALSE;
        ok = gh_store_drafts_accept(self->store, pub->room_key, pub->d, pub->revision, pub->event_id,
                                    pub->created_at, pub->expires_at, &current, &error);
        /* A new head accepted: the room's other identifiers are retired,
         * durably, so a restart still owes the blanks. */
        for (guint i = 0; ok && current && state && state->aliases[i]; i++)
          ok = gh_store_drafts_queue_blank(self->store, pub->room_key, state->aliases[i],
                                           pub->revision, &error);
      }
    }
    if (!ok)
      g_warning("Groundhog could not record an accepted draft publication: %s", error->message);
    set_failure(self, NULL);
    return;
  }
  if (!pub->failure && result->outcome != GH_RELAY_PUBLISH_CANCELLED)
    pub->failure = g_strdup_printf("%s: %s", result->url,
      result->message && *result->message ? result->message : outcome_name(result));
}

static void
publication_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  (void) publish; (void) summary;
  Publication *pub = data;
  GhDraftSync *self = pub->self;
  if (!pub->accepted && bound(self)) {
    g_autoptr(GError) error = NULL;
    g_autofree gchar *outcomes = g_strdup_printf("%s}", pub->outcomes->str);
    if (!gh_store_drafts_set_outcomes(self->store, pub->room_key, pub->d, outcomes, &error))
      g_warning("Groundhog could not record draft publication outcomes: %s", error->message);
    /* The stored signed event is retried later as it is: no new signature. */
    room_get(self, pub->room_key)->retry_at_us =
      now_mono(self) + (gint64) GH_DRAFT_SYNC_RETRY_SECONDS * G_USEC_PER_SEC;
    if (pub->failure)
      set_failure(self, pub->failure);
  }
  pub->cleanup_id = g_idle_add(publication_cleanup_idle, pub);
}

/* Starts the publish of a signed event (freshly signed, or stored). */
static gboolean
publish_event(GhDraftSync *self, const gchar *room_key, const gchar *d, gint64 revision,
              const gchar *event_json, gboolean alias, GError **error)
{
  NostrEvent *event = nostr_event_new();
  if (nostr_event_deserialize_signed(event, event_json, NULL) != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Stored draft event is invalid");
    return FALSE;
  }
  NostrNip37Envelope envelope = { 0 };
  nostr_nip37_parse_envelope(event, &envelope);
  Publication *pub = g_new0(Publication, 1);
  pub->self = g_object_ref(self);
  pub->room_key = g_strdup(room_key);
  pub->d = g_strdup(d);
  pub->revision = revision;
  pub->alias = alias;
  pub->created_at = nostr_event_get_created_at(event);
  pub->expires_at = envelope.has_expiration ? envelope.expiration : 0;
  char *id = nostr_event_get_id(event);
  pub->event_id = g_strdup(id);
  free(id);
  nostr_event_free(event);
  pub->outcomes = g_string_new("{");
  pub->publish = self->network.publish_transport
    ? gh_relay_publish_new_with_transport(self->generation, event_json, self->network.publish_transport,
                                          self->network.publish_data, publication_update,
                                          publication_done, pub, error)
    : gh_relay_publish_new(self->generation, event_json, publication_update, publication_done, pub,
                           error);
  if (!pub->publish) {
    publication_free(pub);
    return FALSE;
  }
  if (self->network.publish_auth_transport)
    gh_relay_publish_set_auth_transport(pub->publish, self->network.publish_auth_transport);
  if (self->network.publish_deadline)
    gh_relay_publish_set_deadline(pub->publish, self->network.publish_deadline);
  /* No quick reconnects: a failed relay is retried from the stored event
   * after GH_DRAFT_SYNC_RETRY_SECONDS, which is also the status's cadence. */
  gh_relay_publish_set_retries(pub->publish, 0, 0);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->network.accounts);
  for (guint i = 0; self->targets[i]; i++)
    /* Current account kind-10013 relays only (the caller's guarantee of
     * gh-auth-policy.h); the deny list already left the blocked ones out. */
    if (!gh_relay_publish_add_url(pub->publish, self->targets[i], error) ||
        !gh_auth_policy_apply_publish(policy, pub->publish, GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_PUBLISH,
                                      self->targets[i], error)) {
      publication_free(pub);
      return FALSE;
    }
  g_ptr_array_add(self->publications, pub);
  if (!gh_relay_publish_start(pub->publish, error)) {
    g_ptr_array_remove_fast(self->publications, pub);
    publication_free(pub);
    return FALSE;
  }
  room_get(self, room_key)->last_publish_us = now_mono(self);
  return TRUE;
}

/* A publication could not even start: the stored signed event waits for the
 * retry cadence rather than being tried (or re-signed) at once. */
static void
publish_failed_now(GhDraftSync *self, const gchar *room_key, const GError *error)
{
  g_warning("Groundhog could not publish a draft revision: %s", error->message);
  set_failure(self, error->message);
  room_get(self, room_key)->retry_at_us =
    now_mono(self) + (gint64) GH_DRAFT_SYNC_RETRY_SECONDS * G_USEC_PER_SEC;
}

/* ---- the encrypt/sign pipeline ------------------------------------------------------ */

static void job_next(GhDraftSync *self);

static void
job_end(GhDraftSync *self)
{
  job_free(g_steal_pointer(&self->job));
  schedule(self);
  refresh_status(self);
}

/* Whether the job's revision (and, for a text, its head identifier) is still
 * the room's: a revision or head changed during encryption or signing
 * discards the result (§3.7). */
static gboolean
job_current(GhDraftSync *self, Job *job)
{
  g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(self->store, job->room_key, NULL);
  if (!state || state->local_revision != job->revision)
    return FALSE;
  if (job->alias_blanks)
    return !state->dirty;
  if (!state->dirty)
    return FALSE;
  return !job->text || !g_strcmp0(state->draft_d, job->ids[0]);
}

static void
signer_failed(GhDraftSync *self, Job *job, GError *error)
{
  if (g_error_matches(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED))
    room_get(self, job->room_key)->denied_revision = job->revision; /* until Retry or an edit */
  else if (!g_error_matches(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED) &&
           !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    self->signer_waiting = TRUE; /* saved on this device; the signer is asked again later */
}

/* The job of a signer callback, or NULL (freed) when its binding went away
 * or it was superseded. */
static Job *
job_of_callback(gpointer data)
{
  Job *job = data;
  if (job->detached || job->self->job != job || !bound(job->self)) {
    job_free(job);
    return NULL;
  }
  return job;
}

static void
signed_cb(GObject *source, GAsyncResult *result, gpointer data)
{
  (void) source;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  Job *job = job_of_callback(data);
  if (!job)
    return;
  GhDraftSync *self = job->self;
  if (!signed_json) {
    signer_failed(self, job, error);
    job_end(self);
    return;
  }
  if (!job_current(self, job)) {
    job_end(self); /* superseded meanwhile: the result is discarded */
    return;
  }
  const gchar *d = job->ids[job->index];
  gboolean alias = job->alias_blanks || job->index >= job->n_head;
  /* Its echo on the live REQ is this device's own: never decrypted again. */
  NostrEvent *own = nostr_event_new();
  if (nostr_event_deserialize_signed(own, signed_json, NULL) == NOSTR_EVENT_VALIDATION_OK) {
    char *own_id = nostr_event_get_id(own);
    g_hash_table_add(self->seen, g_strdup(own_id));
    free(own_id);
  }
  nostr_event_free(own);
  if (!gh_store_drafts_save_publication(self->store, job->room_key, d, job->revision, signed_json,
                                        (const gchar *const *) self->targets, &error) ||
      !publish_event(self, job->room_key, d, job->revision, signed_json, alias, &error)) {
    publish_failed_now(self, job->room_key, error);
    job_end(self);
    return;
  }
  job->index++;
  job_next(self);
}

static void
job_sign(GhDraftSync *self, Job *job)
{
  const gchar *d = job->ids[job->index];
  gint64 now = now_unix(self);
  gint64 created_at = MAX(now, job->last_created_at + 1);
  gint64 expiration = created_at + GH_DRAFT_SYNC_EXPIRATION_SECONDS;
  NostrEvent *envelope = nostr_nip37_build_envelope(self->account, created_at, d,
                                                    NOSTR_KIND_DIRECT_MESSAGE,
                                                    job->cipher ? job->cipher : "", &expiration);
  if (!envelope) {
    set_failure(self, "The draft could not be prepared");
    job_end(self);
    return;
  }
  char *raw = nostr_event_serialize_compact(envelope);
  g_autofree gchar *unsigned_json = g_strdup(raw);
  free(raw);
  nostr_event_free(envelope);
  GhSignerContext context = { _("Keep this draft on your private relays, encrypted to you."),
    "draft-sync", d, 1, TRUE };
  gh_account_controller_sign_with_context_async(self->network.accounts, unsigned_json, &context,
                                                self->cancel, signed_cb, job);
}

static void
job_next(GhDraftSync *self)
{
  Job *job = self->job;
  if (!job->ids[job->index]) {
    job_end(self);
    return;
  }
  job_sign(self, job);
}

static void
encrypted_cb(GObject *source, GAsyncResult *result, gpointer data)
{
  (void) source;
  g_autoptr(GError) error = NULL;
  gchar *cipher = gh_account_controller_nip44_finish(result, &error);
  Job *job = job_of_callback(data);
  if (!job) {
    g_free(cipher);
    return;
  }
  GhDraftSync *self = job->self;
  if (!cipher) {
    signer_failed(self, job, error);
    job_end(self);
    return;
  }
  job->cipher = cipher;
  if (!job_current(self, job)) {
    job_end(self);
    return;
  }
  job_sign(self, job);
}

/* The unsigned kind-14 event a text revision wraps: the account's, with the
 * room's other participants (or the account itself) as p tags. */
static gchar *
inner_draft_json(GhDraftSync *self, const gchar *room_key, const gchar *text)
{
  NostrEvent *draft = nostr_event_new();
  nostr_event_set_kind(draft, NOSTR_KIND_DIRECT_MESSAGE);
  nostr_event_set_pubkey(draft, self->account);
  nostr_event_set_created_at(draft, now_unix(self));
  nostr_event_set_content(draft, text);
  NostrTags *tags = nostr_tags_new(0);
  g_auto(GStrv) members = g_strsplit(room_key, ",", -1);
  guint n_members = g_strv_length(members);
  for (guint i = 0; members[i]; i++)
    if (n_members == 1 || g_strcmp0(members[i], self->account))
      nostr_tags_append(tags, nostr_tag_new("p", members[i], NULL));
  nostr_event_set_tags(draft, tags);
  char *raw = nostr_nip37_serialize_draft(draft);
  nostr_event_free(draft);
  gchar *json = g_strdup(raw);
  free(raw);
  return json;
}

static Job *
job_new(GhDraftSync *self, GhStoreDraftState *state)
{
  Job *job = g_new0(Job, 1);
  job->self = g_object_ref(self);
  job->room_key = g_strdup(state->room_key);
  job->revision = state->local_revision;
  job->last_created_at = state->last_created_at;
  return job;
}

/* Starts the pipeline for a dirty room: its text under its head identifier,
 * or blanks under every identifier after a clear. FALSE when nothing starts. */
static gboolean
job_start_room(GhDraftSync *self, const gchar *room_key)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(self->store, room_key, &error);
  g_autofree gchar *text = NULL;
  if (!state || !state->dirty ||
      !gh_store_drafts_get_room(self->store, room_key, NULL, NULL, &text, &error)) {
    if (error)
      g_warning("Groundhog could not read a draft for sync: %s", error->message);
    return FALSE;
  }
  Job *job = job_new(self, state);
  g_autoptr(GStrvBuilder) ids = g_strv_builder_new();
  if (text) {
    g_autofree gchar *d = NULL;
    if (!gh_store_drafts_assign_identifier(self->store, room_key, &d, &error)) {
      g_warning("Groundhog could not assign a draft identifier: %s", error->message);
      job_free(job);
      return FALSE;
    }
    g_strv_builder_add(ids, d);
    job->text = g_steal_pointer(&text);
    job->n_head = 1;
  } else {
    if (state->draft_d) {
      g_strv_builder_add(ids, state->draft_d);
      job->n_head = 1;
    }
    for (guint i = 0; state->aliases[i]; i++)
      g_strv_builder_add(ids, state->aliases[i]);
  }
  job->ids = g_strv_builder_end(ids);
  if (!job->ids[0]) {
    /* A clear of a room nothing was ever published for: clean at once. */
    job_free(job);
    if (!gh_store_drafts_mark_clean(self->store, room_key, &error))
      g_warning("Groundhog could not settle a draft clear: %s", error->message);
    return FALSE;
  }
  self->job = job;
  room_get(self, room_key)->flush = FALSE;
  if (job->text) {
    g_autofree gchar *plaintext = inner_draft_json(self, room_key, job->text);
    GhSignerContext context = { _("Keep this draft on your private relays, encrypted to you."),
      "draft-sync", job->ids[0], 1, TRUE };
    gh_account_controller_nip44_encrypt_with_context_async(self->network.accounts, plaintext,
      self->account, &context, self->cancel, encrypted_cb, job);
  } else
    job_sign(self, job);
  return TRUE;
}

/* Blanks owed for a clean room's aliases (the QUEUED rows an accepted head
 * left), so a restart still retires them. */
static gboolean
job_start_aliases(GhDraftSync *self, GhStoreDraftState *state, const gchar *const *aliases)
{
  Job *job = job_new(self, state);
  job->ids = g_strdupv((gchar **) aliases);
  job->alias_blanks = TRUE;
  self->job = job;
  job_sign(self, job);
  return TRUE;
}

/* ---- the scheduler --------------------------------------------------------------------- */

static gboolean
publishing(GhDraftSync *self, const gchar *room_key)
{
  for (guint i = 0; i < self->publications->len; i++)
    if (!g_strcmp0(((Publication *) self->publications->pdata[i])->room_key, room_key))
      return TRUE;
  return FALSE;
}

static gboolean
timer_fired(gpointer data)
{
  GhDraftSync *self = data;
  self->timer = 0;
  schedule(self);
  return G_SOURCE_REMOVE;
}

/* Resends a stored signed event whose revision is still current. */
static gboolean
resume_publication(GhDraftSync *self, GhStoreDraftPublication *row)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(self->store, row->room_key, &error);
  gboolean alias = state && g_strv_contains((const gchar *const *) state->aliases, row->d);
  if (!state || row->local_revision != state->local_revision ||
      (!alias && (!state->dirty || g_strcmp0(state->draft_d, row->d)))) {
    /* Stale: the room moved on (or settled); its current state is re-signed. */
    if (!gh_store_drafts_delete_publication(self->store, row->room_key, row->d, &error))
      g_warning("Groundhog could not drop a stale draft publication: %s", error->message);
    return FALSE;
  }
  gboolean any_target = FALSE;
  for (guint i = 0; row->targets[i]; i++)
    any_target = any_target || g_strv_contains((const gchar *const *) self->targets, row->targets[i]);
  if (!any_target) {
    if (!gh_store_drafts_delete_publication(self->store, row->room_key, row->d, &error))
      g_warning("Groundhog could not drop a draft publication: %s", error->message);
    return FALSE;
  }
  if (!publish_event(self, row->room_key, row->d, row->local_revision, row->event_json, alias, &error)) {
    publish_failed_now(self, row->room_key, error);
    return FALSE;
  }
  return TRUE;
}

static void
schedule(GhDraftSync *self)
{
  if (self->timer) {
    gh_clock_source_remove(self->clock, self->timer);
    self->timer = 0;
  }
  if (!active(self) || self->job)
    return;
  g_autoptr(GError) error = NULL;
  gint64 now = now_mono(self);
  gint64 next_due = G_MAXINT64;
  /* Stored signed events first: they need no signer. */
  g_autoptr(GPtrArray) stored = gh_store_drafts_list_publications(self->store, &error);
  for (guint i = 0; stored && i < stored->len; i++) {
    GhStoreDraftPublication *row = stored->pdata[i];
    Room *room = room_get(self, row->room_key);
    if (row->state != GH_STORE_DRAFT_PUBLICATION_SIGNED || publishing(self, row->room_key))
      continue;
    if (room->retry_at_us > now) {
      next_due = MIN(next_due, room->retry_at_us);
      continue;
    }
    resume_publication(self, row);
  }
  gboolean can_sign = !self->signer_waiting && self->network.accounts &&
                      gh_account_controller_can_call_signer(self->network.accounts);
  g_autoptr(GPtrArray) states = gh_store_drafts_list(self->store, &error);
  for (guint i = 0; states && i < states->len; i++) {
    GhStoreDraftState *state = states->pdata[i];
    Room *room = room_get(self, state->room_key);
    if (room->reconcile || publishing(self, state->room_key) || !can_sign)
      continue;
    if (!state->dirty) {
      /* Blanks still owed to a clean room's aliases after an accepted head. */
      g_autoptr(GStrvBuilder) owed = g_strv_builder_new();
      for (guint j = 0; stored && j < stored->len; j++) {
        GhStoreDraftPublication *row = stored->pdata[j];
        if (row->state == GH_STORE_DRAFT_PUBLICATION_QUEUED &&
            !g_strcmp0(row->room_key, state->room_key) &&
            g_strv_contains((const gchar *const *) state->aliases, row->d))
          g_strv_builder_add(owed, row->d);
      }
      g_auto(GStrv) aliases = g_strv_builder_end(owed);
      if (aliases[0] && room->denied_revision != state->local_revision && room->retry_at_us <= now &&
          job_start_aliases(self, state, (const gchar *const *) aliases))
        return;
      if (aliases[0] && room->retry_at_us > now)
        next_due = MIN(next_due, room->retry_at_us);
      continue;
    }
    if (room->denied_revision == state->local_revision)
      continue;
    g_autofree gchar *text = NULL;
    if (!gh_store_drafts_get_room(self->store, state->room_key, NULL, NULL, &text, NULL))
      continue;
    /* A text revision of a room without an identifier waits for the import
     * to complete: no new identifier before every relay answered. */
    if (text && !state->draft_d && !import_settled(self))
      continue;
    gint64 due;
    if (room->last_edit_us == 0 || room->flush || !text)
      due = now; /* pending from before this binding, a left room, or a clear */
    else
      due = room->last_edit_us + (gint64) (remote_signer(self) ? GH_DRAFT_SYNC_REMOTE_DELAY_SECONDS
                                                                : GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS) *
                                  G_USEC_PER_SEC;
    if (room->last_publish_us)
      due = MAX(due, room->last_publish_us + (gint64) GH_DRAFT_SYNC_RATE_SECONDS * G_USEC_PER_SEC);
    due = MAX(due, room->retry_at_us);
    if (due <= now) {
      if (job_start_room(self, state->room_key))
        return;
      continue;
    }
    next_due = MIN(next_due, due);
  }
  if (next_due != G_MAXINT64)
    self->timer = gh_clock_timeout_add(self->clock, MAX((guint64) ((next_due - now + 999) / 1000), 1),
                                       timer_fired, self, NULL);
}

/* ---- status ---------------------------------------------------------------------------- */

static void
refresh_status(GhDraftSync *self)
{
  g_autofree gchar *status = NULL;
  if (sync_enabled(self)) {
    gboolean pending = FALSE;
    g_autoptr(GPtrArray) states = gh_store_drafts_list(self->store, NULL);
    for (guint i = 0; states && i < states->len; i++)
      pending = pending || ((GhStoreDraftState *) states->pdata[i])->dirty;
    if (!self->targets || !self->targets[0])
      status = g_strdup(_("Choose a Private Relay to Sync Drafts"));
    else if (pending && self->signer_waiting)
      status = g_strdup(_("Saved on This Device · Waiting for Signer"));
    else if (pending && self->job == NULL && self->publications->len == 0 && self->failure)
      status = g_strdup_printf(_("Saved on This Device · %s"), self->failure);
    else if (pending && self->job == NULL && self->publications->len == 0) {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init(&iter, self->rooms);
      while (g_hash_table_iter_next(&iter, &key, &value))
        if (((Room *) value)->denied_revision)
          status = g_strdup(_("Saved on This Device · Signer Declined"));
    }
    if (!status && self->incomplete)
      status = g_strdup_printf(ngettext("%u draft could not be synced",
                                        "%u drafts could not be synced", self->incomplete),
                               self->incomplete);
  }
  if (g_strcmp0(self->status, status) == 0)
    return;
  g_free(self->status);
  self->status = g_steal_pointer(&status);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

/* ---- wiring ---------------------------------------------------------------------------- */

/* The sync switch moved (whichever of GhSocialLists' signals arrives first
 * says so): off cancels the pipeline, drops what waited for decryption and
 * deletes unsent events, keeping tombstones and dirty flags; on makes every
 * unsettled local draft one of unknown age until the import is complete
 * (§3.7 "Enabling sync"). */
static void
mark_unknown_age(GhDraftSync *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) locals = gh_store_drafts_list_local(self->store, &error);
  g_autoptr(GPtrArray) pending = gh_store_drafts_list_publications(self->store, NULL);
  for (guint i = 0; locals && i < locals->len; i++) {
    const gchar *room_key = locals->pdata[i];
    g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(self->store, room_key, NULL);
    /* The current revision already signed while sync was on only awaits a
     * relay: its age is known. An older signed row proves nothing. */
    gboolean signed_pending = FALSE;
    for (guint j = 0; state && pending && j < pending->len; j++) {
      GhStoreDraftPublication *row = pending->pdata[j];
      signed_pending = signed_pending || (row->state == GH_STORE_DRAFT_PUBLICATION_SIGNED &&
                                          row->local_revision == state->local_revision &&
                                          !g_strcmp0(row->room_key, room_key));
    }
    if (!signed_pending && (!state || state->dirty || !state->last_event_id))
      room_get(self, room_key)->reconcile = TRUE;
  }
}

static void
follow_switch(GhDraftSync *self)
{
  gboolean enabled = sync_enabled(self);
  if (enabled == self->enabled)
    return;
  self->enabled = enabled;
  if (!enabled) {
    g_autoptr(GError) error = NULL;
    g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel);
    self->cancel = g_cancellable_new();
    self->epoch++;
    detach_job(self);
    drop_incoming(self);
    self->decrypting = FALSE;
    if (!gh_store_drafts_delete_unsent(self->store, &error))
      g_warning("Groundhog could not discard unsent draft revisions: %s", error->message);
  } else
    mark_unknown_age(self);
}

static void
refresh_targets(GhDraftSync *self)
{
  if (!bound(self))
    return;
  follow_switch(self);
  g_auto(GStrv) targets = current_targets(self);
  gboolean same = self->targets && g_strv_length(targets) == g_strv_length(self->targets);
  for (guint i = 0; same && targets[i]; i++)
    same = !g_strcmp0(targets[i], self->targets[i]);
  if (same) {
    refresh_status(self);
    return;
  }
  g_strfreev(self->targets);
  self->targets = g_steal_pointer(&targets);
  /* Publications in flight went to relays that may no longer be current. */
  cancel_publications(self);
  open_scope(self);
  schedule(self);
  refresh_status(self);
}

static void
on_sync_state_changed(GhSocialLists *social, GhDraftSync *self)
{
  (void) social;
  refresh_targets(self);
}

static void
on_social_changed(GhSocialLists *social, GhDraftSync *self)
{
  (void) social;
  refresh_targets(self);
}

static void
on_accounts_changed(GhAccountController *accounts, GhDraftSync *self)
{
  (void) accounts;
  if (!bound(self)) {
    gh_draft_sync_close(self);
    return;
  }
  self->signer_waiting = FALSE;
  schedule(self);
  refresh_status(self);
}

static void
on_draft_changed(GhStoreConversations *conversations, const gchar *room_key, GhDraftSync *self)
{
  (void) conversations;
  if (!bound(self))
    return;
  Room *room = room_get(self, room_key);
  room->last_edit_us = now_mono(self);
  room->denied_revision = 0;
  room->reconcile = FALSE; /* an edit now is this device's current intent */
  room->flush = FALSE;
  schedule(self);
  refresh_status(self);
}

static void
on_enqueued(GObject *outbox, GhDraftSync *self)
{
  (void) outbox;
  gh_draft_sync_wake(self);
}

void
gh_draft_sync_wake(GhDraftSync *self)
{
  g_return_if_fail(GH_IS_DRAFT_SYNC(self));
  if (!bound(self))
    return;
  schedule(self);
  refresh_status(self);
}

void
gh_draft_sync_retry(GhDraftSync *self)
{
  g_return_if_fail(GH_IS_DRAFT_SYNC(self));
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->rooms);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Room *room = value;
    room->denied_revision = 0;
    room->retry_at_us = 0;
  }
  self->signer_waiting = FALSE;
  set_failure(self, NULL);
  if (!bound(self))
    return;
  /* A relay that refused the subscription is asked again too. */
  if (active(self) && !self->eose_all)
    open_scope(self);
  gh_draft_sync_wake(self);
}

void
gh_draft_sync_set_shown_room(GhDraftSync *self, const gchar *room_key)
{
  g_return_if_fail(GH_IS_DRAFT_SYNC(self));
  if (!g_strcmp0(self->shown_room, room_key))
    return;
  if (self->shown_room && bound(self)) {
    /* Leaving a room schedules its latest saved revision (inside the rate limit). */
    Room *room = room_get(self, self->shown_room);
    room->flush = TRUE;
  }
  g_free(self->shown_room);
  self->shown_room = g_strdup(room_key);
  if (bound(self))
    schedule(self);
}

void
gh_draft_sync_set_applier(GhDraftSync *self, const GhDraftSyncApplier *applier, gpointer data)
{
  g_return_if_fail(GH_IS_DRAFT_SYNC(self));
  self->applier = applier ? *applier : (GhDraftSyncApplier) { 0 };
  self->applier_data = applier ? data : NULL;
}

gboolean
gh_draft_sync_bind_store(GhDraftSync *self, GhStore *store, GhStoreConversations *conversations,
                         guint64 generation, GError **error)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), FALSE);
  g_return_val_if_fail(store != NULL, FALSE);
  gh_draft_sync_close(self);
  if (!self->network.accounts) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Draft sync has no network");
    return FALSE;
  }
  self->store = store;
  self->generation = generation;
  self->account = g_strdup(gh_store_get_account_pubkey(store));
  self->clock = gh_clock_ref(gh_store_get_clock(store));
  self->cancel = g_cancellable_new();
  if (conversations) {
    self->conversations = g_object_ref(conversations);
    g_signal_connect(conversations, "draft-changed", G_CALLBACK(on_draft_changed), self);
  }
  g_signal_connect(self->social, "sync-state-changed", G_CALLBACK(on_sync_state_changed), self);
  g_signal_connect(self->social, "changed", G_CALLBACK(on_social_changed), self);
  g_signal_connect(self->network.accounts, "changed", G_CALLBACK(on_accounts_changed), self);
  if (!gh_store_drafts_prune(store, now_unix(self), error)) {
    gh_draft_sync_close(self);
    return FALSE;
  }
  /* Revisions this device published or already holds are not re-imported. */
  g_autoptr(GPtrArray) known = gh_store_drafts_list(store, NULL);
  for (guint i = 0; known && i < known->len; i++) {
    GhStoreDraftState *state = known->pdata[i];
    if (state->last_event_id)
      g_hash_table_add(self->seen, g_strdup(state->last_event_id));
  }
  g_autoptr(GPtrArray) pending = gh_store_drafts_list_publications(store, NULL);
  for (guint i = 0; pending && i < pending->len; i++) {
    GhStoreDraftPublication *row = pending->pdata[i];
    NostrEvent *event = nostr_event_new();
    if (*row->event_json &&
        nostr_event_deserialize_signed(event, row->event_json, NULL) == NOSTR_EVENT_VALIDATION_OK) {
      char *id = nostr_event_get_id(event);
      g_hash_table_add(self->seen, g_strdup(id));
      free(id);
    }
    nostr_event_free(event);
  }
  /* Bound with sync on (a restart): unsettled drafts are of unknown age
   * against what the relays hold, exactly as at enable time. */
  self->enabled = FALSE;
  refresh_targets(self);
  return TRUE;
}

void
gh_draft_sync_watch_outbox(GhDraftSync *self, GObject *outbox)
{
  g_return_if_fail(GH_IS_DRAFT_SYNC(self));
  if (self->outbox == outbox)
    return;
  if (self->outbox) {
    g_signal_handlers_disconnect_by_data(self->outbox, self);
    g_clear_object(&self->outbox);
  }
  if (outbox) {
    self->outbox = g_object_ref(outbox);
    g_signal_connect(outbox, "enqueued", G_CALLBACK(on_enqueued), self);
  }
}

void
gh_draft_sync_close(GhDraftSync *self)
{
  g_return_if_fail(GH_IS_DRAFT_SYNC(self));
  if (self->cancel) {
    g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel);
  }
  self->epoch++;
  close_scope(self);
  cancel_publications(self);
  detach_job(self);
  if (self->timer && self->clock) {
    gh_clock_source_remove(self->clock, self->timer);
    self->timer = 0;
  }
  if (self->conversations) {
    g_signal_handlers_disconnect_by_data(self->conversations, self);
    g_clear_object(&self->conversations);
  }
  gh_draft_sync_watch_outbox(self, NULL);
  if (self->store) {
    g_signal_handlers_disconnect_by_data(self->social, self);
    if (self->network.accounts)
      g_signal_handlers_disconnect_by_data(self->network.accounts, self);
  }
  self->store = NULL;
  g_clear_pointer(&self->account, g_free);
  g_clear_pointer(&self->clock, gh_clock_unref);
  g_clear_pointer(&self->targets, g_strfreev);
  drop_incoming(self);
  g_hash_table_remove_all(self->rooms);
  g_hash_table_remove_all(self->seen);
  g_hash_table_remove_all(self->orphan_blanks);
  self->eose_all = FALSE;
  self->decrypting = FALSE;
  self->decrypted = self->incomplete = 0;
  self->signer_waiting = FALSE;
  self->enabled = FALSE;
  set_failure(self, NULL);
  g_clear_pointer(&self->shown_room, g_free);
  if (self->status) {
    g_clear_pointer(&self->status, g_free);
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  }
}

static void
store_closing(GhAccountStore *account_store, GhDraftSync *self)
{
  (void) account_store;
  gh_draft_sync_close(self);
}

static void
account_store_changed(GhAccountStore *account_store, GhDraftSync *self)
{
  GhStore *store = gh_account_store_get_store(account_store);
  GhStoreConversations *conversations = gh_account_store_get_conversations(account_store);
  if (!store || !conversations) {
    if (self->store)
      gh_draft_sync_close(self);
    return;
  }
  if (self->store != store) {
    g_autoptr(GError) error = NULL;
    if (!gh_draft_sync_bind_store(self, store, conversations,
                                  gh_account_store_get_generation(account_store), &error))
      g_warning("Groundhog could not start draft sync: %s", error->message);
  }
  if (self->store == store)
    gh_draft_sync_watch_outbox(self, gh_account_store_get_outbox(account_store));
}

void
gh_draft_sync_attach_store(GhDraftSync *self, GhAccountStore *account_store)
{
  g_return_if_fail(GH_IS_DRAFT_SYNC(self));
  g_return_if_fail(self->account_store == NULL);
  g_return_if_fail(GH_IS_ACCOUNT_STORE(account_store));
  self->account_store = g_object_ref(account_store);
  g_signal_connect(account_store, "changed", G_CALLBACK(account_store_changed), self);
  g_signal_connect(account_store, "store-closing", G_CALLBACK(store_closing), self);
  account_store_changed(account_store, self);
}

void
gh_draft_sync_set_network(GhDraftSync *self, const GhListAdapterNetwork *network)
{
  g_return_if_fail(GH_IS_DRAFT_SYNC(self));
  g_return_if_fail(self->store == NULL);
  gh_list_adapter_network_clear(&self->network);
  gh_list_adapter_network_copy(&self->network, network);
}

const gchar *
gh_draft_sync_get_status(GhDraftSync *self)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), NULL);
  return self->status;
}

void
gh_draft_sync_get_import_state(GhDraftSync *self, gboolean *out_complete, guint *out_decrypted,
                               guint *out_incomplete)
{
  g_return_if_fail(GH_IS_DRAFT_SYNC(self));
  if (out_complete)
    *out_complete = import_settled(self);
  if (out_decrypted)
    *out_decrypted = self->decrypted;
  if (out_incomplete)
    *out_incomplete = self->incomplete;
}

gboolean
gh_draft_sync_is_active(GhDraftSync *self)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), FALSE);
  return active(self);
}

gboolean
gh_draft_sync_is_enabled(GhDraftSync *self)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), FALSE);
  return sync_enabled(self);
}

guint64
gh_draft_sync_get_generation(GhDraftSync *self)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), 0);
  return self->store ? self->generation : 0;
}

const gchar *
gh_draft_sync_get_account(GhDraftSync *self)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), NULL);
  return self->account;
}

gboolean
gh_draft_sync_can_retry(GhDraftSync *self)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), FALSE);
  if (!active(self))
    return FALSE;
  if (self->signer_waiting || self->failure)
    return TRUE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->rooms);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (((Room *) value)->denied_revision)
      return TRUE;
  return FALSE;
}

/* ---- conflict copies ------------------------------------------------------------------- */

static gboolean
check_bound(GhDraftSync *self, GError **error)
{
  if (bound(self))
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account storage is not open");
  return FALSE;
}

GPtrArray *
gh_draft_sync_list_conflicts(GhDraftSync *self, GError **error)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), NULL);
  if (!bound(self))
    return g_ptr_array_new_with_free_func((GDestroyNotify) gh_store_draft_conflict_free);
  return gh_store_drafts_list_conflicts(self->store, NULL, error);
}

gboolean
gh_draft_sync_discard_conflict(GhDraftSync *self, gint64 id, GError **error)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), FALSE);
  if (!check_bound(self, error) || !gh_store_drafts_delete_conflict(self->store, id, error))
    return FALSE;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
gh_draft_sync_keep_conflict(GhDraftSync *self, const gchar *room_key, const gchar *text,
                            GError **error)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), FALSE);
  g_return_val_if_fail(room_key != NULL && text != NULL, FALSE);
  if (!check_bound(self, error) ||
      !gh_store_drafts_add_conflict(self->store, room_key, text, NULL, now_unix(self), error))
    return FALSE;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
gh_draft_sync_restore_conflict(GhDraftSync *self, gint64 id, GError **error)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), FALSE);
  if (!check_bound(self, error))
    return FALSE;
  if (!self->conversations) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Drafts cannot be restored without the conversation store");
    return FALSE;
  }
  g_autoptr(GPtrArray) conflicts = gh_store_drafts_list_conflicts(self->store, NULL, error);
  if (!conflicts)
    return FALSE;
  GhStoreDraftConflict *found = NULL;
  for (guint i = 0; i < conflicts->len && !found; i++)
    if (((GhStoreDraftConflict *) conflicts->pdata[i])->id == id)
      found = conflicts->pdata[i];
  if (!found) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such draft copy");
    return FALSE;
  }
  /* The text it replaces is kept in turn, so nothing written is lost. */
  g_autofree gchar *local = NULL;
  if (!gh_store_drafts_get_room(self->store, found->room_key, NULL, NULL, &local, error))
    return FALSE;
  if (local && *local && g_strcmp0(local, found->text) &&
      !gh_store_drafts_add_conflict(self->store, found->room_key, local, NULL, now_unix(self), error))
    return FALSE;
  /* A local edit: a new dirty revision that syncs as the newest. */
  if (!gh_store_conversations_set_draft(self->conversations, found->room_key, found->text, error) ||
      !gh_store_drafts_delete_conflict(self->store, id, error))
    return FALSE;
  apply_to_window(self, found->room_key, found->text);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

GhDraftSync *
gh_draft_sync_new(GhSocialLists *social)
{
  g_return_val_if_fail(GH_IS_SOCIAL_LISTS(social), NULL);
  GhDraftSync *self = g_object_new(GH_TYPE_DRAFT_SYNC, NULL);
  self->social = g_object_ref(social);
  return self;
}

static void
gh_draft_sync_dispose(GObject *object)
{
  GhDraftSync *self = GH_DRAFT_SYNC(object);
  gh_draft_sync_close(self);
  if (self->account_store) {
    g_signal_handlers_disconnect_by_data(self->account_store, self);
    g_clear_object(&self->account_store);
  }
  g_clear_object(&self->social);
  gh_list_adapter_network_clear(&self->network);
  G_OBJECT_CLASS(gh_draft_sync_parent_class)->dispose(object);
}

static void
gh_draft_sync_finalize(GObject *object)
{
  GhDraftSync *self = GH_DRAFT_SYNC(object);
  g_ptr_array_unref(self->publications);
  g_ptr_array_unref(self->incoming);
  g_hash_table_unref(self->rooms);
  g_hash_table_unref(self->eose_pending);
  g_hash_table_unref(self->seen);
  g_hash_table_unref(self->orphan_blanks);
  g_free(self->status);
  g_free(self->failure);
  G_OBJECT_CLASS(gh_draft_sync_parent_class)->finalize(object);
}

static void
gh_draft_sync_class_init(GhDraftSyncClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_draft_sync_dispose;
  object_class->finalize = gh_draft_sync_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_draft_sync_init(GhDraftSync *self)
{
  self->publications = g_ptr_array_new();
  self->incoming = g_ptr_array_new_with_free_func(incoming_free);
  self->rooms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->eose_pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->orphan_blanks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, orphan_blank_free);
}
