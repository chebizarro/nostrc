#ifndef GH_CONVERSATION_VIEW_H
#define GH_CONVERSATION_VIEW_H

#include <adwaita.h>
#include "gh-conversation.h"
#include "gh-delivery-indicator.h"

G_BEGIN_DECLS

/* One entry of a conversation's timeline: a message and how it sits among
 * its neighbours. A run is consecutive messages from one sender on one local
 * day, each within 5 minutes of the previous (charter §7.6): its first shows
 * the sender's name (incoming, multi-party rooms), its last the time.
 * "day-label" is the day separator text of the message's local day
 * ("Today", "Yesterday", a weekday, or the date). Read-only properties:
 * "message", "run-start", "run-end", "show-sender" and "day-label"; the
 * booleans and the label notify when a neighbour or the date changes. */
#define GH_TYPE_TIMELINE_ITEM (gh_timeline_item_get_type())
G_DECLARE_FINAL_TYPE(GhTimelineItem, gh_timeline_item, GH, TIMELINE_ITEM, GObject)

GhMessage *gh_timeline_item_get_message(GhTimelineItem *self);
gboolean gh_timeline_item_get_run_start(GhTimelineItem *self);
gboolean gh_timeline_item_get_run_end(GhTimelineItem *self);
gboolean gh_timeline_item_get_show_sender(GhTimelineItem *self);
const gchar *gh_timeline_item_get_day_label(GhTimelineItem *self);

/* The day separator text for the local day of @when relative to @now:
 * "Today", "Yesterday", the weekday within the last 6 days, else the
 * localized date (with the year only when it is not @now's year). */
gchar *gh_conversation_view_format_day(GDateTime *when, GDateTime *now);

/*
 * GhConversationView (data/ui/gh-conversation-view.blp, charter §7.4, §7.6)
 * shows the messages of one GhConversation:
 *  - bubbles (GhMessageRow) in a ListView sectioned by local day with day
 *    separators, grouped into runs: exactly the messages the conversation
 *    holds. An expired message is never in it (G07: the store neither lists
 *    nor admits one, and GhExpiry takes it out when it expires), so the view
 *    has no expiry timer of its own;
 *  - it sticks to the newest message while scrolled to the bottom; otherwise
 *    "Jump to Latest" appears with the number of new messages below. Opening
 *    a conversation with unread messages brings the first of them into view;
 *  - scrolling near the top asks the history loader for older messages
 *    ("Loading earlier messages" shows until it finishes);
 *  - links follow gh-link-policy.h: https to an ASCII host opens through
 *    "open-uri" (default: GtkUriLauncher, i.e. the portal); http, IDN and
 *    user-name addresses first show the full address in a confirmation; a
 *    nostr: address is copied ("copy-text"), never fetched or opened;
 *  - "Show Preview" (charter §2.1, D13) asks consent naming the host and the
 *    network mode unless the link-previews setting is on ("Don't ask again"
 *    turns it on), then asks the preview fetcher; without one it says that
 *    previews are not available and nothing is loaded;
 *  - announcements (charter §7.14): a new incoming message is announced
 *    politely and an own message that becomes "Not sent" assertively, only
 *    while the window is active.
 * Actions (widget actions of the view): conversation.open-link (s: URI),
 * conversation.show-preview (s: rumor id), conversation.retry-message
 * (s: rumor id), conversation.jump-to-latest and
 * conversation.unlock-messages. Signals: "retry-requested" (GhMessage) for
 * the outbox (G06/G13), "unlock-requested" for the inbox, "open-uri" (URI,
 * run last) whose default handler launches the URI, "copy-text" (text, run
 * last) whose default handler puts it on the clipboard, and
 * "preview-changed" (rumor id) when a message's link preview state changes.
 * Properties: "conversation", "compact" (below 480sp: narrower bubbles) and
 * "settings" (nullable GSettings of org.nostr.Groundhog for link-previews and
 * network-mode). Nothing here publishes, fetches or marks anything read.
 */
#define GH_TYPE_CONVERSATION_VIEW (gh_conversation_view_get_type())
G_DECLARE_FINAL_TYPE(GhConversationView, gh_conversation_view, GH, CONVERSATION_VIEW,
                     AdwBreakpointBin)

GtkWidget *gh_conversation_view_new(void);

/* The shown conversation (NULL: none). Read its unread count before marking
 * it read: that decides where the view opens. */
void gh_conversation_view_set_conversation(GhConversationView *self,
                                           GhConversation *conversation);
GhConversation *gh_conversation_view_get_conversation(GhConversationView *self);
void gh_conversation_view_set_settings(GhConversationView *self, GSettings *settings);
gboolean gh_conversation_view_get_compact(GhConversationView *self);
/* The visible timeline: a GListModel (and GtkSectionModel) of GhTimelineItem. */
GListModel *gh_conversation_view_get_timeline(GhConversationView *self);
GtkListView *gh_conversation_view_get_message_list(GhConversationView *self);

/* Scrolls to the newest message and sticks there. */
void gh_conversation_view_scroll_to_latest(GhConversationView *self);
/* Whether the newest message is on screen (the view sticks to it). */
gboolean gh_conversation_view_get_at_latest(GhConversationView *self);
/* New messages below the visible ones ("Jump to Latest" shows them). */
guint gh_conversation_view_get_new_below(GhConversationView *self);

/* Older history (e.g. gh_store_conversations_load_older()): called when the
 * user scrolls near the top of a conversation that has older messages, at
 * most once until gh_conversation_view_finish_loading_older(). */
typedef void (*GhConversationViewLoadOlder)(GhConversationView *view,
                                            GhConversation *conversation,
                                            gpointer user_data);
void gh_conversation_view_set_history_loader(GhConversationView *self,
                                             GhConversationViewLoadOlder load_older,
                                             gpointer user_data, GDestroyNotify destroy);
void gh_conversation_view_finish_loading_older(GhConversationView *self);
gboolean gh_conversation_view_get_loading_older(GhConversationView *self);

/* Per-relay outcomes of an own message for its delivery details, e.g. from
 * GhOutboxItem (gh-outbox.h). NULL: none known. */
typedef GhDeliveryReport *(*GhConversationViewDeliveryReport)(GhMessage *message,
                                                              gpointer user_data);
void gh_conversation_view_set_delivery_report_func(GhConversationView *self,
                                                   GhConversationViewDeliveryReport func,
                                                   gpointer user_data, GDestroyNotify destroy);
/* The report for @message (transfer full; NULL when no function is set or it
 * knows none). Used by the delivery details. */
GhDeliveryReport *gh_conversation_view_dup_delivery_report(GhConversationView *self,
                                                           GhMessage *message);

/* A link preview fetcher (charter §2.1: the configured network mode, GET over
 * https only, bounded). It is only ever called after the user asked for a
 * preview. finish returns TRUE with title and description (either may be
 * NULL) or FALSE with error. */
typedef void (*GhLinkPreviewFetch)(const gchar *uri, GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer callback_data,
                                   gpointer user_data);
typedef gboolean (*GhLinkPreviewFinish)(GAsyncResult *result, gchar **title,
                                        gchar **description, GError **error,
                                        gpointer user_data);
void gh_conversation_view_set_link_preview_fetcher(GhConversationView *self,
                                                   GhLinkPreviewFetch fetch,
                                                   GhLinkPreviewFinish finish,
                                                   gpointer user_data, GDestroyNotify destroy);

typedef enum {
  GH_LINK_PREVIEW_NONE,        /* not asked for */
  GH_LINK_PREVIEW_ASKING,      /* waiting for consent */
  GH_LINK_PREVIEW_LOADING,
  GH_LINK_PREVIEW_LOADED,
  GH_LINK_PREVIEW_FAILED,
  GH_LINK_PREVIEW_UNAVAILABLE  /* no fetcher: nothing was loaded */
} GhLinkPreviewState;

/* The preview state of @message; title and description (nullable, borrowed)
 * for LOADED. The view emits "preview-changed" (rumor id) when it changes. */
GhLinkPreviewState gh_conversation_view_get_link_preview(GhConversationView *self,
                                                         GhMessage *message,
                                                         const gchar **title,
                                                         const gchar **description);

/* Charter §7.15 state 11: the banner "@name hasn't set up private messaging
 * yet" (NULL hides it). */
void gh_conversation_view_set_recipient_without_inbox(GhConversationView *self,
                                                      const gchar *name);
/* Charter §7.15 state 12: "Waiting for Nostr Signer to unlock N messages"
 * with Unlock (conversation.unlock-messages); 0 hides it. */
void gh_conversation_view_set_locked_messages(GhConversationView *self, guint count);

/* The view's accessibility announcements (charter §7.14), for tests: how
 * many were made at @priority, and the last text. */
guint gh_conversation_view_get_announcements(GhConversationView *self,
                                             GtkAccessibleAnnouncementPriority priority);
const gchar *gh_conversation_view_get_last_announcement(GhConversationView *self);

G_END_DECLS
#endif
