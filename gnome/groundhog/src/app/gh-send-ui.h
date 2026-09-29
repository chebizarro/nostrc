#ifndef GH_SEND_UI_H
#define GH_SEND_UI_H

#include <adwaita.h>

#include "gh-account-store.h"
#include "gh-dm-inbox.h"
#include "gh-delivery-indicator.h"
#include "gh-window.h"

G_BEGIN_DECLS

struct _GhExpiry;

typedef struct {
  GhAccountController *accounts;       /* required */
  GhConversationStore *conversations;  /* required: the model the window lists */
  GhAccountStore *account_store;       /* required: drafts and the account's outbox */
  GhDmInbox *inbox;                    /* nullable: messages waiting for the signer */
  GSettings *settings;                 /* nullable: enter-sends, signer-method */
  struct _GhExpiry *expiry;            /* nullable: the open store's timers (G07) */
} GhSendUiConfig;

/*
 * Sending from the window (charter §3.5, §3.6, §7.6, §7.7, §7.14, §7.15, G13):
 * connects the content page's GhComposer and the conversation view (which
 * gh_conversation_list_attach() must have installed) to the active account's
 * durable outbox (gh-outbox.h, from the account store) and drafts.
 *
 *  - Send: gh_outbox_send() to the shown conversation's one other participant,
 *    or to the account itself in a note to self. T-enqueue is written before
 *    any signer call and clears the stored draft; the UI never waits for the
 *    signer or a relay. The message is shown at once (its local echo, from the
 *    queued rumor) and its status follows the outbox honestly: "Waiting for
 *    approval", "Sending…", "Sent", "Not sent", ... (GhOutboxItem:status is
 *    bound to GhMessage:status). A text the store could not queue (e.g.
 *    storage full) stays in the composer with the reason under it.
 *  - Status after a restart: the own messages of the shown conversation are
 *    matched with their outbox entries, settled ones included, so they say
 *    "Sent" again and have delivery details; a self-copy of a message sent
 *    from another device has none and shows no status.
 *  - The view's seams: its delivery details come from the outbox's per-relay
 *    outcomes, "retry-requested" retries through the outbox,
 *    "unlock-requested" asks the inbox to offer the messages the signer did
 *    not unlock again (gh_dm_inbox_unlock()), whose count the view shows
 *    ("Waiting for Nostr Signer to unlock N messages"), and a recipient
 *    without a message inbox (the newest own message "Can't send") gets the
 *    view's banner.
 *  - Drafts: the composer's text is saved to the conversation's draft 1 s
 *    after the last edit and when another conversation is shown, and a
 *    conversation's draft is restored when it is shown (also after a
 *    restart).
 *  - Why sending is unavailable, in the composer's place (never a disabled
 *    button without a reason): no active account, the signer unreachable or
 *    unsupported (gh_account_describe_limits()), the message store opening,
 *    locked, unavailable, damaged or failed, a NIP-17 group conversation
 *    (only one-to-one sending exists yet; a relay group's reason is its
 *    delegate's, below), or a recipient without a message inbox
 *    (with "Check Again", which retries that message). Offline is not a
 *    reason: the outbox waits for the connection.
 *  - Length: the composer measures texts with gh_outbox_text_fits().
 *  - Enter sends while the enter-sends setting is on.
 *  - The disappearing timer before sending (charter §3.7, W14 review B1):
 *    the composer's "disappearing-timer" is the shown conversation's timer
 *    (gh_expiry_get_timer()), kept live through GhExpiry's "timer-changed"
 *    (e.g. from Conversation Info); 0 (hidden) with no GhExpiry, no
 *    conversation or a timer that can't be read.
 * Everything is released with window.
 */
void gh_send_ui_attach(GhWindow *window, const GhSendUiConfig *config);

/* The GhExpiry of the account's open store, NULL while none is open: the
 * application calls this whenever it creates or disposes one (before the
 * dispose), and after changing its default timer, which a conversation not
 * stored yet shows. The window holds a reference until the next call and
 * disconnects from the previous one. Nothing for a window without the send
 * UI. */
void gh_send_ui_set_expiry(GhWindow *window, struct _GhExpiry *expiry);

/* G20b: another sending engine for some conversations (NIP-29 relay groups,
 * gh-group-ui.c). For a conversation it handles, the composer's reason,
 * send, retry and delivery details are the delegate's, never the NIP-17
 * outbox's, and no draft is stored (drafts are NIP-17 store rows). */
typedef struct {
  gboolean (*handles)(GhConversation *conversation, gpointer data);
  /* Why sending is unavailable, or NULL when it is (transfer full). */
  gchar *(*reason)(GhConversation *conversation, gpointer data);
  /* Queues text; FALSE with a user-facing error (it stays in the composer). */
  gboolean (*send)(GhConversation *conversation, const gchar *text, gpointer data,
                   GError **error);
  /* The user's Retry of an own message; FALSE with error. */
  gboolean (*retry)(GhMessage *message, gpointer data, GError **error);
  /* Delivery details of an own message (transfer full), or NULL. */
  GhDeliveryReport *(*report)(GhMessage *message, gpointer data);
} GhSendUiDelegate;

/* Sets (NULL: clears) window's delegate; delegate is copied, data is borrowed
 * until replaced. Nothing for a window without the send UI. */
void gh_send_ui_set_delegate(GhWindow *window, const GhSendUiDelegate *delegate, gpointer data);
/* Evaluates the composer's reason again (the delegate's state changed). */
void gh_send_ui_refresh(GhWindow *window);

G_END_DECLS
#endif
