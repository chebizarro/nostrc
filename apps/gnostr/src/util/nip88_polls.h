#ifndef GNOSTR_NIP88_POLLS_H
#define GNOSTR_NIP88_POLLS_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * NIP-88: Poll Events Utility Functions
 *
 * Poll Event (kind 1018):
 * - content: poll question
 * - tags: ["poll_option", "0", "Option text"], ["poll_option", "1", "..."], ...
 *         ["closed_at", "unix_timestamp"] (optional)
 *         ["value_maximum", "1"] for single choice, omit or >1 for multiple
 *
 * Poll Response (kind 1019):
 * - content: "" (empty or optional comment)
 * - tags: ["e", "<poll_event_id>", "", "root"]
 *         ["response", "0"], ["response", "2"] (selected indices)
 *         ["p", "<poll_author_pubkey>"]
 */

#define NIP88_KIND_POLL      1018
#define NIP88_KIND_RESPONSE  1019

/**
 * Parsed poll option
 */
typedef struct {
  int index;
  char *text;
} GnostrNip88PollOption;

/**
 * Parsed poll event data
 */
typedef struct {
  char *event_id;           /* Poll event ID (hex) */
  char *pubkey;             /* Poll author pubkey (hex) */
  char *question;           /* Poll question (from content) */
  GPtrArray *options;       /* Array of GnostrNip88PollOption* */
  gint64 closed_at;         /* Closing timestamp (0 = no limit) */
  int value_maximum;        /* Max selections (1 = single choice) */
  gint64 created_at;        /* Event creation timestamp */
} GnostrNip88Poll;

/**
 * Parsed poll response data
 */
typedef struct {
  char *event_id;           /* Response event ID (hex) */
  char *poll_id;            /* Referenced poll event ID (hex) */
  char *responder_pubkey;   /* Responder's pubkey (hex) */
  GArray *selected_indices; /* Array of int (selected option indices) */
  gint64 created_at;        /* Response creation timestamp */
} GnostrNip88Response;

/**
 * Parse a poll event from JSON
 * @param json_str: JSON string of the event
 * @return: Parsed poll or NULL on error. Free with gnostr_nip88_poll_free().
 */
GnostrNip88Poll *gnostr_nip88_poll_parse(const char *json_str);

/**
 * Parse a poll response from JSON
 * @param json_str: JSON string of the event
 * @return: Parsed response or NULL on error. Free with gnostr_nip88_response_free().
 */
GnostrNip88Response *gnostr_nip88_response_parse(const char *json_str);

/**
 * Check if an event is a poll (kind 1068)
 * @param kind: Event kind
 */
gboolean gnostr_nip88_is_poll_kind(int kind);

/**
 * Check if an event is a poll response (kind 1018)
 * @param kind: Event kind
 */
gboolean gnostr_nip88_is_response_kind(int kind);

/**
 * Check if a poll is currently open for voting
 * @param poll: Parsed poll data
 */
gboolean gnostr_nip88_poll_is_open(GnostrNip88Poll *poll);

/**
 * Check if a poll allows multiple selections
 * @param poll: Parsed poll data
 */
gboolean gnostr_nip88_poll_is_multiple_choice(GnostrNip88Poll *poll);

/**
 * Build tags array for a new poll event
 * @param options: GPtrArray of option text strings
 * @param closed_at: Closing timestamp (0 = no limit)
 * @param multiple_choice: TRUE to allow multiple selections
 * @return: JSON array string of tags. Caller must free.
 */
char *gnostr_nip88_build_poll_tags(GPtrArray *options,
                                    gint64 closed_at,
                                    gboolean multiple_choice);

/**
 * Build tags array for a poll response event
 * @param poll_id: Poll event ID (hex)
 * @param poll_pubkey: Poll author pubkey (hex)
 * @param selected_indices: Array of selected option indices
 * @param num_indices: Number of selected indices
 * @return: JSON array string of tags. Caller must free.
 */
char *gnostr_nip88_build_response_tags(const char *poll_id,
                                        const char *poll_pubkey,
                                        int *selected_indices,
                                        gsize num_indices);

/**
 * Free a parsed poll
 */
void gnostr_nip88_poll_free(GnostrNip88Poll *poll);

/**
 * Free a parsed response
 */
void gnostr_nip88_response_free(GnostrNip88Response *response);

/**
 * Vote tally result structure
 */
typedef struct {
  guint *vote_counts;     /* Array of vote counts indexed by option index */
  gsize num_options;      /* Number of options */
  guint total_voters;     /* Total unique voters */
  GHashTable *voter_map;  /* Map pubkey -> first response (for dedup) */
} GnostrNip88VoteTally;

/**
 * Tally votes from an array of poll responses
 * @param responses: GPtrArray of GnostrNip88Response* (all for same poll)
 * @param num_options: Number of poll options
 * @return: Vote tally. Free with gnostr_nip88_vote_tally_free().
 */
GnostrNip88VoteTally *gnostr_nip88_tally_votes(GPtrArray *responses,
                                                 gsize num_options);

/**
 * Free a vote tally
 */
void gnostr_nip88_vote_tally_free(GnostrNip88VoteTally *tally);

/**
 * Check if a pubkey has voted in a tally
 * @param tally: Vote tally
 * @param pubkey_hex: Voter's public key (hex)
 * @return: TRUE if pubkey has voted
 */
gboolean gnostr_nip88_has_voted(GnostrNip88VoteTally *tally,
                                 const char *pubkey_hex);

/**
 * Get the indices a pubkey voted for
 * @param tally: Vote tally
 * @param pubkey_hex: Voter's public key (hex)
 * @return: GArray of ints with voted indices, or NULL. Caller must free.
 */
GArray *gnostr_nip88_get_voter_choices(GnostrNip88VoteTally *tally,
                                        const char *pubkey_hex);

G_END_DECLS

#endif /* GNOSTR_NIP88_POLLS_H */
