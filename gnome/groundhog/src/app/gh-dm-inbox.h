#ifndef GH_DM_INBOX_H
#define GH_DM_INBOX_H

#include "gh-account-relays.h"
#include "gh-conversation-store.h"

G_BEGIN_DECLS

typedef enum {
  GH_DM_INBOX_INACTIVE,        /* no active account */
  GH_DM_INBOX_NO_INBOX_RELAYS, /* no admitted kind-10050 list (yet) for the
                                * account, or it names no usable ws(s) URL */
  GH_DM_INBOX_CONNECTING,      /* REQ issued; no inbox relay has answered */
  GH_DM_INBOX_BACKFILLING,     /* an inbox relay answered; one has no EOSE yet
                                * or is still being paged backwards */
  GH_DM_INBOX_LIVE,            /* every reachable inbox relay sent EOSE and was
                                * paged (see the relay states for any relay
                                * whose backfill stayed incomplete) */
  GH_DM_INBOX_ERROR,           /* seen-set unusable, or every relay failed */
  GH_DM_INBOX_NO_STORAGE       /* storage mode: the account's store is not open
                                * (locked, unavailable, opening...), so nothing
                                * is subscribed; see gh_dm_inbox_set_storage() */
} GhDmInboxState;

GType gh_dm_inbox_state_get_type(void);
#define GH_TYPE_DM_INBOX_STATE (gh_dm_inbox_state_get_type())

/* One inbox relay of the current session, as the UI explains it. */
typedef enum {
  GH_DM_INBOX_RELAY_NONE,          /* not an inbox relay of this session */
  GH_DM_INBOX_RELAY_CONNECTING,    /* REQ issued; not answered yet */
  GH_DM_INBOX_RELAY_WAITING_FOR_APPROVAL, /* the relay asked to sign in; the
                                    * signer is asking the user (§4.4 R6) */
  GH_DM_INBOX_RELAY_BACKFILLING,   /* answering, or older pages are fetched */
  GH_DM_INBOX_RELAY_LIVE,          /* everything it holds since the window
                                    * start was fetched; now live */
  GH_DM_INBOX_RELAY_INCOMPLETE,    /* live, but older messages could not all be
                                    * fetched this session (page bound, a failed
                                    * or unpageable page): the checkpoint is
                                    * held, so a later session retries */
  GH_DM_INBOX_RELAY_AUTH_REQUIRED, /* the relay serves your messages only after
                                    * sign-in, which was declined or failed */
  GH_DM_INBOX_RELAY_FAILED         /* unreachable, or it refused the REQ */
} GhDmInboxRelayState;

/* Since policy. NIP-59 wraps carry a created_at randomized up to two days
 * into the past, so a wrap published at time T can claim T - 2d. The inbox
 * keeps a per-account checkpoint C: a wall-clock time by which every wrap the
 * inbox relays had was received and settled (every relay at EOSE and fully
 * paged, nothing queued or in flight, and no deferred wrap this session). A
 * REQ asks for since = C - WRAP_SKEW (two days plus an hour of clock slack),
 * and without a checkpoint for since = now - INITIAL_BACKFILL.
 *
 * Paging (charter §4.5 S6). A relay answers a REQ with its newest REQ_LIMIT
 * wraps at most, so a relay that has delivered REQ_LIMIT distinct wraps by
 * its EOSE may hold older ones. It is paged backwards with one-shot REQs
 * {since, until = the oldest created_at seen, limit} (until is inclusive;
 * repeats are skipped by id) until a page returns fewer than REQ_LIMIT. A
 * page that cannot get older (more than a page of wraps in one second) steps
 * over that second and leaves the relay INCOMPLETE; so does a failed page or
 * a run of MAX_PAGES pages. An INCOMPLETE relay holds the checkpoint for the
 * session, and its next EOSE (after a reconnect) pages it again. Only fully
 * paged relays let the checkpoint advance. Replayed wraps cost no signer
 * call: the seen-set skips them first. */
#define GH_DM_INBOX_WRAP_SKEW        ((gint64)(2 * 24 + 1) * 3600)
#define GH_DM_INBOX_INITIAL_BACKFILL ((gint64)30 * 24 * 3600)
#define GH_DM_INBOX_REQ_LIMIT        1000
/* Older-page REQs per relay and paging run. */
#define GH_DM_INBOX_MAX_PAGES        16
/* Rejected-wrap ids kept by a memory-only inbox (gh_dm_inbox_new). */
#define GH_DM_INBOX_SEEN_CAPACITY    16384
/* Wraps waiting for the signer; more are deferred to a later REQ. */
#define GH_DM_INBOX_QUEUE_LIMIT      1024
/* Unwraps waiting on the signer at once (each may be a visible approval). */
#define GH_DM_INBOX_MAX_IN_FLIGHT    4

typedef struct {
  guint received;   /* verified kind-1059 events delivered (each relay's copy) */
  guint skipped;    /* wrap already seen, rejected, queued or deferred: no
                     * signer call made */
  guint admitted;   /* new messages added to the conversation store */
  guint duplicates; /* unwrapped, but its rumor was already stored or seen,
                     * or it was recorded as seen only (expired on arrival) */
  guint rejected;   /* failed NIP-17 validation; counted, never shown, and
                     * recorded when a signer call was made (no re-prompt) */
  guint deferred;   /* signer error or full queue; a later session retries it */
  guint pending;    /* queued plus in flight */
  guint in_flight;  /* unwraps waiting on the signer */
  guint pages;      /* older-page REQs issued (backfill paging) */
  guint backfill_incomplete; /* paging runs that ended with a relay INCOMPLETE */
} GhDmInboxCounters;

#define GH_TYPE_DM_INBOX (gh_dm_inbox_get_type())
G_DECLARE_FINAL_TYPE(GhDmInbox, gh_dm_inbox, GH, DM_INBOX, GObject)

/* The active account's NIP-17 receive pipeline, memory-only (a build without
 * the encrypted store). Per account generation it binds store to the account
 * (clearing the previous one's conversations) with a delegate that keeps the
 * messages and their seen keys (wrap id, rumor id) in memory only, and the
 * checkpoint too: nothing that would make a message count as seen outlives
 * the message itself (W13 review B1). The first session of every process
 * therefore asks for the initial backfill window, and wraps the relays still
 * hold are unwrapped (signer calls) and listed again after a restart. Only
 * the rejected-wrap namespace is kept on disk, in the account's pseudonymous
 * <state_dir>/<acct>.seen (0600; gh_nip17_seen_file_name(); state_dir NULL
 * means $XDG_STATE_HOME/groundhog/nip17): a rejected wrap never hides a
 * message. On binding, the rejected ids of the files Groundhog 0.6.0 named by
 * the pubkey (<pubkey>.seen, <pubkey>.checkpoint) are moved into it and both files
 * are deleted; their other keys and the old checkpoint are dropped. It then
 * keeps one URL-scoped live REQ
 * {kinds:[1059], #p:[account], since, limit} open on each of exactly the
 * account's own kind-10050 relays from relays, and on nothing else; older
 * pages go to the same relays only.
 *
 * NIP-42 (charter §4.3 own inbox read, §4.4). Each of those REQs uses AUTH
 * identity ACCOUNT for its own URL through a GhAccountAuth of the
 * generation: a relay that refuses the REQ with "auth-required:" after a
 * challenge gets one AUTH signed by the account's signer and the REQ again.
 * At most one signer request per relay is open at a time, and a relay whose
 * AUTH the user declined is not asked again this generation (R6); it is
 * reported as GH_DM_INBOX_RELAY_AUTH_REQUIRED. No other URL is ever
 * authenticated as the account by the inbox.
 *
 * The memory delegate is store's persistence delegate for the account (see
 * GhConversationDelegate; the encrypted store replaces it). Each delivered
 * wrap is checked with gh_conversation_store_has_wrap(), the delegate's
 * rejected namespace (gh_conversation_store_has_rejected()) and the wraps
 * queued or deferred this session before any
 * signer call, queued, and unwrapped by gh_nip17_unwrap_async() at most
 * max-in-flight at a time. A verified message goes through the single
 * gh_conversation_store_admit() call, which commits it and its seen keys
 * together. A wrap finally rejected after a signer call is recorded as
 * rejected, so no later session asks the signer about it again; one rejected
 * before any signer call is only counted. A signer error (denial, outage)
 * defers the wrap: it is not asked about again this session, and not being
 * recorded, it is offered again by a later session. A failed commit is
 * deferred too.
 *
 * An account switch or a change of the inbox relay set tears the session down
 * first (scopes cancelled, pending unwraps and their signer approvals
 * cancelled) and only then starts the next; a result from an older session or
 * generation is discarded. "changed" is emitted on the main context whenever
 * the state, a relay state or a counter changes; "state" also notifies.
 *
 * transport NULL uses gnostr relays (with their NIP-42 half). A custom
 * transport authenticates only with auth_transport (nullable: never). */
GhDmInbox *gh_dm_inbox_new(GhAccountController *accounts,
                           GhAccountRelays *relays,
                           GhConversationStore *store,
                           const gchar *state_dir,
                           const GhRelayTransport *transport,
                           const GhRelayAuthTransport *auth_transport,
                           gpointer transport_data);

/* ---- Storage mode (privacy charter §3.4, §8.2 G04) ----------------------------
 * An inbox made with gh_dm_inbox_new_with_storage() keeps nothing itself: it
 * opens no seen file, writes no checkpoint file, creates no state directory
 * and never binds store to an account. The app's account store
 * (gh-account-store.h) binds store to the account's encrypted store (or to the
 * in-memory store the user explicitly chose) and then grants it for that
 * account generation with gh_dm_inbox_set_storage(). The inbox subscribes
 * only while it holds a grant for the current generation, so every message it
 * admits is committed durably, and it is GH_DM_INBOX_NO_STORAGE otherwise
 * (charter §3.4: a locked keyring means no inbox REQ; the wraps stay on the
 * relays). The checkpoint (see "Since policy") is read and written through
 * the grant's callbacks.
 *
 * gh_dm_inbox_clear_storage() withdraws the grant: the latest checkpoint is
 * saved, then the session is torn down at once (scopes closed, queued and
 * pending unwraps and their signer approvals cancelled), so the caller may
 * detach and close the store right after it returns. An account switch
 * withdraws the grant the same way. The storage data is borrowed until then. */
typedef struct {
  /* The checkpoint saved for the account (unix seconds), or 0 for none. */
  gint64 (*load_checkpoint)(gpointer data);
  gboolean (*save_checkpoint)(gpointer data, gint64 checkpoint, GError **error);
} GhDmInboxStorage;

GhDmInbox *gh_dm_inbox_new_with_storage(GhAccountController *accounts,
                                        GhAccountRelays *relays,
                                        GhConversationStore *store,
                                        const GhRelayTransport *transport,
                                        const GhRelayAuthTransport *auth_transport,
                                        gpointer transport_data);
/* Grants storage for the account generation @generation; FALSE (and no
 * grant) unless the inbox is in storage mode and @generation is the active
 * account's current one. Replaces an earlier grant of the same generation. */
gboolean gh_dm_inbox_set_storage(GhDmInbox *self, guint64 generation,
                                 const GhDmInboxStorage *storage, gpointer data);
void gh_dm_inbox_clear_storage(GhDmInbox *self);
/* Whether a grant is held (always FALSE outside storage mode). */
gboolean gh_dm_inbox_has_storage(GhDmInbox *self);

/* 1 (the default) to GH_DM_INBOX_MAX_IN_FLIGHT; applies to the next unwrap. */
void gh_dm_inbox_set_max_in_flight(GhDmInbox *self, guint max_in_flight);
/* The REQ limit (1 to GH_DM_INBOX_REQ_LIMIT, the default; e.g. a relay's
 * smaller NIP-11 max_limit), from the next session on, and the pages per
 * paging run (1 to GH_DM_INBOX_MAX_PAGES, the default), from the next page
 * on. */
void gh_dm_inbox_set_backfill_limits(GhDmInbox *self, guint req_limit, guint max_pages);

GhDmInboxState gh_dm_inbox_get_state(GhDmInbox *self);
/* Why the state is ERROR; NULL otherwise. */
const gchar *gh_dm_inbox_get_error(GhDmInbox *self);
/* The account generation the session belongs to; 0 when inactive. */
guint64 gh_dm_inbox_get_generation(GhDmInbox *self);
/* The sorted inbox relay URLs the current REQs target; NULL without them. */
const gchar *const *gh_dm_inbox_get_relays(GhDmInbox *self);
/* The state of one of those relays; detail (nullable, borrowed until the
 * next change) is the relay's own reason or an explanation, or NULL. */
GhDmInboxRelayState gh_dm_inbox_get_relay_state(GhDmInbox *self, const gchar *url,
                                                const gchar **detail);
/* The since of the current REQs; 0 without them. */
gint64 gh_dm_inbox_get_since(GhDmInbox *self);
/* The settled checkpoint of the current account generation (see "Since
 * policy"; unix seconds), 0 for none. In storage mode it is loaded from and
 * saved through the grant; memory-only it lasts for this process only (a
 * change of the relay set reuses it). */
gint64 gh_dm_inbox_get_checkpoint(GhDmInbox *self);
/* Counters of the current account generation (reset on switch). */
void gh_dm_inbox_get_counters(GhDmInbox *self, GhDmInboxCounters *counters);

G_END_DECLS
#endif
