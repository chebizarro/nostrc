#ifndef GH_REACTION_H
#define GH_REACTION_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* NIP-25 reactions across all three conversation backends.
 *
 * A reaction is a kind 7 event targeting a message by its rumor/event id
 * (the e-tag). In MLS groups the inner unsigned rumor is kind 7 with an
 * e-tag and the emoji as content, exactly as MDK v0.11 marmot-app sends.
 * In NIP-17 DMs it is a kind 7 rumor in a gift wrap. In NIP-29 groups it
 * is a kind 7 event with an h-tag.
 *
 * Removal uses each protocol's deletion mechanism: kind 5 in MLS groups
 * (with e-tags pointing to the reaction event ids), kind 5 in NIP-17 (a
 * kind 5 rumor gift-wrapped), and kind 5 with h-tag in NIP-29.
 *
 * Custom emoji images are not fetched (privacy charter PD-2: no remote
 * images by default); the shortcode text is shown instead. */

/* Maximum emoji content length (bytes). MDK v0.11 caps at 64. */
#define GH_REACTION_MAX_EMOJI 64

/* The default quick-reaction set: the picker shows these before the GTK
 * emoji chooser (GNOME HIG, non-technical users). */
#define GH_REACTION_QUICK_SET_SIZE 6
extern const gchar *const gh_reaction_quick_set[GH_REACTION_QUICK_SET_SIZE];

/* ---- GhReaction: one reaction on one message -------------------------------- */

#define GH_TYPE_REACTION (gh_reaction_get_type())
G_DECLARE_FINAL_TYPE(GhReaction, gh_reaction, GH, REACTION, GObject)

/* Creates a reaction. All strings are copied. emoji must be non-empty and
 * at most GH_REACTION_MAX_EMOJI bytes; NULL defaults to "+". */
GhReaction *gh_reaction_new(const gchar *target_rumor_id,
                            const gchar *reaction_rumor_id,
                            const gchar *sender_pubkey,
                            const gchar *emoji,
                            gint64 created_at,
                            const gchar *room_id);

const gchar *gh_reaction_get_target_rumor_id(GhReaction *self);
const gchar *gh_reaction_get_reaction_rumor_id(GhReaction *self);
const gchar *gh_reaction_get_sender(GhReaction *self);
const gchar *gh_reaction_get_emoji(GhReaction *self);
gint64 gh_reaction_get_created_at(GhReaction *self);
const gchar *gh_reaction_get_room_id(GhReaction *self);

/* ---- GhReactionChip: one emoji's aggregation for one message ---------------- */

/* An aggregated emoji chip: the emoji, its count, whether the user reacted
 * with it, and the reactors' pubkeys (for tooltips). Not a GObject: a plain
 * struct in a GPtrArray owned by the reaction store. */
typedef struct {
  gchar *emoji;
  guint count;
  gboolean is_own;
  GPtrArray *reactors;   /* (element-type utf8) sender pubkeys */
} GhReactionChip;

void gh_reaction_chip_free(GhReactionChip *chip);

/* ---- GhReactionSummary: all reactions for one message ----------------------- */

/* Readable summary of reactions on one message, updated live by the
 * reaction store. Properties: "total-count" (uint), "has-own" (boolean).
 * The chips are accessible through gh_reaction_summary_get_chips(). */
#define GH_TYPE_REACTION_SUMMARY (gh_reaction_summary_get_type())
G_DECLARE_FINAL_TYPE(GhReactionSummary, gh_reaction_summary, GH, REACTION_SUMMARY, GObject)

guint gh_reaction_summary_get_total_count(GhReactionSummary *self);
gboolean gh_reaction_summary_get_has_own(GhReactionSummary *self);
/* Borrowed; the array and its GhReactionChip elements are valid until the
 * next change. NULL when empty. */
const GPtrArray *gh_reaction_summary_get_chips(GhReactionSummary *self);
/* The reaction_rumor_id of the user's own reaction with @emoji, or NULL. */
const gchar *gh_reaction_summary_own_reaction_id(GhReactionSummary *self,
                                                 const gchar *emoji);

G_END_DECLS
#endif
