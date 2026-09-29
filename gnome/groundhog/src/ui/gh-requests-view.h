#ifndef GH_REQUESTS_VIEW_H
#define GH_REQUESTS_VIEW_H

#include <adwaita.h>

#include "gh-conversation.h"

G_BEGIN_DECLS

/*
 * GhRequestsView (privacy charter §7.9, PD-8, PT-8, §8.2 G18): the content
 * page for one message request (data/ui/gh-requests-view.blp). It shows the
 * sender's npub (never a fetched profile), the message count and the first
 * message as plain text (no markup, no links, nothing from the web), and
 * offers:
 *   - Accept: gh_conversation_accept() (persisted by the store's delegate);
 *     the contact directory then looks up the sender's name and inbox (G10).
 *     "accepted" (GhConversation) is emitted so the window can open it.
 *   - Delete…: after an AdwAlertDialog, the backend's forget() (charter §3.8:
 *     messages removed, a tombstone refuses older backfill).
 *   - Block…: after an AdwAlertDialog, the backend's block() (local block of
 *     this conversation: later messages in the room are never shown or
 *     notified; New Message to the same people unblocks it, G18).
 * The two confirmations are template objects of the Blueprint
 * (delete_dialog, block_dialog; responses "delete-confirm"/"delete-cancel"
 * and "block-confirm"/"block-cancel").
 * Nothing is published and the sender is never told. Without a backend
 * (no private storage) Delete and Block are unavailable and say why. The
 * outcome is acknowledged with a toast when the view is inside an
 * AdwToastOverlay, and announced.
 */

typedef struct {
  gboolean (*forget)(gpointer data, GhConversation *request, GError **error);
  gboolean (*block)(gpointer data, GhConversation *request, GError **error);
} GhRequestsBackend;

#define GH_TYPE_REQUESTS_VIEW (gh_requests_view_get_type())
G_DECLARE_FINAL_TYPE(GhRequestsView, gh_requests_view, GH, REQUESTS_VIEW, AdwBin)

GtkWidget *gh_requests_view_new(void);

/* The request shown (NULL: the "Message Requests" explanation). A room that
 * is not a request (any more) is shown as none. Property "request". */
void gh_requests_view_set_request(GhRequestsView *self, GhConversation *request);
GhConversation *gh_requests_view_get_request(GhRequestsView *self);

/* backend NULL: Delete and Block are unavailable. destroy releases data
 * when replaced or when the view is disposed. */
void gh_requests_view_set_backend(GhRequestsView *self, const GhRequestsBackend *backend,
                                  gpointer data, GDestroyNotify destroy);

/* The confirmation dialog shown for Delete or Block, if any (tests); NULL
 * once it was answered or dismissed. */
AdwAlertDialog *gh_requests_view_get_confirmation(GhRequestsView *self);

/* The largest part of the first message shown, in characters. */
#define GH_REQUESTS_VIEW_MAX_PREVIEW 2000

G_END_DECLS
#endif
