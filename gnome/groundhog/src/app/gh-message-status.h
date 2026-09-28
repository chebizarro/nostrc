#ifndef GH_MESSAGE_STATUS_H
#define GH_MESSAGE_STATUS_H

#include <glib.h>

G_BEGIN_DECLS

/* The honest, user-facing state of one outgoing message (Groundhog privacy/UX
 * charter §3.6), GTK-free. A relay OK is acceptance by that relay, never
 * delivery: there is deliberately no DELIVERED or READ value. A self-copy
 * failure never changes this status (it is a secondary "Not saved to your
 * other devices" note). */
typedef enum {
  GH_MESSAGE_STATUS_WAITING_FOR_SIGNER,   /* "Waiting for approval" */
  GH_MESSAGE_STATUS_QUEUED_OFFLINE,       /* "Waiting for connection" */
  GH_MESSAGE_STATUS_SENDING,              /* "Sending…": no recipient reached yet */
  GH_MESSAGE_STATUS_SENT,                 /* every recipient: >= 1 inbox relay accepted */
  GH_MESSAGE_STATUS_PARTIALLY_SENT,       /* some recipients reached, others not */
  GH_MESSAGE_STATUS_RETRYING,             /* none reached, transient failures (outbox) */
  GH_MESSAGE_STATUS_NOT_SENT,             /* all terminal, or needs attention */
  GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX, /* a recipient has no kind-10050 */
  GH_MESSAGE_STATUS_CANCELLED
} GhMessageStatus;

G_END_DECLS
#endif
