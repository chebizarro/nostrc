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
 */

#ifndef GNOSTR_NIP75_GOALS_H
#define GNOSTR_NIP75_GOALS_H

#include <glib.h>

G_BEGIN_DECLS

#define NIP75_KIND_ZAP_GOAL 9041

/**
 * Parsed zap goal event data
 */
typedef struct {
  gchar *event_id;           /* Goal event ID (hex) */
  gchar *pubkey;             /* Goal creator pubkey (hex) */
  gchar *description;        /* Goal description (from content) */
  gint64 target_msat;        /* Target amount in millisatoshis */
  gint64 closed_at;          /* Deadline timestamp (0 = no deadline) */
  gint64 created_at;         /* Event creation timestamp */
  gchar **relays;            /* Relays for zap receipts (NULL-terminated) */
  gchar *linked_event_id;    /* Referenced event ID (optional) */
  gchar *linked_pubkey;      /* Referenced profile pubkey (optional) */
  gchar *external_url;       /* External reference URL (optional) */
} GnostrNip75Goal;

/**
 * Zap goal progress tracking
 */
typedef struct {
  gint64 total_received_msat; /* Total zaps received */
  guint zap_count;            /* Number of zaps */
  gdouble progress_percent;   /* 0.0 - 100.0+ */
  gboolean is_complete;       /* target reached */
  gboolean is_expired;        /* deadline passed */
} GnostrNip75GoalProgress;

/**
 * Callback for goal progress calculation
 */
typedef void (*GnostrNip75GoalProgressCallback)(
    const GnostrNip75Goal *goal,
    const GnostrNip75GoalProgress *progress,
    GError *error,
    gpointer user_data);

/**
 * gnostr_nip75_is_goal_kind:
 * @kind: Event kind
 *
 * Check if an event kind is a zap goal (kind 9041).
 *
 * Returns: TRUE if kind is 9041
 */
gboolean gnostr_nip75_is_goal_kind(gint kind);

/**
 * gnostr_nip75_goal_parse:
 * @json_str: JSON string of the event
 *
 * Parse a zap goal event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed goal or NULL on error.
 *          Free with gnostr_nip75_goal_free().
 */
GnostrNip75Goal *gnostr_nip75_goal_parse(const gchar *json_str);

/**
 * gnostr_nip75_goal_free:
 * @goal: Goal to free
 *
 * Free a parsed zap goal.
 */
void gnostr_nip75_goal_free(GnostrNip75Goal *goal);

/**
 * gnostr_nip75_goal_progress_new:
 *
 * Create a new goal progress structure.
 *
 * Returns: (transfer full): New progress structure. Free with g_free().
 */
GnostrNip75GoalProgress *gnostr_nip75_goal_progress_new(void);

/**
 * gnostr_nip75_goal_is_expired:
 * @goal: Zap goal
 *
 * Check if the goal has passed its deadline.
 *
 * Returns: TRUE if deadline has passed, FALSE otherwise
 */
gboolean gnostr_nip75_goal_is_expired(const GnostrNip75Goal *goal);

/**
 * gnostr_nip75_goal_has_deadline:
 * @goal: Zap goal
 *
 * Check if the goal has a deadline.
 *
 * Returns: TRUE if goal has a closed_at timestamp
 */
gboolean gnostr_nip75_goal_has_deadline(const GnostrNip75Goal *goal);

/**
 * gnostr_nip75_calculate_progress:
 * @goal: Zap goal
 * @zap_receipts_json: Array of kind:9735 zap receipt JSON strings
 * @num_receipts: Number of receipts
 *
 * Calculate progress from zap receipts.
 *
 * Returns: (transfer full): Progress info. Free with g_free().
 */
GnostrNip75GoalProgress *gnostr_nip75_calculate_progress(
    const GnostrNip75Goal *goal,
    const gchar **zap_receipts_json,
    gsize num_receipts);

/**
 * gnostr_nip75_build_goal_event:
 * @description: Goal description (content)
 * @target_msat: Target amount in millisatoshis
 * @relays: NULL-terminated array of relay URLs (optional)
 * @closed_at: Deadline timestamp, or 0 for no deadline
 * @linked_event_id: Event ID this goal is for (optional)
 * @linked_pubkey: Pubkey this goal is for (optional)
 * @external_url: External reference URL (optional)
 *
 * Build an unsigned kind:9041 zap goal event JSON.
 * The event must be signed before publishing.
 *
 * Returns: (transfer full) (nullable): JSON string of the unsigned event
 */
gchar *gnostr_nip75_build_goal_event(const gchar *description,
                                      gint64 target_msat,
                                      const gchar * const *relays,
                                      gint64 closed_at,
                                      const gchar *linked_event_id,
                                      const gchar *linked_pubkey,
                                      const gchar *external_url);

/**
 * gnostr_nip75_format_target:
 * @target_msat: Target amount in millisatoshis
 *
 * Format the target amount for display (e.g., "100K sats", "1M sats").
 *
 * Returns: (transfer full): Formatted string
 */
gchar *gnostr_nip75_format_target(gint64 target_msat);

/**
 * gnostr_nip75_format_progress:
 * @received_msat: Received amount in millisatoshis
 * @target_msat: Target amount in millisatoshis
 *
 * Format progress for display (e.g., "50K / 100K sats").
 *
 * Returns: (transfer full): Formatted string
 */
gchar *gnostr_nip75_format_progress(gint64 received_msat, gint64 target_msat);

/**
 * gnostr_nip75_format_time_remaining:
 * @closed_at: Deadline timestamp
 *
 * Format time remaining until deadline (e.g., "3 days", "2 hours").
 *
 * Returns: (transfer full) (nullable): Formatted string or NULL if no deadline
 */
gchar *gnostr_nip75_format_time_remaining(gint64 closed_at);

G_END_DECLS

#endif /* GNOSTR_NIP75_GOALS_H */
