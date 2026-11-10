#ifndef GH_STORE_DRAFTS_H
#define GH_STORE_DRAFTS_H

#include "gh-store.h"

G_BEGIN_DECLS

/* GhStoreDrafts: the NIP-37 draft-synchronization state beside
 * conversations.draft (plan §3.4 schema v13, §3.7; nostrc-9lwl7.8). Every
 * row is keyed by the canonical NIP-17 room key (conversations.backend_key of
 * a NIP-17 room). All text stays inside the account's encrypted store: the
 * only things that leave it are the signed events in draft_publications,
 * whose content is NIP-44 ciphertext to the account.
 *
 * The functions marked "inside the caller's transaction" are the hooks the
 * store itself runs: the draft setter (gh-store-conversations.c), the actual
 * enqueue (gh_store_enqueue(), chat kinds only) and forget
 * (gh_store_forget_conversation()); they commit or roll back with that
 * transaction, so a draft, its revision and its tombstone never disagree.
 * Everything else is called by GhDraftSync (src/app/gh-draft-sync.c) on the
 * store's thread, each in its own transaction. */

typedef struct {
  gchar *room_key;
  gchar *draft_d;          /* the identifier local revisions publish under; NULL until assigned */
  GStrv aliases;           /* other identifiers learned for the room (never NULL) */
  gint64 local_revision;   /* bumped by every local save or clear */
  gboolean dirty;          /* a local revision no relay has accepted yet */
  gchar *last_event_id;    /* the newest revision a relay holds, as far as known */
  gint64 last_created_at;
  gint64 cleared_at;       /* local clear tombstone (unix seconds), 0 = none */
  gint64 expires_at;       /* expiration of the last accepted publication */
} GhStoreDraftState;
void gh_store_draft_state_free(GhStoreDraftState *state);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhStoreDraftState, gh_store_draft_state_free)

typedef enum {
  GH_STORE_DRAFT_PUBLICATION_QUEUED = 0, /* a blank is owed for d; no event yet */
  GH_STORE_DRAFT_PUBLICATION_SIGNED = 1  /* event_json awaits a relay's acceptance */
} GhStoreDraftPublicationState;

typedef struct {
  gchar *room_key;
  gchar *d;
  gint64 local_revision;
  gchar *event_json;       /* "" while QUEUED */
  GStrv targets;           /* the kind-10013 relays it was signed for */
  gchar *outcomes_json;    /* the service's per-relay outcome record (opaque here) */
  GhStoreDraftPublicationState state;
} GhStoreDraftPublication;
void gh_store_draft_publication_free(GhStoreDraftPublication *publication);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhStoreDraftPublication, gh_store_draft_publication_free)

typedef struct {
  gint64 id;
  gchar *room_key;
  gchar *text;
  gchar *source_event_id;  /* the remote revision involved, or NULL */
  gint64 saved_at;
} GhStoreDraftConflict;
void gh_store_draft_conflict_free(GhStoreDraftConflict *conflict);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhStoreDraftConflict, gh_store_draft_conflict_free)

/* ---- Hooks inside the caller's transaction ------------------------------------------ */

/* A draft text was saved for the conversation: local_revision + 1, dirty.
 * Nothing for a room of another backend. */
gboolean gh_store_drafts_touch(GhStore *store, gint64 conversation_id, GError **error);
/* The conversation's draft was cleared locally (the user emptied it, a chat
 * message was enqueued, or the conversation was forgotten): cleared_at =
 * MAX(cleared_at, now), local_revision + 1, and a blank publication queued
 * for every known identifier (dirty only when there is one, so a later
 * enable clears the relays too). A room never published keeps only the
 * tombstone. Nothing for a room of another backend. */
gboolean gh_store_drafts_clear(GhStore *store, gint64 conversation_id, gint64 now,
                               GError **error);

/* ---- State ----------------------------------------------------------------------------- */

/* The room's row; NULL without an error when there is none. */
GhStoreDraftState *gh_store_drafts_get(GhStore *store, const gchar *room_key, GError **error);
/* Every row (array of GhStoreDraftState). */
GPtrArray *gh_store_drafts_list(GhStore *store, GError **error);
/* The room an identifier belongs to (draft_d or an alias), or NULL. */
gchar *gh_store_drafts_find_room(GhStore *store, const gchar *identifier, GError **error);
/* The local draft and room facts the import rules need. A room that is not
 * stored is not blocked, has no tombstone and no draft. */
gboolean gh_store_drafts_get_room(GhStore *store, const gchar *room_key, gboolean *out_blocked,
                                  gint64 *out_forgotten_before, gchar **out_draft,
                                  GError **error);
/* Deletes rows whose publication expired and that owe nothing (dirty = 0),
 * so a tombstone lives exactly as long as a late revision could. */
gboolean gh_store_drafts_prune(GhStore *store, gint64 now, GError **error);
/* The room keys of every NIP-17 room with a local draft (array of strings). */
GPtrArray *gh_store_drafts_list_local(GhStore *store, GError **error);
/* Scheduling decisions of the service: a room whose draft must be published
 * (local_revision + 1, dirty), or one whose local revision is settled or
 * superseded by the relays (dirty = 0). */
gboolean gh_store_drafts_mark_dirty(GhStore *store, const gchar *room_key, GError **error);
gboolean gh_store_drafts_mark_clean(GhStore *store, const gchar *room_key, GError **error);

/* ---- Identifiers ----------------------------------------------------------------------- */

/* A new random 256-bit identifier for a room without one (*out_d, owned).
 * The row is created when absent. A room that already has one keeps it. */
gboolean gh_store_drafts_assign_identifier(GhStore *store, const gchar *room_key,
                                           gchar **out_d, GError **error);
/* An identifier learned from a decrypted remote revision. The lowest known
 * identifier becomes draft_d (future local revisions go under it); the others
 * are aliases that get blanked once a newer head is accepted. */
gboolean gh_store_drafts_add_identifier(GhStore *store, const gchar *room_key,
                                        const gchar *identifier, GError **error);
/* An alias whose blank a relay accepted. */
gboolean gh_store_drafts_remove_alias(GhStore *store, const gchar *room_key,
                                      const gchar *identifier, GError **error);

/* ---- Remote revisions ---------------------------------------------------------------- */

/* Records a remote revision as the newest known (last_event_id,
 * last_created_at, expires_at) and, with apply_text, sets conversations.draft
 * to text (NULL: a remote clear, which also raises cleared_at so an older
 * revision cannot resurrect it), creating the room as accepted when it is not
 * stored. Never touches local_revision or dirty: a remote revision is not a
 * local edit. */
gboolean gh_store_drafts_apply_remote(GhStore *store, const gchar *room_key, const gchar *text,
                                      const gchar *event_id, gint64 created_at, gint64 expires_at,
                                      gboolean apply_text, GError **error);

/* ---- Publications ---------------------------------------------------------------------- */

/* The signed event for (room, d) at local_revision, replacing a queued blank
 * or an older event; targets are the kind-10013 relays it goes to. */
gboolean gh_store_drafts_save_publication(GhStore *store, const gchar *room_key, const gchar *d,
                                          gint64 local_revision, const gchar *event_json,
                                          const gchar *const *targets, GError **error);
gboolean gh_store_drafts_set_outcomes(GhStore *store, const gchar *room_key, const gchar *d,
                                      const gchar *outcomes_json, GError **error);
/* A relay accepted the publication of (room, d): the row goes, and when it
 * carried the room's current local_revision the room is clean with
 * last_event_id/last_created_at/expires_at set from it. A newer local
 * revision keeps the room dirty. */
gboolean gh_store_drafts_accept(GhStore *store, const gchar *room_key, const gchar *d,
                                gint64 local_revision, const gchar *event_id, gint64 created_at,
                                gint64 expires_at, gboolean *out_current, GError **error);
gboolean gh_store_drafts_delete_publication(GhStore *store, const gchar *room_key, const gchar *d,
                                            GError **error);
/* Every pending publication (array of GhStoreDraftPublication). */
GPtrArray *gh_store_drafts_list_publications(GhStore *store, GError **error);
/* Sync switched off: unsent events are deleted (the tombstones and dirty
 * flags stay, so a later enable publishes from current state). */
gboolean gh_store_drafts_delete_unsent(GhStore *store, GError **error);

/* ---- Conflict copies ------------------------------------------------------------------- */

gboolean gh_store_drafts_add_conflict(GhStore *store, const gchar *room_key, const gchar *text,
                                      const gchar *source_event_id, gint64 saved_at,
                                      GError **error);
/* Newest first; room_key NULL lists every room's. Array of GhStoreDraftConflict. */
GPtrArray *gh_store_drafts_list_conflicts(GhStore *store, const gchar *room_key, GError **error);
gboolean gh_store_drafts_delete_conflict(GhStore *store, gint64 id, GError **error);

G_END_DECLS
#endif
