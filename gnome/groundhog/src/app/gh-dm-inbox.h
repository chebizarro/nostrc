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
  GH_DM_INBOX_BACKFILLING,     /* an inbox relay answered; one has no EOSE yet */
  GH_DM_INBOX_LIVE,            /* every reachable inbox relay sent EOSE */
  GH_DM_INBOX_ERROR            /* seen-set unusable, or every relay failed */
} GhDmInboxState;

GType gh_dm_inbox_state_get_type(void);
#define GH_TYPE_DM_INBOX_STATE (gh_dm_inbox_state_get_type())

/* Since policy. NIP-59 wraps carry a created_at randomized up to two days
 * into the past, so a wrap published at time T can claim T - 2d. The inbox
 * keeps a per-account checkpoint C: a wall-clock time by which every wrap the
 * inbox relays had was received and settled (all relays at EOSE, nothing
 * queued or in flight, and no deferred wrap this session). A REQ asks for
 * since = C - WRAP_SKEW (two days plus an hour of clock slack), and without a
 * checkpoint for since = now - INITIAL_BACKFILL. REQ_LIMIT bounds each
 * relay's stored answer (the newest wraps win). Replayed wraps cost no signer
 * call: the seen-set skips them first. */
#define GH_DM_INBOX_WRAP_SKEW        ((gint64)(2 * 24 + 1) * 3600)
#define GH_DM_INBOX_INITIAL_BACKFILL ((gint64)30 * 24 * 3600)
#define GH_DM_INBOX_REQ_LIMIT        1000
/* Seen-set keys (two per message: wrap id and rumor id). */
#define GH_DM_INBOX_SEEN_CAPACITY    16384
/* Wraps waiting for the signer; more are deferred to a later REQ. */
#define GH_DM_INBOX_QUEUE_LIMIT      1024
/* Unwraps waiting on the signer at once (each may be a visible approval). */
#define GH_DM_INBOX_MAX_IN_FLIGHT    4

typedef struct {
  guint received;   /* verified kind-1059 events delivered by the scope */
  guint skipped;    /* wrap already seen or queued: no signer call made */
  guint admitted;   /* new messages added to the conversation store */
  guint duplicates; /* unwrapped, but its rumor was already stored or seen */
  guint rejected;   /* failed NIP-17 validation; counted, never shown */
  guint deferred;   /* signer error or full queue; a later REQ retries it */
  guint pending;    /* queued plus in flight */
  guint in_flight;  /* unwraps waiting on the signer */
} GhDmInboxCounters;

#define GH_TYPE_DM_INBOX (gh_dm_inbox_get_type())
G_DECLARE_FINAL_TYPE(GhDmInbox, gh_dm_inbox, GH, DM_INBOX, GObject)

/* The active account's NIP-17 receive pipeline. Per account generation it
 * binds store to the account (clearing the previous one's conversations) and
 * opens the per-account seen-set <state_dir>/<pubkey>.seen and checkpoint
 * <state_dir>/<pubkey>.checkpoint (both 0600; state_dir NULL means
 * $XDG_STATE_HOME/groundhog/nip17). It then keeps one URL-scoped live REQ
 * {kinds:[1059], #p:[account], since, limit} open on exactly the account's
 * own kind-10050 relays from relays, and on nothing else.
 *
 * The seen-set becomes store's persistence delegate for the account (see
 * GhConversationDelegate; the encrypted store replaces it). Each delivered
 * wrap is checked with gh_conversation_store_has_wrap() before any signer
 * call, queued, and unwrapped by gh_nip17_unwrap_async() at most
 * max-in-flight at a time. A verified message goes through the single
 * gh_conversation_store_admit() call, which commits it and its seen keys
 * together; a rejected wrap is only counted, and a failed commit is deferred.
 *
 * An account switch or a change of the inbox relay set tears the session down
 * first (scope cancelled, pending unwraps and their signer approvals
 * cancelled) and only then starts the next; a result from an older session or
 * generation is discarded. "changed" is emitted on the main context whenever
 * the state or a counter changes; "state" also notifies. transport NULL uses
 * gnostr relays. */
GhDmInbox *gh_dm_inbox_new(GhAccountController *accounts,
                           GhAccountRelays *relays,
                           GhConversationStore *store,
                           const gchar *state_dir,
                           const GhRelayTransport *transport,
                           gpointer transport_data);

/* 1 (the default) to GH_DM_INBOX_MAX_IN_FLIGHT; applies to the next unwrap. */
void gh_dm_inbox_set_max_in_flight(GhDmInbox *self, guint max_in_flight);

GhDmInboxState gh_dm_inbox_get_state(GhDmInbox *self);
/* Why the state is ERROR; NULL otherwise. */
const gchar *gh_dm_inbox_get_error(GhDmInbox *self);
/* The account generation the session belongs to; 0 when inactive. */
guint64 gh_dm_inbox_get_generation(GhDmInbox *self);
/* The sorted inbox relay URLs the current REQ targets; NULL without one. */
const gchar *const *gh_dm_inbox_get_relays(GhDmInbox *self);
/* The since of the current REQ; 0 without one. */
gint64 gh_dm_inbox_get_since(GhDmInbox *self);
/* Counters of the current account generation (reset on switch). */
void gh_dm_inbox_get_counters(GhDmInbox *self, GhDmInboxCounters *counters);

G_END_DECLS
#endif
