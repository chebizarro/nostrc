#include "gh-mls-commits.h"

#include "gh-store-marmot.h"

#include <stdlib.h>
#include <string.h>

#include <nostr-event.h>

G_DEFINE_QUARK(gh-mls-commit-error-quark, gh_mls_commit_error)

/* GhRelayPublishOutcome values (src/relay/gh-relay-publish.h); the store
 * keeps outcomes as integers and this store-layer module does not link the
 * relay code. */
enum {
  OUTCOME_PENDING = 0,
  OUTCOME_ACCEPTED = 1,
  OUTCOME_REJECTED = 2
};

void
gh_mls_commit_publish_free(GhMlsCommitPublish *publish)
{
  if (!publish)
    return;
  g_free(publish->group_id_hex);
  g_free(publish->event_id);
  g_free(publish->event_json);
  g_strfreev(publish->relay_urls);
  g_free(publish);
}

/* ---- Errors ------------------------------------------------------------------------ */

/* A libmarmot failure: the storage error behind it when there is one (so
 * GH_STORE_ERROR_FULL stays FULL), else the MarmotError. */
static gboolean
marmot_fail(MarmotStorage *storage, MarmotError err, const gchar *what, GError **error)
{
  GError *store_error = gh_store_marmot_take_error(storage);
  if (store_error) {
    g_propagate_prefixed_error(error, store_error, "%s: ", what);
    return FALSE;
  }
  g_set_error(error, GH_MLS_COMMIT_ERROR, err, "%s: %s", what, marmot_error_string(err));
  return FALSE;
}

/* Starts clean: a failure recorded by an earlier call must not be reported
 * for this one. */
static void
drop_stale_error(MarmotStorage *storage)
{
  GError *stale = gh_store_marmot_take_error(storage);
  g_clear_error(&stale);
}

/* ---- Helpers --------------------------------------------------------------------------- */

/* The canonical id of our own signed kind:445 (verified: we never seal an
 * event whose id or signature is wrong). */
static gchar *
event_id_of(const gchar *event_json, gint64 *out_created_at, GError **error)
{
  NostrEvent *event = nostr_event_new();
  char id[65] = { 0 };
  gboolean ok = event && nostr_event_deserialize_compact(event, event_json, NULL) &&
                nostr_event_get_kind(event) == MARMOT_KIND_GROUP_MESSAGE &&
                nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK;
  if (ok && out_created_at)
    *out_created_at = nostr_event_get_created_at(event);
  nostr_event_free(event);
  if (!ok) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The Commit is not a validly signed kind:445 event");
    return NULL;
  }
  return g_strdup(id);
}

/* The outbox op_id of the Commit @event_id: stable, so a repeat is
 * idempotent (T-enqueue), and recognizable as this module's. */
static gchar *
op_id_for(const gchar *event_id)
{
  g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
  static const guchar domain[] = "groundhog/mls-commit/v1";
  g_checksum_update(sum, domain, sizeof domain);   /* with its NUL: a separator */
  g_checksum_update(sum, (const guchar *) event_id, (gssize) strlen(event_id));
  return g_strndup(g_checksum_get_string(sum), 32);
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *) a, *(const gchar *const *) b);
}

/* The group's relay URLs, sorted (NULL-terminated; empty when none). */
static GStrv
group_relays(Marmot *marmot, MarmotStorage *storage, const MarmotGroupId *gid, GError **error)
{
  MarmotGroupRelay *relays = NULL;
  size_t n = 0;
  MarmotError err = marmot_get_group_relay_urls(marmot, gid, &relays, &n);
  if (err != MARMOT_OK) {
    marmot_fail(storage, err, "Reading the group relays", error);
    return NULL;
  }
  GPtrArray *urls = g_ptr_array_new();
  for (size_t i = 0; i < n; i++) {
    if (relays[i].relay_url)
      g_ptr_array_add(urls, g_strdup(relays[i].relay_url));
    free(relays[i].relay_url);
    marmot_group_id_free(&relays[i].mls_group_id);
  }
  free(relays);
  g_ptr_array_sort(urls, compare_strings);
  g_ptr_array_add(urls, NULL);
  return (GStrv) g_ptr_array_free(urls, FALSE);
}

/* The entry's Commit event (its only event, role MLS_MESSAGE), or NULL. */
static GhStoreOutboxEvent *
commit_event_of(GhStoreOutboxEntry *entry)
{
  if (!entry->events || entry->events->len != 1)
    return NULL;
  GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, 0);
  return event->role == GH_STORE_OUTBOX_ROLE_MLS_MESSAGE ? event : NULL;
}

static gboolean
target_known(const GhStoreOutboxEvent *event, const gchar *url)
{
  for (guint i = 0; i < event->targets->len; i++) {
    GhStoreOutboxTarget *t = g_ptr_array_index(event->targets, i);
    if (g_strcmp0(t->relay_url, url) == 0)
      return TRUE;
  }
  return FALSE;
}

/* A publish record of @entry: its stored targets plus @extra_urls (the
 * group's current relays, which join as targets of the same event), minus
 * those that answered: an OK true needs nothing more, an OK false would be
 * repeated. */
static GhMlsCommitPublish *
publish_of(const gchar *gid_hex, GhStoreOutboxEntry *entry, GhStoreOutboxEvent *event,
           const gchar *const *extra_urls)
{
  GhMlsCommitPublish *publish = g_new0(GhMlsCommitPublish, 1);
  publish->group_id_hex = g_strdup(gid_hex);
  publish->outbox_id = entry->id;
  publish->outbox_event_id = event->id;
  publish->event_id = g_strdup(event->event_id);
  publish->event_json = g_strdup(event->event_json);
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < event->targets->len; i++) {
    GhStoreOutboxTarget *t = g_ptr_array_index(event->targets, i);
    if (t->outcome != OUTCOME_ACCEPTED && t->outcome != OUTCOME_REJECTED)
      g_ptr_array_add(urls, g_strdup(t->relay_url));
  }
  for (guint i = 0; extra_urls && extra_urls[i]; i++)
    if (!target_known(event, extra_urls[i]))
      g_ptr_array_add(urls, g_strdup(extra_urls[i]));
  g_ptr_array_sort(urls, compare_strings);
  g_ptr_array_add(urls, NULL);
  publish->relay_urls = (GStrv) g_ptr_array_steal(urls, NULL);
  return publish;
}

static gboolean
outbox_set(GhStore *store, gint64 outbox_id, GhStoreOutboxState state, const gchar *reason,
           GError **error)
{
  GhStoreOutboxUpdate update = { .state = state, .last_error = reason };
  return gh_store_outbox_update(store, outbox_id, &update, error);
}

/* Enqueues (idempotently) and, when not sealed yet, seals @event_json for
 * @relays in @gid_hex's conversation. The entry is returned loaded. */
static GhStoreOutboxEntry *
ensure_entry(GhStore *store, const gchar *gid_hex, const gchar *account_pubkey,
             const gchar *event_id, const gchar *event_json, gint64 created_at,
             const gchar *const *relays, GError **error)
{
  gint64 conversation = 0, outbox_id = 0, message_id = 0;
  if (!gh_store_ensure_conversation(store, GH_STORE_BACKEND_MLS, gid_hex,
                                    GH_STORE_REQUEST_ACCEPTED, &conversation, error))
    return NULL;
  g_autofree gchar *op_id = op_id_for(event_id);
  GhStoreOutgoing outgoing = {
    .conversation_id = conversation,
    .op_id = op_id,
    .backend_msg_id = event_id,
    .sender_pubkey = account_pubkey,
    .kind = MARMOT_KIND_GROUP_MESSAGE,
    .created_at = created_at,
    .rumor_json = event_json,
  };
  if (!gh_store_enqueue(store, &outgoing, &outbox_id, &message_id, error))
    return NULL;
  g_autoptr(GhStoreOutboxEntry) entry = gh_store_outbox_load(store, outbox_id, error);
  if (!entry)
    return NULL;
  if (entry->events->len == 0) {
    GhStoreSealedEvent sealed = {
      .role = GH_STORE_OUTBOX_ROLE_MLS_MESSAGE,
      .event_id = event_id,
      .event_json = event_json,
      .relay_urls = relays,
    };
    if (!gh_store_seal(store, outbox_id, &sealed, 1, error))
      return NULL;
    g_clear_pointer(&entry, gh_store_outbox_entry_free);
    entry = gh_store_outbox_load(store, outbox_id, error);
    if (!entry)
      return NULL;
  }
  GhStoreOutboxEvent *event = commit_event_of(entry);
  /* Only ever the same signed bytes (never re-signed, never rebuilt). */
  if (!event || g_strcmp0(event->event_id, event_id) != 0 ||
      g_strcmp0(event->event_json, event_json) != 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
                        "The sealed Commit differs from the pending one");
    return NULL;
  }
  return g_steal_pointer(&entry);
}

/* ---- Stage --------------------------------------------------------------------------- */

GhMlsCommitPublish *
gh_mls_commit_stage(GhStore *store, Marmot *marmot, MarmotStorage *storage,
                    const MarmotGroupId *group_id, const gchar *account_pubkey,
                    GhMlsCommitProducer producer, gpointer user_data, GError **error)
{
  g_return_val_if_fail(store && marmot && storage && group_id && account_pubkey && producer,
                       NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  drop_stale_error(storage);

  g_autofree gchar *gid_hex = marmot_group_id_to_hex(group_id);
  if (!gid_hex) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "No MLS group id");
    return NULL;
  }
  if (!gh_store_begin(store, error))
    return NULL;

  char *commit_json = NULL;
  g_autofree gchar *event_id = NULL;
  g_auto(GStrv) relays = NULL;
  g_autoptr(GhStoreOutboxEntry) entry = NULL;
  GhMlsCommitPublish *publish = NULL;
  gint64 created_at = 0;
  MarmotError err = producer(marmot, group_id, user_data, &commit_json);
  if (err != MARMOT_OK) {
    marmot_fail(storage, err, "Making the Commit", error);
    goto fail;
  }
  if (!commit_json) {
    g_set_error_literal(error, GH_MLS_COMMIT_ERROR, MARMOT_ERR_INTERNAL,
                        "The producer returned no Commit");
    goto fail;
  }
  if (!(event_id = event_id_of(commit_json, &created_at, error)) ||
      !(relays = group_relays(marmot, storage, group_id, error)))
    goto fail;
  if (!relays[0]) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The group has no relays to publish its Commit to");
    goto fail;
  }
  entry = ensure_entry(store, gid_hex, account_pubkey, event_id, commit_json, created_at,
                       (const gchar *const *) relays, error);
  if (!entry)
    goto fail;
  if (gh_store_commit(store, error))   /* a failed commit rolled back */
    publish = publish_of(gid_hex, entry, commit_event_of(entry), NULL);
  free(commit_json);
  return publish;

fail:
  gh_store_rollback(store);
  free(commit_json);
  return NULL;
}

/* ---- Relay answers ------------------------------------------------------------------ */

/* Every target answered: accepted or refused (a pending or failed
 * connection is not an answer). */
static void
count_targets(const GhStoreOutboxEvent *event, guint *accepted, guint *refused,
              guint *unanswered)
{
  *accepted = *refused = *unanswered = 0;
  for (guint i = 0; i < event->targets->len; i++) {
    GhStoreOutboxTarget *t = g_ptr_array_index(event->targets, i);
    if (t->outcome == OUTCOME_ACCEPTED)
      (*accepted)++;
    else if (t->outcome == OUTCOME_REJECTED)
      (*refused)++;
    else
      (*unanswered)++;
  }
}

/* Where the entry's Commit stands when libmarmot no longer has it pending. */
static GhMlsCommitState
finished_state(const GhStoreOutboxEntry *entry)
{
  if (entry->state == GH_STORE_OUTBOX_CANCELLED)
    return g_strcmp0(entry->last_error, GH_MLS_COMMIT_SUPERSEDED_REASON) == 0
             ? GH_MLS_COMMIT_SUPERSEDED : GH_MLS_COMMIT_CLEARED;
  return GH_MLS_COMMIT_MERGED;
}

static gboolean
record_answer_locked(GhStore *store, Marmot *marmot, MarmotStorage *storage,
                     const GhMlsCommitPublish *publish, const MarmotGroupId *gid,
                     const gchar *relay_url, gboolean accepted, const gchar *relay_message,
                     GhMlsCommitState *out_state, GError **error)
{
  GhStoreTargetOutcome outcome = {
    .relay_url = relay_url,
    .outcome = accepted ? OUTCOME_ACCEPTED : OUTCOME_REJECTED,
    .ok_prefix = -1,
    .ok_message = relay_message,
    .count_attempt = TRUE,
  };
  if (!gh_store_record_outcome(store, publish->outbox_event_id, &outcome, error))
    return FALSE;
  g_autoptr(GhStoreOutboxEntry) entry = gh_store_outbox_load(store, publish->outbox_id, error);
  if (!entry)
    return FALSE;
  GhStoreOutboxEvent *event = commit_event_of(entry);
  if (!event || event->id != publish->outbox_event_id) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "No such MLS Commit in the outbox");
    return FALSE;
  }

  char *pending_json = NULL;
  bool superseded = false;
  MarmotError err = marmot_get_pending_commit(marmot, gid, &pending_json, &superseded);
  if (err != MARMOT_OK)
    return marmot_fail(storage, err, "Reading the pending Commit", error);
  gboolean ours = pending_json && strcmp(pending_json, publish->event_json) == 0;
  free(pending_json);

  guint n_accepted = 0, n_refused = 0, n_unanswered = 0;
  count_targets(event, &n_accepted, &n_refused, &n_unanswered);
  GhMlsCommitState state;
  const gchar *reason = GH_MLS_COMMIT_REFUSED_REASON;   /* of a CLEARED one */
  if (!ours) {
    /* Merged already (an earlier OK, its relay echo, before a crash) or
     * dropped: only the answer is new. */
    state = finished_state(entry);
  } else if (accepted) {
    err = marmot_merge_pending_commit(marmot, gid);
    if (err == MARMOT_ERR_WRONG_EPOCH) {
      state = GH_MLS_COMMIT_SUPERSEDED;
    } else if (err == MARMOT_OK) {
      state = GH_MLS_COMMIT_MERGED;
    } else {
      /* libmarmot drops a Commit that can no longer be authorized; a
       * storage error keeps it pending. Only the latter is a failure: the
       * drop must stick, or every later OK would fail again. */
      char *still = NULL;
      MarmotError perr = marmot_get_pending_commit(marmot, gid, &still, NULL);
      gboolean dropped = perr == MARMOT_OK && !still;
      free(still);
      if (!dropped)
        return marmot_fail(storage, err, "Merging the Commit", error);
      drop_stale_error(storage);
      state = GH_MLS_COMMIT_CLEARED;
      reason = GH_MLS_COMMIT_INVALID_REASON;
    }
  } else if (n_accepted == 0 && n_unanswered == 0) {
    /* Every relay refused it: nobody can have it. */
    err = marmot_clear_pending_commit(marmot, gid);
    if (err != MARMOT_OK)
      return marmot_fail(storage, err, "Clearing the Commit", error);
    state = GH_MLS_COMMIT_CLEARED;
  } else {
    state = GH_MLS_COMMIT_PENDING;
  }

  if (entry->state != GH_STORE_OUTBOX_CANCELLED && entry->state != GH_STORE_OUTBOX_SETTLED) {
    gboolean ok = TRUE;
    if (state == GH_MLS_COMMIT_SUPERSEDED)
      ok = outbox_set(store, entry->id, GH_STORE_OUTBOX_CANCELLED,
                      GH_MLS_COMMIT_SUPERSEDED_REASON, error);
    else if (state == GH_MLS_COMMIT_CLEARED)
      ok = outbox_set(store, entry->id, GH_STORE_OUTBOX_CANCELLED, reason, error);
    else if (state == GH_MLS_COMMIT_MERGED)
      ok = outbox_set(store, entry->id, n_unanswered == 0 ? GH_STORE_OUTBOX_SETTLED
                                                          : GH_STORE_OUTBOX_PUBLISHING,
                      NULL, error);
    else if (entry->state == GH_STORE_OUTBOX_SEALED)
      ok = outbox_set(store, entry->id, GH_STORE_OUTBOX_PUBLISHING, NULL, error);
    if (!ok)
      return FALSE;
  }
  if (out_state)
    *out_state = state;
  return TRUE;
}

gboolean
gh_mls_commit_record_answer(GhStore *store, Marmot *marmot, MarmotStorage *storage,
                            const GhMlsCommitPublish *publish, const gchar *relay_url,
                            gboolean accepted, const gchar *relay_message,
                            GhMlsCommitState *out_state, GError **error)
{
  g_return_val_if_fail(store && marmot && storage && publish && relay_url, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  drop_stale_error(storage);

  gsize gid_len = 0;
  g_autofree guchar *gid_bytes = NULL;
  if (publish->group_id_hex && strlen(publish->group_id_hex) % 2 == 0 &&
      strlen(publish->group_id_hex) > 0) {
    gid_len = strlen(publish->group_id_hex) / 2;
    gid_bytes = g_malloc(gid_len);
    for (gsize i = 0; i < gid_len && gid_bytes; i++) {
      gint hi = g_ascii_xdigit_value(publish->group_id_hex[2 * i]);
      gint lo = g_ascii_xdigit_value(publish->group_id_hex[2 * i + 1]);
      if (hi < 0 || lo < 0)
        g_clear_pointer(&gid_bytes, g_free);
      else
        gid_bytes[i] = (guchar) (hi << 4 | lo);
    }
  }
  if (!gid_bytes) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Bad MLS group id");
    return FALSE;
  }
  MarmotGroupId gid = { .data = gid_bytes, .len = gid_len };

  if (!gh_store_begin(store, error))
    return FALSE;
  GhMlsCommitState state = GH_MLS_COMMIT_PENDING;
  if (!record_answer_locked(store, marmot, storage, publish, &gid, relay_url, accepted,
                            relay_message, &state, error)) {
    gh_store_rollback(store);
    return FALSE;
  }
  if (!gh_store_commit(store, error))
    return FALSE;
  if (out_state)
    *out_state = state;
  return TRUE;
}

/* ---- Restart --------------------------------------------------------------------------- */

/* One group's pending Commit, in its own transaction: *out_publish is NULL
 * when there is nothing to publish. @pending_ids collects the event ids of
 * the Commits still pending. */
static gboolean
resume_group(GhStore *store, Marmot *marmot, MarmotStorage *storage, const MarmotGroupId *gid,
             gboolean active, const gchar *account_pubkey, GHashTable *pending_ids,
             GhMlsCommitPublish **out_publish, GError **error)
{
  *out_publish = NULL;
  g_autofree gchar *gid_hex = marmot_group_id_to_hex(gid);
  char *pending_json = NULL;
  bool superseded = false;
  g_autofree gchar *event_id = NULL;
  g_auto(GStrv) relays = NULL;
  g_autoptr(GhStoreOutboxEntry) entry = NULL;
  g_autoptr(GError) lookup = NULL;
  gint64 created_at = 0, conversation = 0, outbox_id = 0;
  if (!gid_hex)
    return TRUE;
  if (!gh_store_begin(store, error))
    return FALSE;
  MarmotError err = marmot_get_pending_commit(marmot, gid, &pending_json, &superseded);
  if (err != MARMOT_OK) {
    marmot_fail(storage, err, "Reading the pending Commit", error);
    goto fail;
  }
  if (!pending_json)
    return gh_store_commit(store, error);   /* possibly finished a merged one */
  if (!(event_id = event_id_of(pending_json, &created_at, error)))
    goto fail;
  if (!active) {
    /* A group we left: nothing to publish, but its entry is not finished. */
    g_hash_table_add(pending_ids, g_strdup(event_id));
    free(pending_json);
    return gh_store_commit(store, error);
  }

  if (superseded) {
    /* Another member's Commit won: drop ours, cancel its entry. */
    err = marmot_merge_pending_commit(marmot, gid);
    if (err != MARMOT_ERR_WRONG_EPOCH && err != MARMOT_OK) {
      marmot_fail(storage, err, "Dropping a superseded Commit", error);
      goto fail;
    }
    if (gh_store_find_conversation(store, GH_STORE_BACKEND_MLS, gid_hex, &conversation,
                                   &lookup) &&
        gh_store_outbox_find_by_rumor(store, conversation, event_id, &outbox_id, &lookup)) {
      entry = gh_store_outbox_load(store, outbox_id, error);
      if (!entry)
        goto fail;
      if (entry->state != GH_STORE_OUTBOX_CANCELLED &&
          entry->state != GH_STORE_OUTBOX_SETTLED &&
          !outbox_set(store, outbox_id, GH_STORE_OUTBOX_CANCELLED,
                      GH_MLS_COMMIT_SUPERSEDED_REASON, error))
        goto fail;
    } else if (!g_error_matches(lookup, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
      g_propagate_error(error, g_steal_pointer(&lookup));
      goto fail;
    }
    free(pending_json);
    return gh_store_commit(store, error);
  }

  if (!(relays = group_relays(marmot, storage, gid, error)))
    goto fail;
  entry = ensure_entry(store, gid_hex, account_pubkey, event_id, pending_json, created_at,
                       (const gchar *const *) relays, error);
  if (!entry)
    goto fail;
  if (entry->state == GH_STORE_OUTBOX_CANCELLED || entry->state == GH_STORE_OUTBOX_SETTLED) {
    /* Dropped or merged before, yet libmarmot still holds it: the two
     * disagree, which the shared transactions rule out. Fail closed:
     * publish nothing. */
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
                        "A finished Commit is still pending");
    goto fail;
  }
  if (!gh_store_commit(store, error)) {
    free(pending_json);
    return FALSE;
  }
  g_hash_table_add(pending_ids, g_strdup(event_id));
  *out_publish = publish_of(gid_hex, entry, commit_event_of(entry),
                            (const gchar *const *) relays);
  free(pending_json);
  return TRUE;

fail:
  gh_store_rollback(store);
  free(pending_json);
  return FALSE;
}

/* Unfinished Commit entries of this module whose Commit is no longer
 * pending merged (by relay echo, or before a crash): settle them. */
static gboolean
settle_merged(GhStore *store, GHashTable *pending_ids, GError **error)
{
  g_autoptr(GArray) ids = gh_store_outbox_list_unfinished(store, GH_STORE_BACKEND_MLS, error);
  if (!ids)
    return FALSE;
  for (guint i = 0; i < ids->len; i++) {
    gint64 outbox_id = g_array_index(ids, gint64, i);
    g_autoptr(GhStoreOutboxEntry) entry = gh_store_outbox_load(store, outbox_id, error);
    if (!entry)
      return FALSE;
    GhStoreOutboxEvent *event = commit_event_of(entry);
    if (!event || !event->event_id)
      continue;
    g_autofree gchar *op_id = op_id_for(event->event_id);
    if (g_strcmp0(op_id, entry->op_id) != 0 ||
        g_hash_table_contains(pending_ids, event->event_id))
      continue;   /* not one of ours, or still pending */
    if (!outbox_set(store, outbox_id, GH_STORE_OUTBOX_SETTLED, NULL, error))
      return FALSE;
  }
  return TRUE;
}

GPtrArray *
gh_mls_commit_resume(GhStore *store, Marmot *marmot, MarmotStorage *storage,
                     const gchar *account_pubkey, GError **error)
{
  g_return_val_if_fail(store && marmot && storage && account_pubkey, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  drop_stale_error(storage);

  MarmotGroup **groups = NULL;
  size_t n_groups = 0;
  MarmotError err = marmot_get_all_groups(marmot, &groups, &n_groups);
  if (err != MARMOT_OK) {
    marmot_fail(storage, err, "Listing the MLS groups", error);
    return NULL;
  }
  g_autoptr(GPtrArray) out =
    g_ptr_array_new_with_free_func((GDestroyNotify) gh_mls_commit_publish_free);
  g_autoptr(GHashTable) pending_ids = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                           g_free, NULL);
  gboolean ok = TRUE;
  for (size_t i = 0; i < n_groups; i++) {
    if (ok) {
      GhMlsCommitPublish *publish = NULL;
      ok = resume_group(store, marmot, storage, &groups[i]->mls_group_id,
                        groups[i]->state == MARMOT_GROUP_STATE_ACTIVE, account_pubkey,
                        pending_ids, &publish, error);
      if (publish)
        g_ptr_array_add(out, publish);
    }
    marmot_group_free(groups[i]);
  }
  free(groups);
  if (!ok || !settle_merged(store, pending_ids, error))
    return NULL;
  return g_steal_pointer(&out);
}
