/* GhDraftSync as the Drafts page's backend (gh-drafts-page.h): the UI sees
 * status, conflict copies with the people's npubs, and the three actions;
 * never a GhStore or a hex room key it has to interpret. */
#include "gh-draft-sync-backend.h"

#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <string.h>

static gchar *
npub_of(const gchar *pubkey_hex)
{
  guint8 bytes[32];
  char *npub = NULL;
  if (!nostr_hex2bin(bytes, pubkey_hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return g_strdup(pubkey_hex);
  gchar *out = g_strdup(npub);
  free(npub);
  return out;
}

static guint64 generation(GObject *owner) { return gh_draft_sync_get_generation(GH_DRAFT_SYNC(owner)); }
static gboolean enabled(GObject *owner) { return gh_draft_sync_is_enabled(GH_DRAFT_SYNC(owner)); }
static const gchar *status(GObject *owner) { return gh_draft_sync_get_status(GH_DRAFT_SYNC(owner)); }
static gboolean can_retry(GObject *owner) { return gh_draft_sync_can_retry(GH_DRAFT_SYNC(owner)); }
static void retry(GObject *owner) { gh_draft_sync_retry(GH_DRAFT_SYNC(owner)); }

static GPtrArray *
list_conflicts(GObject *owner, GError **error)
{
  GhDraftSync *sync = GH_DRAFT_SYNC(owner);
  const gchar *account = gh_draft_sync_get_account(sync);
  g_autoptr(GPtrArray) stored = gh_draft_sync_list_conflicts(sync, error);
  if (!stored)
    return NULL;
  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify) gh_drafts_conflict_free);
  for (guint i = 0; i < stored->len; i++) {
    GhStoreDraftConflict *row = stored->pdata[i];
    GhDraftsConflict *conflict = g_new0(GhDraftsConflict, 1);
    conflict->id = row->id;
    conflict->text = g_strdup(row->text);
    conflict->saved_at = row->saved_at;
    conflict->room_key = g_strdup(row->room_key);
    g_auto(GStrv) members = g_strsplit(row->room_key, ",", -1);
    g_autoptr(GStrvBuilder) npubs = g_strv_builder_new();
    guint n = g_strv_length(members);
    for (guint j = 0; members[j]; j++)
      if (n == 1 || g_strcmp0(members[j], account)) {
        g_autofree gchar *npub = npub_of(members[j]);
        g_strv_builder_add(npubs, npub);
      }
    conflict->npubs = g_strv_builder_end(npubs);
    g_ptr_array_add(result, conflict);
  }
  return result;
}

static gboolean restore(GObject *owner, gint64 id, GError **error)
{ return gh_draft_sync_restore_conflict(GH_DRAFT_SYNC(owner), id, error); }
static gboolean discard(GObject *owner, gint64 id, GError **error)
{ return gh_draft_sync_discard_conflict(GH_DRAFT_SYNC(owner), id, error); }
static gboolean keep(GObject *owner, const gchar *room_key, const gchar *text, GError **error)
{ return gh_draft_sync_keep_conflict(GH_DRAFT_SYNC(owner), room_key, text, error); }

GhDraftsBackend
gh_draft_sync_backend(GhDraftSync *self)
{
  g_return_val_if_fail(GH_IS_DRAFT_SYNC(self), (GhDraftsBackend) { 0 });
  return (GhDraftsBackend) {
    .owner = G_OBJECT(self), .generation = generation, .enabled = enabled, .status = status,
    .can_retry = can_retry, .retry = retry, .list_conflicts = list_conflicts,
    .restore = restore, .discard = discard, .keep = keep,
  };
}
