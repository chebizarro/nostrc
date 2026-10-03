#ifndef GH_MLS_POLL_H
#define GH_MLS_POLL_H

#include <glib-object.h>

G_BEGIN_DECLS

/* NIP-88 inner-event kinds carried inside MLS app messages (MDK v0.11,
 * crates/traits/src/polls.rs). */
#define GH_MLS_POLL_KIND      1068
#define GH_MLS_POLL_VOTE_KIND 1018

/* Tag names. */
#define GH_MLS_POLL_OPTION_TAG   "option"
#define GH_MLS_POLL_TYPE_TAG     "polltype"
#define GH_MLS_POLL_ENDS_AT_TAG  "endsAt"
#define GH_MLS_POLL_RESPONSE_TAG "response"
#define GH_MLS_POLL_EVENT_REF_TAG "e"

/* Bounds matching MDK. */
#define GH_MLS_POLL_MIN_OPTIONS       2
#define GH_MLS_POLL_MAX_OPTIONS      10
#define GH_MLS_POLL_MAX_QUESTION   1024
#define GH_MLS_POLL_MAX_OPTION_LABEL 256
#define GH_MLS_POLL_MAX_OPTION_ID     64
#define GH_MLS_POLL_MAX_TAGS         32
#define GH_MLS_POLL_MAX_LIFETIME  (30 * 24 * 60 * 60)

typedef enum {
  GH_MLS_POLL_SINGLE_CHOICE,
  GH_MLS_POLL_MULTIPLE_CHOICE,
} GhMlsPollType;

/* One option of a poll definition. */
typedef struct {
  gchar *id;
  gchar *label;
} GhMlsPollOption;

void gh_mls_poll_option_free(GhMlsPollOption *opt);

/* Parsed poll definition (kind 1068). */
typedef struct {
  gchar        *question;
  GPtrArray    *options;     /* GhMlsPollOption */
  GhMlsPollType poll_type;
  gint64        ends_at;     /* 0 = no deadline */
} GhMlsPollDef;

void gh_mls_poll_def_free(GhMlsPollDef *def);

/* One option's tally in a live poll projection. */
typedef struct {
  gchar  *id;
  gchar  *label;
  guint   votes;
} GhMlsPollTally;

void gh_mls_poll_tally_free(GhMlsPollTally *tally);

/* A voter's selection for one poll. */
typedef struct {
  gchar  *voter_pubkey;
  gchar **option_ids;   /* NULL-terminated */
  gchar  *vote_event_id;  /* inner event id of the vote (for withdrawal) */
} GhMlsPollVoterRecord;

void gh_mls_poll_voter_record_free(GhMlsPollVoterRecord *rec);

/* ---- GhMlsPoll: the live projection of one poll in one group ----------- */

#define GH_TYPE_MLS_POLL (gh_mls_poll_get_type())
G_DECLARE_FINAL_TYPE(GhMlsPoll, gh_mls_poll, GH, MLS_POLL, GObject)

/* Parse a kind-1068 inner event into a poll; NULL on malformed (error set).
 * event_id is the inner event id; sender is the authenticated MLS sender. */
GhMlsPoll *gh_mls_poll_new_from_event(const gchar *event_id,
                                      const gchar *sender,
                                      gint64       created_at,
                                      const gchar *inner_event_json,
                                      GError     **error);

const gchar      *gh_mls_poll_get_event_id(GhMlsPoll *self);
const gchar      *gh_mls_poll_get_creator(GhMlsPoll *self);
const gchar      *gh_mls_poll_get_question(GhMlsPoll *self);
GhMlsPollType     gh_mls_poll_get_poll_type(GhMlsPoll *self);
gint64            gh_mls_poll_get_ends_at(GhMlsPoll *self);
gint64            gh_mls_poll_get_created_at(GhMlsPoll *self);
guint             gh_mls_poll_get_n_options(GhMlsPoll *self);
const GhMlsPollTally *gh_mls_poll_get_option(GhMlsPoll *self, guint index);
guint             gh_mls_poll_get_total_voters(GhMlsPoll *self);

/* Whether the poll is still open (not past its deadline, if it has one). */
gboolean gh_mls_poll_is_open(GhMlsPoll *self, gint64 now_s);

/* The account's current selection (NULL-terminated option ids; empty array
 * when no vote cast). Borrowed. */
const gchar *const *gh_mls_poll_get_local_selection(GhMlsPoll *self);
gboolean            gh_mls_poll_has_voted(GhMlsPoll *self);

/* Apply a vote (kind 1018): voter_pubkey voted for option_ids (validated
 * against the poll definition). Returns TRUE if tallies changed.
 * Replaces any earlier vote by the same voter. Notifies "tallies-changed". */
gboolean gh_mls_poll_apply_vote(GhMlsPoll     *self,
                                const gchar   *voter_pubkey,
                                const gchar  **option_ids,
                                gint64         vote_created_at,
                                const gchar   *vote_event_id);

/* Remove a voter's record by pubkey.  Returns TRUE and emits
 * "tallies-changed" if the voter was present. */
gboolean gh_mls_poll_remove_voter(GhMlsPoll *self, const gchar *voter_pubkey);

/* Remove the voter whose vote has this inner event id.  Returns TRUE and
 * emits "tallies-changed" if found.  Used by the withdrawal path, which
 * only knows the rumor id. */
gboolean gh_mls_poll_remove_voter_by_event_id(GhMlsPoll *self, const gchar *event_id);

/* Mark a pubkey as the local account (its votes are the local selection). */
void gh_mls_poll_set_local_account(GhMlsPoll *self, const gchar *account_pubkey);

/* ---- inner-event builders ---------------------------------------------- */

/* Build the inner event JSON for a new kind-1068 poll.
 * Returns the JSON (caller frees) or NULL on validation error. */
gchar *gh_mls_poll_build_event(const gchar   *account_pubkey,
                               const gchar   *nostr_group_hex,
                               gint64         created_at,
                               const gchar   *question,
                               const gchar  **option_labels,
                               guint          n_options,
                               GhMlsPollType  poll_type,
                               gint64         ends_at,
                               GError       **error);

/* Build the inner event JSON for a kind-1018 poll response.
 * Returns the JSON (caller frees) or NULL on validation error. */
gchar *gh_mls_poll_build_vote_event(const gchar   *account_pubkey,
                                    const gchar   *nostr_group_hex,
                                    gint64         created_at,
                                    const gchar   *poll_event_id,
                                    const gchar  **option_ids,
                                    guint          n_options,
                                    GError       **error);

/* Parse a kind-1018 vote's tags to extract the target poll id and
 * selected option ids. Returns TRUE on success. option_ids is set to a
 * NULL-terminated array (caller frees with g_strfreev). */
gboolean gh_mls_poll_parse_vote(const gchar  *inner_event_json,
                                gchar       **out_target_poll_id,
                                gchar      ***out_option_ids,
                                gchar       **out_event_id,
                                GError      **error);

G_END_DECLS
#endif
