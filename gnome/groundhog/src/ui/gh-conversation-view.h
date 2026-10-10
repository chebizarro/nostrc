#ifndef GH_CONVERSATION_VIEW_H
#define GH_CONVERSATION_VIEW_H

#include <adwaita.h>
#include "gh-conversation.h"
#include "gh-delivery-indicator.h"

#include "gh-web-content.h"
#include <nostr-gtk-1.0/gn-nostr-reference.h>

G_BEGIN_DECLS

typedef struct _GhMessageRow GhMessageRow;

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
/* A local event instead of a message (message NULL): the conversation's
 * disappearing timer changed (charter §3.7, nostrc-qp24.83), in its own
 * words ("You set messages to disappear after 1 day"), and when (unix
 * seconds). Readable properties "is-message", "is-event" and "event-text".
 * NULL and 0 for a message. */
const gchar *gh_timeline_item_get_event_text(GhTimelineItem *self);
/* A valid kind-1201/1202 Marmot activity row, never a bubble. */
gboolean gh_timeline_item_get_is_agent(GhTimelineItem *self);
gint64 gh_timeline_item_get_event_at(GhTimelineItem *self);

/* W26 slice B (nostrc-191r): the live reaction summary for this message,
 * or NULL. The summary auto-updates as reactions arrive. */
typedef struct _GhReactionSummary GhReactionSummary;
GhReactionSummary *gh_timeline_item_get_reaction_summary(GhTimelineItem *self);

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
 *    a conversation with unread messages brings the first of them into view,
 *    or the top when unread ones are still in its unloaded older history;
 *  - while the conversation has older history that is not listed, reaching
 *    the top of what is asks the history loader for it (the sliding window;
 *    there is no button, nostrc-p15n5.2); the messages on screen stay in
 *    place as the older ones are listed above them. "Loading earlier
 *    messages…" shows until it finishes; after a failure it is asked for
 *    again only once the reader has left the top and come back;
 *  - links follow gh-link-policy.h: https to an ASCII host opens through
 *    "open-uri" (default: GtkUriLauncher, i.e. the portal); http, IDN and
 *    user-name addresses first show the full address in a confirmation; a
 *    nostr: address is copied ("copy-text"), never fetched or opened;
 *  - "Show Preview" (charter §2.1, D13) asks consent naming the host and the
 *    network mode unless the link-previews setting is on ("Don't ask again"
 *    turns it on), then asks the preview fetcher. Without a fetcher no
 *    preview is offered at all: nothing asks consent for a fetch that can't
 *    happen, and link-previews is never written;
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

/* W26 slice B (nostrc-191r): the reaction store for displaying emoji
 * reaction chips on message bubbles. The view automatically looks up each
 * message's reaction summary when items are created. NULL detaches. */
typedef struct _GhReactionStore GhReactionStore;
void gh_conversation_view_set_reaction_store(GhConversationView *self,
                                             GhReactionStore *store);

/* W26 slice B (nostrc-191r): the callback invoked when the user toggles a
 * reaction chip (add or remove their emoji on a message). The caller
 * dispatches to the appropriate backend (NIP-17, NIP-29 or MLS). */
typedef void (*GhConversationViewReactFunc)(GhConversation *conversation,
                                            GhMessage *target,
                                            const gchar *emoji,
                                            gboolean add,
                                            gpointer user_data);
void gh_conversation_view_set_reaction_func(GhConversationView *self,
                                            GhConversationViewReactFunc func,
                                            gpointer user_data,
                                            GDestroyNotify destroy);
gboolean gh_conversation_view_get_compact(GhConversationView *self);
/* The visible timeline: a GListModel (and GtkSectionModel) of GhTimelineItem:
 * the conversation's messages and, after the messages written at or before
 * it, its latest timer change (a local event; nostrc-qp24.83). */
GListModel *gh_conversation_view_get_timeline(GhConversationView *self);
GtkListView *gh_conversation_view_get_message_list(GhConversationView *self);

/* Borrowed, escaped markup for the current account's immutable message.
 * A cache miss schedules rendering; list-row bind never parses Markdown. */
const gchar *gh_conversation_view_get_render_markup(GhConversationView *self,
                                                     GhMessage *message);
/* Borrowed offline NIP-21/NIP-18 descriptor from the render cache; no relay lookup. */
gboolean gh_conversation_view_get_reference(GhConversationView *self, GhMessage *message,
                                           const gchar **uri, const gchar **label);
const gchar *gh_conversation_view_get_preview_uri(GhConversationView *self,
                                                GhMessage *message);
/* The descriptor was parsed off the row-bind path. The source must answer from
 * a verified local memory cache, without I/O or network access. NULL means
 * unresolved; the returned summary is owned by the caller. */
typedef gchar *(*GhReferenceSummaryFunc)(const GnNostrReference *reference,
                                         gpointer user_data);
void gh_conversation_view_set_reference_source(GhConversationView *self,
                                                GhReferenceSummaryFunc summary,
                                                gpointer user_data,
                                                GDestroyNotify destroy);
gchar *gh_conversation_view_dup_reference_summary(GhConversationView *self, GhMessage *message);
gboolean gh_conversation_view_has_public_note_reference(GhConversationView *self,
                                                       GhMessage *message);
gboolean gh_conversation_view_can_find_references(GhConversationView *self);
void gh_conversation_view_references_changed(GhConversationView *self);

/* Scrolls to the newest message and sticks there. */
void gh_conversation_view_scroll_to_latest(GhConversationView *self);
/* Whether the newest message is on screen (the view sticks to it). */
gboolean gh_conversation_view_get_at_latest(GhConversationView *self);
/* New messages below the visible ones ("Jump to Latest" shows them). */
guint gh_conversation_view_get_new_below(GhConversationView *self);

/* Older history (e.g. gh_store_conversations_load_older()): called when the
 * user reaches the top of a conversation that has older messages, at most
 * once until
 * gh_conversation_view_finish_loading_older() (listed) or
 * gh_conversation_view_fail_loading_older() (not: announced, and tried again
 * only after the reader leaves the top and returns). In the application gh_conversation_list_attach()
 * installs it (gh_conversation_list_set_history_source()). */
typedef void (*GhConversationViewLoadOlder)(GhConversationView *view,
                                            GhConversation *conversation,
                                            gpointer user_data);
void gh_conversation_view_set_history_loader(GhConversationView *self,
                                             GhConversationViewLoadOlder load_older,
                                             gpointer user_data, GDestroyNotify destroy);
void gh_conversation_view_finish_loading_older(GhConversationView *self);
void gh_conversation_view_fail_loading_older(GhConversationView *self);
gboolean gh_conversation_view_get_loading_older(GhConversationView *self);
/* The last load failed (until a new load or conversation). */
gboolean gh_conversation_view_get_older_failed(GhConversationView *self);

typedef enum {
  GH_CONVERSATION_WINDOW_NEWER,
  GH_CONVERSATION_WINDOW_LATEST
} GhConversationWindowNavigation;
typedef void (*GhConversationViewWindowNavigate)(GhConversationView *view,
                                                  GhConversation *conversation,
                                                  GhConversationWindowNavigation navigation,
                                                  gpointer user_data);
void gh_conversation_view_set_window_loader(GhConversationView *self,
                                            GhConversationViewWindowNavigate navigate,
                                            gpointer user_data);
void gh_conversation_view_finish_window_navigation(GhConversationView *self,
                                                   GhConversationWindowNavigation navigation,
                                                   gboolean success);
gboolean gh_conversation_view_get_loading_newer(GhConversationView *self);
/* A reply target outside the window is loaded from the store on demand. */
typedef void (*GhConversationViewLoadTarget)(GhConversationView *view,
                                             GhConversation *conversation,
                                             const gchar *rumor_id,
                                             gpointer user_data);
void gh_conversation_view_set_target_loader(GhConversationView *self,
                                            GhConversationViewLoadTarget load_target,
                                            gpointer user_data);
gboolean gh_conversation_view_scroll_to_message(GhConversationView *self,
                                                const gchar *rumor_id);

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

/* Production web loader. No fetch until a per-item confirmation. Remembered
 * consent lasts only for the open conversation and is revoked by Preferences. */
void gh_conversation_view_enable_web_content(GhConversationView *self,
                                             const GhHttpTransport *transport, gpointer data);
typedef gchar *(*GhPictureUriFunc)(const gchar *pubkey, gpointer data);
/* The shared profile-picture cache (consent per contact, one download per
 * URL): with it, an allow for a picture is kept for that contact and the
 * picture shows wherever the contact appears. NULL: per-conversation only. */
void gh_conversation_view_set_picture_cache(GhConversationView *self, GObject *cache);
/* The picture URL for a contact as the picture source gives it (checked by
 * the link policy), or NULL. Transfer full. */
gchar *gh_conversation_view_dup_picture_uri_for(GhConversationView *self, const gchar *pubkey);
void gh_conversation_view_set_picture_source(GhConversationView *self, GhPictureUriFunc func,
                                              GObject *source);
gboolean gh_conversation_view_has_web_content(GhConversationView *self);
gchar *gh_conversation_view_dup_picture_uri(GhConversationView *self, GhMessage *message);

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
/* Whether a fetcher is set, i.e. whether any preview is offered. */
gboolean gh_conversation_view_get_previews_available(GhConversationView *self);

typedef enum {
  GH_LINK_PREVIEW_NONE,        /* not asked for */
  GH_LINK_PREVIEW_ASKING,      /* waiting for consent */
  GH_LINK_PREVIEW_LOADING,
  GH_LINK_PREVIEW_LOADED,
  GH_LINK_PREVIEW_FAILED,
  GH_LINK_PREVIEW_UNAVAILABLE  /* the fetcher was removed while asking: nothing loaded */
} GhLinkPreviewState;
GdkTexture *gh_conversation_view_get_web_texture(GhConversationView *self, GhMessage *message,
                                                 GhWebKind kind, GhLinkPreviewState *state);
/* Loads message's picture / linked image without a click when the kind's
 * preference is on or its sender was allowed here; else does nothing. */
void gh_conversation_view_auto_load(GhConversationView *self, GhMessage *message, GhWebKind kind);


/* The preview state of @message; title and description (nullable, borrowed)
 * for LOADED. The view emits "preview-changed" (rumor id) when it changes. */
GhLinkPreviewState gh_conversation_view_get_link_preview(GhConversationView *self,
                                                         GhMessage *message,
                                                         const gchar **title,
                                                         const gchar **description);
/* Metadata-only URL from a consented page preview; artwork needs a separate
 * explicit image request through conversation.show-preview. */
const gchar *gh_conversation_view_get_og_image_uri(GhConversationView *self,
                                                   GhMessage *message);
/* The loaded page's og:site_name (e.g. "GitHub"), or NULL. */
const gchar *gh_conversation_view_get_link_preview_site(GhConversationView *self,
                                                        GhMessage *message);

/* Charter §7.15 state 11: the banner "@name hasn't set up private messaging
 * yet" (NULL hides it). */
void gh_conversation_view_set_recipient_without_inbox(GhConversationView *self,
                                                      const gchar *name);
/* The same for a room of several people where nobody has set up private
 * messaging (nostrc-lff5): "No one in this conversation has set up private
 * messaging yet" (FALSE hides it). It takes the banner's place over a name. */
void gh_conversation_view_set_room_without_inbox(GhConversationView *self, gboolean nobody);
/* Charter §7.15 state 12: "Waiting for Grotto to unlock N messages"
 * with Unlock (conversation.unlock-messages); 0 hides it. */
void gh_conversation_view_set_locked_messages(GhConversationView *self, guint count);
/* Charter §7.15 state 13, encrypted groups (nostrc-oya4): "Some messages in
 * this group can't be read yet." while GhMlsGroup:decrypt-pending; FALSE
 * hides it. No number and no cause: a held event's type (message or group
 * change) is sealed until its epoch opens, and junk anyone posts looks the
 * same (W22 review N4). No action: nothing the user can do makes them
 * readable. The getter reads what is shown, for tests. */
void gh_conversation_view_set_decrypt_pending(GhConversationView *self, gboolean pending);
gboolean gh_conversation_view_get_decrypt_pending(GhConversationView *self);
/* Why the shown group can't be read past a change, for good (nostrc-prrl):
 * shown in the same row instead of "can't be read yet". NULL clears it. */
void gh_conversation_view_set_unreadable_reason(GhConversationView *self, const gchar *reason);
const gchar *gh_conversation_view_get_unreadable_reason(GhConversationView *self);

/* The view's accessibility announcements (charter §7.14), for tests: how
 * many were made at @priority, and the last text. */
guint gh_conversation_view_get_announcements(GhConversationView *self,
                                             GtkAccessibleAnnouncementPriority priority);
const gchar *gh_conversation_view_get_last_announcement(GhConversationView *self);


/* Row enricher (W26 polls): called by GhMessageRow when its message changes,
 * so the MLS UI layer can inject a poll card without a dependency from the
 * conversation UI to MLS types. The enricher receives the row, its current
 * message (nullable), and its data; it should call
 * gh_message_row_set_poll_widget() as needed. */
typedef void (*GhConversationViewRowEnricher)(GhMessageRow *row, GhMessage *message,
                                              gpointer data);
void gh_conversation_view_set_row_enricher(GhConversationView *self,
                                           GhConversationViewRowEnricher enricher,
                                           gpointer data);
/* Called by GhMessageRow after it updates its message (update_all).
 * Invokes the registered enricher, if any. */
void gh_conversation_view_enrich_row(GhConversationView *self, GhMessageRow *row);

G_END_DECLS
#endif
