#ifndef GH_CONVERSATION_INFO_DIALOG_H
#define GH_CONVERSATION_INFO_DIALOG_H

#include <adwaita.h>
#include "gh-conversation-store.h"
#include "gh-store.h"
#include "gh-window.h"

G_BEGIN_DECLS

struct _GhStoreConversations;
struct _GhExpiry;
struct _GhNotifier;

/*
 * GhConversationInfoDialog (data/ui/gh-conversation-info-dialog.blp; privacy
 * charter §7.4, §2.2, §3.7, §3.8, §5.2; item G19): Conversation Info for a
 * private (NIP-17) conversation.
 *
 *  - People: each other person with the name Groundhog shows for them (the
 *    profile function's cached display name, else the short npub; nothing is
 *    fetched), their short npub, a NIP-05 address they claim, shown as not
 *    checked, and whether you marked their key verified. Copy npub; the row
 *    opens Verify Key.
 *  - Verify Key: their full npub in groups of four, the safety code
 *    (gh_privacy_safety_code() of both keys) and your own npub. "Mark as
 *    Verified" records the time in the encrypted store
 *    (gh_store_contacts_set_verified()); the UI only ever says "You marked
 *    this key verified": Groundhog checks nothing with anyone.
 *  - Privacy: gh_privacy_summary_new() for the conversation.
 *  - Mute (charter §5.2 N8, NO-7): 1 hour, 8 hours, 1 week or until turned
 *    back on, in the store's conversations.muted_until, never GSettings;
 *    muting withdraws what the notifier shows for the conversation.
 *  - Disappearing messages (G07): the conversation's timer through GhExpiry,
 *    for messages the account sends from now on, with the charter's copy
 *    (relays are asked to delete; that is not guaranteed).
 *  - Block (confirmed): gh_store_conversations_set_blocked(); the
 *    conversation leaves the list, its notifications are withdrawn and none
 *    follow. The window shows "Conversation blocked" with Undo.
 *  - Forget on This Device (confirmed; charter §3.8, ST-9):
 *    gh_store_conversations_forget(); the copy says the other people and
 *    relays keep their copies.
 * Nothing here publishes anything (P8). Text from profiles is never markup.
 * The dialog closes as soon as its conversation leaves the model (forgotten,
 * blocked, account switch), before the store it borrows can close.
 *
 * Actions (widget actions of the dialog): info.verify (s: pubkey hex),
 * info.mark-verified, info.clear-verified, info.copy-key (s: text),
 * info.mute (s: seconds, or "always"), info.unmute, info.block and
 * info.forget (each presents its confirmation).
 */

/* What Groundhog has cached about a person (GhContactDirectory): display
 * only, copied at once. */
typedef struct {
  const gchar *name;  /* display name, or NULL */
  const gchar *nip05; /* claimed NIP-05 address (never verified), or NULL */
} GhConversationInfoProfile;

typedef void (*GhConversationInfoProfileFunc)(const gchar *pubkey,
                                              GhConversationInfoProfile *profile,
                                              gpointer user_data);

typedef struct {
  /* Required: the model that lists the conversation. */
  GhConversationStore *model;
  /* The account's open store and its delegate, attached to model. Both NULL
   * while no store is open: mute, block, forget and verification then say
   * that they are unavailable. store is borrowed while the conversation is
   * listed. */
  struct _GhStoreConversations *conversations;
  GhStore *store;
  struct _GhExpiry *expiry;     /* nullable: the timer row says it is unavailable */
  struct _GhNotifier *notifier; /* nullable */
  GhConversationInfoProfileFunc profile; /* nullable */
  gpointer profile_data;
} GhConversationInfoServices;

#define GH_TYPE_CONVERSATION_INFO_DIALOG (gh_conversation_info_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhConversationInfoDialog, gh_conversation_info_dialog, GH,
                     CONVERSATION_INFO_DIALOG, AdwDialog)

/* A dialog for conversation (listed in services->model). */
GhConversationInfoDialog *gh_conversation_info_dialog_new(GhConversation *conversation,
                                                          const GhConversationInfoServices *services);
GhConversation *gh_conversation_info_dialog_get_conversation(GhConversationInfoDialog *self);

/* Window glue (charter §7.4): adds win.conversation-info to window, enabled
 * while its content page shows a conversation, and the content page's info
 * button uses it. Activating it fills a GhConversationInfoServices with
 * services_func (FALSE: nothing opens) and presents the dialog for the shown
 * conversation. user_data is released with destroy together with window. */
typedef gboolean (*GhConversationInfoServicesFunc)(GhConversationInfoServices *services,
                                                   gpointer user_data);
void gh_conversation_info_attach(GhWindow *window, GhConversationInfoServicesFunc services_func,
                                 gpointer user_data, GDestroyNotify destroy);

/* G20b: a relay group (NIP-29) has its own info dialog (GhGroupInfoDialog).
 * With a handler set, win.conversation-info is enabled for a shown relay
 * group too and runs func instead of this dialog (FALSE: nothing opened);
 * without one it stays disabled there (W15 review non-blocking #2). NULL
 * clears. user_data is borrowed until replaced or the window goes. */
typedef gboolean (*GhConversationInfoGroupFunc)(GhWindow *window, GhConversation *conversation,
                                                gpointer user_data);
void gh_conversation_info_set_group_handler(GhWindow *window, GhConversationInfoGroupFunc func,
                                            gpointer user_data);

G_END_DECLS
#endif
