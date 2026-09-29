#ifndef GH_NOTIFIER_H
#define GH_NOTIFIER_H

#include "gh-clock.h"
#include "gh-conversation-store.h"
#include "gh-window.h"

G_BEGIN_DECLS

/*
 * GhNotifier: private message notifications (privacy charter §5.1, §5.2,
 * §8.2 G16; tests NO-1…NO-8). One per process, made by gh-app-services.c and
 * reachable with gh_notifier_get_for_application(). It sends GNotifications
 * through its GApplication (org.gtk.Notifications on a GNOME desktop, the
 * Notification portal in a sandbox) and keeps no copy of what it showed.
 *
 * What is notified: an incoming message the conversation model newly
 * admitted ("message-added") for its bound account, while
 * `notifications-enabled` is on. Never:
 *   - the account's own messages (self-copies, messages sent elsewhere);
 *   - messages written before this session: before the model bound the
 *     account (backfill of what arrived while Groundhog was not receiving);
 *   - expired messages;
 *   - a conversation that is muted or blocked (room_state: the encrypted
 *     store, never GSettings), visible in the active window
 *     (gh_notifier_set_visible_conversation()), or read by the time the
 *     notification would go out. Decrypt failures never reach the model.
 *
 * Content (`notification-privacy`; any unknown value counts as hidden):
 *   hidden (default)  title "New message", body "N new messages", one id for
 *                     the whole app (GH_NOTIFIER_ID_MESSAGES); no name, npub,
 *                     subject or text.
 *   sender            title the conversation's name, body generic ("New
 *                     message", "N new messages"); an id per conversation
 *                     (GH_NOTIFIER_ID_CONVERSATION_PREFIX + a number).
 *   preview           as sender, with the newest message's text as the body:
 *                     line breaks collapsed, at most GH_NOTIFIER_PREVIEW_MAX
 *                     graphemes, prefixed with its author ("npub1…: ") in a
 *                     group.
 * Message Requests are hidden at every level (counted in
 * GH_NOTIFIER_ID_MESSAGES). No icon is set, so the desktop shows the app's
 * (an avatar would be a fetch and a lock-screen exposure). Category
 * GH_NOTIFIER_CATEGORY; priority normal, never urgent, so Do Not Disturb
 * wins. GNotification has no lock-screen hint: GNOME shows details on the
 * lock screen only if the user allows it for Groundhog in Settings ›
 * Notifications (off by default), which is why hidden is the default.
 *
 * Coalescing (N2): an id is sent on the main-loop turn after the message
 * arrived and then updated at most once per GH_NOTIFIER_UPDATE_INTERVAL_MS
 * with the count and text recomputed, so a burst becomes one notification per
 * id. The "play-sound" signal (its default handler rings the desktop's alert
 * sound; GNotification has no sound of its own) is emitted with a new
 * message, only with `sound-enabled`, at most once per
 * GH_NOTIFIER_SOUND_INTERVAL_MS.
 *
 * Withdrawal (N3): a conversation's share is withdrawn when it is read,
 * visible in the active window, forgotten, muted or blocked
 * (gh_notifier_withdraw_conversation()), or when a message it counts is
 * purged or expires; everything on account switch, when notifications are
 * turned off and when the content level changes (later messages use the new
 * level). A count that shrinks is not sent again (that would alert again);
 * the notification goes once nothing is left.
 *
 * Activation (N6, N7): the one action is app.open-conversation (GH_NOTIFIER_
 * ACTION) with a (tx) target: the account generation (unique to this
 * notifier; it changes with every account switch) and a conversation number
 * of that generation. Neither names an account or a room. The hidden
 * notification carries a target only while it counts a single conversation.
 * Activation first activates the application (its window comes up), then
 * emits "open-conversation" for a current target, or "stale-activation" for
 * any other (another or an earlier account, an earlier process) with that
 * account's npub if this process knew it: a stale target never opens a
 * thread. With a window attached (gh_notifier_attach_window()) the default
 * handlers open the conversation, or show the list and the toast "That
 * notification was for another account" with a Switch button.
 *
 * Locked store (charter §3.4, NO-11): gh_notifier_set_store_locked() shows one
 * hidden-level notice (GH_NOTIFIER_ID_STORE_LOCKED) while no window is
 * attached, withdrawn once the store opens or a window shows the state.
 *
 * GTK-free apart from the window glue; used on the main context.
 */

#define GH_NOTIFIER_ACTION "open-conversation"
#define GH_NOTIFIER_CATEGORY "im.received"
#define GH_NOTIFIER_ID_MESSAGES "messages"
#define GH_NOTIFIER_ID_STORE_LOCKED "store-locked"
#define GH_NOTIFIER_ID_CONVERSATION_PREFIX "conv-"
#define GH_NOTIFIER_UPDATE_INTERVAL_MS 2000
#define GH_NOTIFIER_SOUND_INTERVAL_MS 10000
#define GH_NOTIFIER_PREVIEW_MAX 120

/* A room's notification state where it is kept (the encrypted store). */
typedef struct {
  gint64 muted_until;  /* unix seconds; 0 = not muted */
  gboolean blocked;    /* never notified */
} GhNotifierRoomState;

/* Fills *state for @room_id of the bound account. FALSE when it cannot be
 * read: the room is then not notified (fail closed). */
typedef gboolean (*GhNotifierRoomStateFunc)(gpointer data, const gchar *room_id,
                                            GhNotifierRoomState *state);

typedef struct {
  /* Required: org.nostr.Groundhog (notifications-enabled,
   * notification-privacy, sound-enabled). */
  GSettings *settings;
  GhConversationStore *conversations;  /* required: the process's model */
  GhClock *clock;                      /* NULL = system clock */
  GhNotifierRoomStateFunc room_state;  /* nullable: nothing muted or blocked */
  gpointer room_state_data;
} GhNotifierConfig;

#define GH_TYPE_NOTIFIER (gh_notifier_get_type())
G_DECLARE_FINAL_TYPE(GhNotifier, gh_notifier, GH, NOTIFIER, GObject)

/* Adds app.open-conversation to app. Dispose (g_object_run_dispose())
 * withdraws everything it shows, removes the action and detaches from app.
 * Signals: "open-conversation" (GhConversation), "stale-activation"
 * (nullable npub string) and "play-sound" (see above). */
GhNotifier *gh_notifier_new(GApplication *app, const GhNotifierConfig *config);

/* Borrowed; NULL when the process has none. */
GhNotifier *gh_notifier_get_for_application(GApplication *app);

/* The conversation on screen in the active window, or NULL. Setting it
 * withdraws its notification, and nothing is notified for it meanwhile. */
void gh_notifier_set_visible_conversation(GhNotifier *self, GhConversation *conversation);
GhConversation *gh_notifier_get_visible_conversation(GhNotifier *self);

/* Withdraws what is shown for @room_id and drops its pending messages: for
 * a mute, a block or a purge of its messages. Later messages are notified
 * as usual (room_state decides). */
void gh_notifier_withdraw_conversation(GhNotifier *self, const gchar *room_id);

/* Whether the active account's store is locked (charter §3.4). */
void gh_notifier_set_store_locked(GhNotifier *self, gboolean locked);

/* Follows window: the conversation it shows while active (see
 * gh_notifier_set_visible_conversation()), and the target of the default
 * "open-conversation" and "stale-activation" handlers. Released with the
 * window. */
void gh_notifier_attach_window(GhNotifier *self, GhWindow *window);

G_END_DECLS
#endif
