#ifndef GH_MLS_COMMITS_H
#define GH_MLS_COMMITS_H

#include <marmot/marmot.h>

#include "gh-store.h"

G_BEGIN_DECLS

/* GhMlsCommits (nostrc-qp24.7): the durable, encrypted lifecycle of this
 * account's own MLS Commits (add, remove, metadata) over its GhStore and a
 * libmarmot client on GhStoreMarmot (gh-store-marmot.h). GTK-free and
 * network-free: the caller publishes and reports each relay's answer. The
 * app integration (nostrc-qp24.13) drives it.
 *
 * Stage (T-mls, one transaction). The libmarmot producer leaves a pending
 * Commit: the signed kind:445 event, the state it produces and an Add's
 * Welcomes, bound to the state it was built on. The outbox entry seals that
 * same event for every group relay. Nothing is applied yet (MIP-03: never
 * before a relay accepted the Commit). A failure anywhere leaves nothing
 * behind.
 *
 * Publish and answer. The caller publishes the sealed event byte for byte on
 * each relay and reports each NIP-01 OK with gh_mls_commit_record_answer(),
 * which records it (T-outcome) in one transaction with its effect:
 *  - the first OK true merges the Commit (the epoch transition and the
 *    Welcome outbox). Later OKs are only recorded. The entry settles once
 *    every target has answered;
 *  - OK false is recorded. Once every target refused and none accepted, the
 *    Commit is cleared and the entry cancelled;
 *  - a Commit that another member's Commit superseded is dropped when the
 *    first OK arrives, and the entry is cancelled: the group follows the
 *    winner, and an Add's Welcomes must not be sent;
 *  - a timeout or a lost connection is not an answer: report nothing, and
 *    the Commit stays pending. It may still have been stored.
 *
 * Restart (idempotent). gh_mls_commit_resume() lists every pending Commit
 * with the targets that have not answered (plus group relays added since),
 * to publish the same signed event from the pending record. It is never re-signed and never rebuilt,
 * and it must match the sealed copy byte for byte, or resume fails closed.
 * Resume also:
 *  - drops a superseded Commit and cancels its entry;
 *  - re-seals the pending event if its entry is missing (both are written in
 *    one T-mls, so only a Commit staged outside this module lacks one);
 *  - settles entries whose Commit has already merged: through its relay
 *    echo (marmot_process_message() merges our own Commit), or before a
 *    crash.
 *
 * Cancelled entries carry the reason in last_error: "mls-superseded",
 * "mls-refused", or "mls-invalid" (an OK came, but libmarmot found the Commit
 * could no longer be authorized and dropped it; reported as CLEARED). An entry's op_id derives from the event id (a domain-
 * separated SHA-256), so staging is idempotent and resume can recognize its
 * own entries among the MLS backend's.
 *
 * Every write of every step is inside the encrypted store. The caller must
 * not call libmarmot's merge or clear for a Commit staged here: use this
 * module, or the outbox entry is not updated.
 *
 * Threading: like the GhStore and the Marmot, one thread at a time. */

#define GH_MLS_COMMIT_ERROR (gh_mls_commit_error_quark())
GQuark gh_mls_commit_error_quark(void);
/* GH_MLS_COMMIT_ERROR codes are MarmotError values (e.g.
 * MARMOT_ERR_OWN_COMMIT_PENDING). A storage failure is reported as the
 * underlying GH_STORE_ERROR instead (e.g. GH_STORE_ERROR_FULL). */

#define GH_MLS_COMMIT_SUPERSEDED_REASON "mls-superseded"
#define GH_MLS_COMMIT_REFUSED_REASON    "mls-refused"
#define GH_MLS_COMMIT_INVALID_REASON    "mls-invalid"

/* Makes the Commit on @group_id and returns its signed event in
 * *out_commit_json (malloc(); the module frees it). It is called inside
 * the stage transaction, e.g. with marmot_add_members(). */
typedef MarmotError (*GhMlsCommitProducer)(Marmot *marmot, const MarmotGroupId *group_id,
                                           gpointer user_data, char **out_commit_json);

typedef struct {
  gchar *group_id_hex;      /* the MLS group id */
  gint64 outbox_id;
  gint64 outbox_event_id;   /* the T-outcome key */
  gchar *event_id;          /* the kind:445's id */
  gchar *event_json;        /* signed; published byte for byte, every time */
  GStrv relay_urls;         /* targets that have not answered yet, sorted */
} GhMlsCommitPublish;

void gh_mls_commit_publish_free(GhMlsCommitPublish *publish);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhMlsCommitPublish, gh_mls_commit_publish_free)

typedef enum {
  GH_MLS_COMMIT_PENDING,    /* no relay accepted it yet */
  GH_MLS_COMMIT_MERGED,     /* a relay accepted it; the group moved on */
  GH_MLS_COMMIT_SUPERSEDED, /* another member's Commit won; dropped */
  GH_MLS_COMMIT_CLEARED     /* every relay refused it (or it became invalid); dropped */
} GhMlsCommitState;

/* Stages the Commit @producer makes (T-mls, above). @account_pubkey is the
 * store's account (64 lowercase hex). The group needs relays. NULL on error,
 * with nothing stored. */
GhMlsCommitPublish *gh_mls_commit_stage(GhStore *store, Marmot *marmot,
                                        MarmotStorage *storage,
                                        const MarmotGroupId *group_id,
                                        const gchar *account_pubkey,
                                        GhMlsCommitProducer producer,
                                        gpointer user_data, GError **error);

/* Records @relay_url's NIP-01 OK for @publish (@accepted: OK true), together
 * with its effect (above), in one transaction; *out_state (nullable) is where
 * the Commit stands afterwards. On error nothing of it is recorded. */
gboolean gh_mls_commit_record_answer(GhStore *store, Marmot *marmot,
                                     MarmotStorage *storage,
                                     const GhMlsCommitPublish *publish,
                                     const gchar *relay_url, gboolean accepted,
                                     const gchar *relay_message,
                                     GhMlsCommitState *out_state, GError **error);

/* After a start: the pending Commits to publish (GhMlsCommitPublish), per
 * group (above). NULL on error. */
GPtrArray *gh_mls_commit_resume(GhStore *store, Marmot *marmot, MarmotStorage *storage,
                                const gchar *account_pubkey, GError **error);

G_END_DECLS
#endif
