#ifndef GH_DELIVERY_INDICATOR_H
#define GH_DELIVERY_INDICATOR_H

#include <adwaita.h>
#include "gh-message.h"

G_BEGIN_DECLS

/* What the relays reported for one own message, for its delivery details
 * (charter §3.6: "details on demand"). A relay accepting a message only means
 * that relay stored it. Filled by the owner of the outbox, e.g. from
 * gh_outbox_item_dup_targets(). */
typedef struct {
  gchar *recipient;    /* lowercase hex pubkey of the wrap's receiver; NULL
                        * for the self-copy (your other devices) */
  gchar *relay_url;    /* NULL: the recipient has no message relay to send to
                        * (W17: nothing went out for them; outcome says why) */
  gboolean accepted;   /* the relay answered OK true (or "duplicate:") */
  gchar *outcome;      /* one plain-language sentence, e.g.
                        * gh_message_status_describe_target() */
} GhDeliveryTarget;

typedef struct {
  GPtrArray *targets;         /* GhDeliveryTarget, recipients' first */
  gchar *detail;              /* the status in one or two plain sentences, e.g.
                               * GhOutboxItem:detail; NULL: the status's
                               * accessible description is shown */
  gint64 next_attempt_at;     /* unix seconds of the next automatic try; 0 none */
  gboolean self_copy_missing; /* show gh_message_status_get_self_copy_note() */
} GhDeliveryReport;

GhDeliveryReport *gh_delivery_report_new(void);
/* Appends a target (copies the strings). recipient NULL: the self-copy.
 * relay_url NULL (a recipient only): that recipient has no message relay,
 * shown as one row saying so with outcome as its explanation. */
void gh_delivery_report_add(GhDeliveryReport *report, const gchar *recipient,
                            const gchar *relay_url, gboolean accepted, const gchar *outcome);
void gh_delivery_report_free(GhDeliveryReport *report);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhDeliveryReport, gh_delivery_report_free)

/* The delivery indicator of an own message (data/ui/gh-delivery-details.blp,
 * charter §7.4, §7.6): hidden for incoming messages and
 * GH_MESSAGE_STATUS_NONE; otherwise a flat button with the status icon and,
 * unless the message is simply "Sent", its short label (gh-message-status.h).
 * It opens the details: the status, its full description, the relays of
 * each recipient with what each reported, the next automatic try and the
 * self-copy note, from gh_conversation_view_dup_delivery_report() of the
 * enclosing view, and always the reminder that acceptance is not delivery.
 * Properties: "message" (GhMessage) and "status" (read-only, notifies). */
#define GH_TYPE_DELIVERY_INDICATOR (gh_delivery_indicator_get_type())
G_DECLARE_FINAL_TYPE(GhDeliveryIndicator, gh_delivery_indicator, GH, DELIVERY_INDICATOR,
                     GtkWidget)

void gh_delivery_indicator_set_message(GhDeliveryIndicator *self, GhMessage *message);
GhMessage *gh_delivery_indicator_get_message(GhDeliveryIndicator *self);
GhMessageStatus gh_delivery_indicator_get_status(GhDeliveryIndicator *self);
/* Opens (or closes) the details popover. */
void gh_delivery_indicator_show_details(GhDeliveryIndicator *self);
void gh_delivery_indicator_hide_details(GhDeliveryIndicator *self);

/* The status text shown next to the icon: NULL for NONE and SENT (icon
 * only), else the short label. */
const gchar *gh_delivery_indicator_status_text(GhMessageStatus status);

G_END_DECLS
#endif
