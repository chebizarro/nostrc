#ifndef GH_DRAFT_SYNC_H
#define GH_DRAFT_SYNC_H

#include "gh-account-store.h"
#include "gh-social-lists.h"
#include "gh-store-conversations.h"
#include "gh-store-drafts.h"

G_BEGIN_DECLS

/*
 * GhDraftSync: NIP-37 synchronization of the account's NIP-17 drafts (plan
 * §3.7, nostrc-9lwl7.8). Owned by GhAppServices beside GhSocialLists, whose
 * account lifecycle, "Sync Drafts and Lists" switch, kind-10013 private
 * relay list and transports it shares; it keeps no list state of its own.
 *
 *  - Local drafts stay in conversations.draft; the store's own transactions
 *    keep draft_sync_state in step (gh-store-drafts.h). This service only
 *    wakes on "draft-changed" (GhStoreConversations), "enqueued" (GhOutbox)
 *    and the sync switch, and then publishes or imports.
 *  - Publication: kind 31234, a random opaque `d` per room (never derived
 *    from the participants), `k` 14, expiration 30 days, content the
 *    NIP-44 ciphertext (to the account) of an unsigned kind-14 event that
 *    names the room only inside. Revisions follow NIP-01 ordering with
 *    created_at = MAX(now, last + 1). Only the configured, unblocked
 *    kind-10013 relays are used, with OWN_PRIVATE_SYNC_PUBLISH; nothing
 *    falls back to write, inbox or discovery relays.
 *  - Cadence: 30 s after the last edit (5 min, or leaving the room, for a
 *    NIP-46 account, whose signatures take the signer's bulk lane), at most
 *    once per room per 60 s, one encrypt/sign pipeline at a time; a revision
 *    changed during signing discards the result; a signer denial stops
 *    attempts for that revision until Retry or an edit; a relay failure
 *    retries the stored signed event without a new signature.
 *  - Import: one live REQ {kinds:[31234], authors:[me], #k:["14"]} on those
 *    relays with OWN_PRIVATE_SYNC_READ; no new local identifier before every
 *    relay reached EOSE; at most 64 decryptions per binding, newest first,
 *    one at a time; undecryptable, malformed, expired, blocked-room and
 *    forgotten revisions are ignored (never blanked) and counted as
 *    incomplete. A newer revision replaces a clean local draft through the
 *    applier (a programmatic composer update), is kept as a conflict copy
 *    when the local draft is dirty or the composer is focused with other
 *    text, and is only recorded when the text is the same.
 *  - Enabling: existing drafts of unknown age are reconciled after EOSE:
 *    published when the relays have nothing, otherwise superseded by the
 *    remote revision with the local text kept as a conflict copy.
 *
 * Signals: changed (), after the status or import state changed. Main
 * context only.
 */

#define GH_TYPE_DRAFT_SYNC (gh_draft_sync_get_type())
G_DECLARE_FINAL_TYPE(GhDraftSync, gh_draft_sync, GH, DRAFT_SYNC, GObject)

/* The window side of a remote revision (gh-send-ui.h implements it). */
typedef struct {
  /* Whether the composer shows room_key and has keyboard focus. */
  gboolean (*composer_focused)(const gchar *room_key, gpointer data);
  /* Show text (NULL: empty) for room_key: a programmatic update that emits
   * no draft change, listing the room in the model when it is not yet. */
  void (*apply)(const gchar *room_key, const gchar *text, gpointer data);
} GhDraftSyncApplier;

#define GH_DRAFT_SYNC_LOCAL_DELAY_SECONDS   30
#define GH_DRAFT_SYNC_REMOTE_DELAY_SECONDS  300
#define GH_DRAFT_SYNC_RATE_SECONDS          60
#define GH_DRAFT_SYNC_RETRY_SECONDS         60
#define GH_DRAFT_SYNC_EXPIRATION_SECONDS    (30 * 24 * 60 * 60)
#define GH_DRAFT_SYNC_MAX_IMPORT            64

GhDraftSync *gh_draft_sync_new(GhSocialLists *social);
/* Transports and the account controller (copied, as GhSocialLists'). */
void gh_draft_sync_set_network(GhDraftSync *self, const GhListAdapterNetwork *network);
/* Follows the account store's bindings: binds once its store, conversations
 * and outbox exist, closes before the store closes. */
void gh_draft_sync_attach_store(GhDraftSync *self, GhAccountStore *account_store);
/* Explicit binding seam (tests, session stores): store and conversations are
 * borrowed until gh_draft_sync_close(). */
gboolean gh_draft_sync_bind_store(GhDraftSync *self, GhStore *store,
                                  GhStoreConversations *conversations, guint64 generation,
                                  GError **error);
/* The outbox whose "enqueued" wakes the scheduler (a GhOutbox). */
void gh_draft_sync_watch_outbox(GhDraftSync *self, GObject *outbox);
void gh_draft_sync_close(GhDraftSync *self);

void gh_draft_sync_set_applier(GhDraftSync *self, const GhDraftSyncApplier *applier,
                               gpointer data);
/* The room the composer shows (NULL: none); leaving a room schedules its
 * latest saved revision. */
void gh_draft_sync_set_shown_room(GhDraftSync *self, const gchar *room_key);
/* A local change committed: re-evaluates the schedule. */
void gh_draft_sync_wake(GhDraftSync *self);
/* Tries again after a signer denial or a relay failure. */
void gh_draft_sync_retry(GhDraftSync *self);

/* NULL while nothing needs saying; otherwise "Choose a Private Relay to Sync
 * Drafts", "Saved on This Device · Waiting for Signer", ... */
const gchar *gh_draft_sync_get_status(GhDraftSync *self);
/* Import state of this binding: every relay reached EOSE; revisions
 * decrypted; events that could not be imported (bound, undecryptable). */
void gh_draft_sync_get_import_state(GhDraftSync *self, gboolean *out_complete,
                                    guint *out_decrypted, guint *out_incomplete);
gboolean gh_draft_sync_is_active(GhDraftSync *self);

G_END_DECLS
#endif
