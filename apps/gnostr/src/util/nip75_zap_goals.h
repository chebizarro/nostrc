/**
 * NIP-75: Zap Goals Utility
 *
 * Zap Goal events (kind 9041) for crowdfunding/fundraising targets.
 * Goals have an amount target and track progress via zap receipts.
 *
 * Event Structure:
 * - kind: 9041
 * - content: goal description
 * - tags:
 *   - ["amount", "<target_millisats>"] - required
 *   - ["relays", "<relay1>", "<relay2>", ...] - relays for zap receipts
 *   - ["closed_at", "<unix_timestamp>"] - optional deadline
 *   - ["e", "<event_id>"] - optional: event the goal is for
 *   - ["p", "<pubkey>"] - optional: profile the goal is for (zapathon)
 *   - ["r", "<url>"] - optional: external reference
 *
 * Progress is tracked via kind 9735 zap receipts that include an "e" tag
 * referencing the goal event.
 */

#ifndef GNOSTR_NIP75_ZAP_GOALS_H
#define GNOSTR_NIP75_ZAP_GOALS_H

#include <glib.h>

G_BEGIN_DECLS

#define NIP75_KIND_ZAP_GOAL 9041

/**
 * GnostrZapGoal:
 * @title: Goal title/description from event content
 * @target_msats: Target funding amount in millisatoshis
 * @current_msats: Current amount received (from zap receipts)
 * @end_time: Optional deadline timestamp (0 = no deadline)
 * @event_id: Goal event ID (hex, 64 chars)
 * @pubkey: Goal creator's pubkey (hex, 64 chars)
 * @lud16: Creator's lightning address for zapping
 * @relays: NULL-terminated array of relay URLs for zap receipts
 * @created_at: Event creation timestamp
 * @linked_event_id: Referenced event ID (optional)
 * @linked_pubkey: Referenced profile pubkey (optional)
 * @external_url: External reference URL (optional)
 *
 * Parsed zap goal event data structure.
 */
typedef struct {
  gchar *title;              /* Goal description/title (from content) */
  gint64 target_msats;       /* Target amount in millisatoshis */
  gint64 current_msats;      /* Current amount received */
  gint64 end_time;           /* Deadline timestamp (0 = no deadline) */
  gchar *event_id;           /* Goal event ID (hex) */
  gchar *pubkey;             /* Goal creator pubkey (hex) */
  gchar *lud16;              /* Creator's lightning address */
  gchar **relays;            /* Relays for zap receipts (NULL-terminated) */
  gint64 created_at;         /* Event creation timestamp */
  gchar *linked_event_id;    /* Referenced event ID (optional) */
  gchar *linked_pubkey;      /* Referenced profile pubkey (optional) */
  gchar *external_url;       /* External reference URL (optional) */
} GnostrZapGoal;

/**
 * GnostrZapGoalProgress:
 * @total_received_msats: Total amount received from zaps
 * @zap_count: Number of individual zaps
 * @progress_percent: Progress as percentage (0.0 - 100.0+)
 * @is_complete: Whether target amount has been reached
 * @is_expired: Whether deadline has passed
 *
 * Calculated progress data for a zap goal.
 */
typedef struct {
  gint64 total_received_msats; /* Total zaps received in millisatoshis */
  guint zap_count;             /* Number of zaps */
  gdouble progress_percent;    /* 0.0 - 100.0+ percentage */
  gboolean is_complete;        /* Target reached */
  gboolean is_expired;         /* Deadline passed */
} GnostrZapGoalProgress;

/**
 * GnostrZapGoalProgressCallback:
 * @goal: The zap goal
 * @progress: Calculated progress data
 * @error: Error if any, or NULL
 * @user_data: User data passed to the function
 *
 * Callback type for async progress calculation.
 */
typedef void (*GnostrZapGoalProgressCallback)(
    const GnostrZapGoal *goal,
    const GnostrZapGoalProgress *progress,
    GError *error,
    gpointer user_data);

/* ============== Kind Check ============== */

/**
 * gnostr_nip75_is_zap_goal_kind:
 * @kind: Event kind number
 *
 * Check if an event kind is a zap goal (kind 9041).
 *
 * Returns: TRUE if kind is 9041
 */
gboolean gnostr_nip75_is_zap_goal_kind(gint kind);

/* ============== Parsing ============== */

/**
 * gnostr_zap_goal_parse:
 * @json_str: JSON string of the kind:9041 event
 *
 * Parse a zap goal event from its JSON representation.
 * Validates that the event is kind 9041 and has required tags.
 *
 * Returns: (transfer full) (nullable): Parsed goal or NULL on error.
 *          Free with gnostr_zap_goal_free().
 */
GnostrZapGoal *gnostr_zap_goal_parse(const gchar *json_str);

/**
 * gnostr_zap_goal_free:
 * @goal: Goal to free
 *
 * Free a parsed zap goal and all its allocated memory.
 */
void gnostr_zap_goal_free(GnostrZapGoal *goal);

/* ============== Progress Calculation ============== */

/**
 * gnostr_zap_goal_progress_new:
 *
 * Create a new zeroed progress structure.
 *
 * Returns: (transfer full): New progress structure. Free with g_free().
 */
GnostrZapGoalProgress *gnostr_zap_goal_progress_new(void);

/**
 * gnostr_zap_goal_calculate_progress:
 * @goal: The zap goal to calculate progress for
 * @zap_receipts_json: Array of kind:9735 zap receipt JSON strings
 * @num_receipts: Number of receipts in the array
 *
 * Calculate progress for a zap goal from its associated zap receipts.
 * Only receipts that include an "e" tag referencing this goal are counted.
 *
 * Returns: (transfer full): Progress data. Free with g_free().
 */
GnostrZapGoalProgress *gnostr_zap_goal_calculate_progress(
    const GnostrZapGoal *goal,
    const gchar **zap_receipts_json,
    gsize num_receipts);

/**
 * gnostr_zap_goal_update_current:
 * @goal: The zap goal to update
 * @progress: Progress data to apply
 *
 * Update a goal's current_msats from calculated progress.
 */
void gnostr_zap_goal_update_current(GnostrZapGoal *goal,
                                     const GnostrZapGoalProgress *progress);

/* ============== Goal Creation ============== */

/**
 * gnostr_zap_goal_create_event:
 * @title: Goal title/description (goes in content)
 * @target_msats: Target amount in millisatoshis
 * @relays: NULL-terminated array of relay URLs (optional, can be NULL)
 * @closed_at: Deadline timestamp, or 0 for no deadline
 * @linked_event_id: Event ID this goal is for (optional, can be NULL)
 * @linked_pubkey: Pubkey this goal is for (optional, can be NULL)
 * @external_url: External reference URL (optional, can be NULL)
 *
 * Build an unsigned kind:9041 zap goal event JSON.
 * The event must be signed before publishing.
 *
 * Returns: (transfer full) (nullable): JSON string of the unsigned event,
 *          or NULL on error
 */
gchar *gnostr_zap_goal_create_event(const gchar *title,
                                     gint64 target_msats,
                                     const gchar * const *relays,
                                     gint64 closed_at,
                                     const gchar *linked_event_id,
                                     const gchar *linked_pubkey,
                                     const gchar *external_url);

/* ============== Status Checks ============== */

/**
 * gnostr_zap_goal_is_expired:
 * @goal: Zap goal to check
 *
 * Check if the goal has passed its deadline.
 *
 * Returns: TRUE if deadline has passed, FALSE otherwise
 */
gboolean gnostr_zap_goal_is_expired(const GnostrZapGoal *goal);

/**
 * gnostr_zap_goal_has_deadline:
 * @goal: Zap goal to check
 *
 * Check if the goal has a deadline set.
 *
 * Returns: TRUE if goal has a closed_at timestamp > 0
 */
gboolean gnostr_zap_goal_has_deadline(const GnostrZapGoal *goal);

/**
 * gnostr_zap_goal_is_complete:
 * @goal: Zap goal to check
 *
 * Check if the goal has reached its target.
 *
 * Returns: TRUE if current_msats >= target_msats
 */
gboolean gnostr_zap_goal_is_complete(const GnostrZapGoal *goal);

/* ============== Formatting ============== */

/**
 * gnostr_zap_goal_format_target:
 * @target_msats: Target amount in millisatoshis
 *
 * Format the target amount for display (e.g., "100K sats", "1M sats", "0.5 BTC").
 *
 * Returns: (transfer full): Formatted string
 */
gchar *gnostr_zap_goal_format_target(gint64 target_msats);

/**
 * gnostr_zap_goal_format_progress:
 * @current_msats: Current amount in millisatoshis
 * @target_msats: Target amount in millisatoshis
 *
 * Format progress for display (e.g., "50K / 100K sats").
 *
 * Returns: (transfer full): Formatted string
 */
gchar *gnostr_zap_goal_format_progress(gint64 current_msats, gint64 target_msats);

/**
 * gnostr_zap_goal_format_time_remaining:
 * @end_time: Deadline timestamp
 *
 * Format time remaining until deadline (e.g., "3 days", "2 hours", "Ended").
 *
 * Returns: (transfer full) (nullable): Formatted string or NULL if no deadline
 */
gchar *gnostr_zap_goal_format_time_remaining(gint64 end_time);

/**
 * gnostr_zap_goal_get_progress_percent:
 * @goal: Zap goal
 *
 * Get the current progress percentage.
 *
 * Returns: Progress as 0.0-100.0+ percentage
 */
gdouble gnostr_zap_goal_get_progress_percent(const GnostrZapGoal *goal);

G_END_DECLS

#endif /* GNOSTR_NIP75_ZAP_GOALS_H */
