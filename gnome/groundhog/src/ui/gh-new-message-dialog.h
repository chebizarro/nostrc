#ifndef GH_NEW_MESSAGE_DIALOG_H
#define GH_NEW_MESSAGE_DIALOG_H

#include <adwaita.h>

#include "gh-conversation-store.h"
#include "gh-inbox-resolver.h"
#include "gh-nip05.h"
#include "gh-window.h"

G_BEGIN_DECLS

/*
 * GhNewMessageDialog (privacy charter §7.9, §2 PD-2/PD-8, §8.2 G18;
 * data/ui/gh-new-message-dialog.blp): starting a conversation.
 *
 * Lookup consent. Typing searches only what is already on this device: the
 * people of accepted conversations, by their cached name, claimed address
 * or npub. An npub, nprofile or nostr: URI is decoded offline
 * (gh-recipient.h). Nothing contacts the network until the user chooses a
 * row that says what it contacts:
 *   - "Look up name@domain" (subtitle: "Connects to domain …") runs one
 *     NIP-05 lookup (gh-nip05.h) for that address;
 *   - on the confirm page, "Check Whether They Can Receive Private Messages"
 *     (subtitle: the discovery relays it asks) resolves each recipient's
 *     kind-10050 inbox through the GhInboxResolver (the contact directory).
 * Pressing Enter adds an exact or local match only; it never looks anything
 * up.
 *
 * Recipients are chips, up to GH_CONVERSATION_MAX_PEERS (10); at the limit
 * the dialog says "A private conversation can include up to 10 people." and
 * then, only when config->encrypted_groups, "For larger groups, create an
 * encrypted group.", else "Larger groups aren't available in this version
 * yet." (P4: nothing points to a missing feature). It adds nobody else. "Note to
 * Self" opens the account's own room. Start Conversation opens (or creates,
 * empty and accepted) the room with gh_conversation_store_open_room(),
 * emits "conversation-started" (GhConversation) and closes; nothing is sent
 * or published. Pending lookups are cancelled when the dialog closes.
 */

typedef struct {
  GhConversationStore *conversations; /* required: contacts, and the room opened */
  GhInboxResolver *inboxes;           /* nullable: no inbox check (the row says so) */
  GhNip05 *nip05;                     /* nullable: addresses can't be looked up (said) */
  GSettings *settings;                /* nullable: discovery-relays, network-mode */
  /* Nullable: what is cached about a contact (the directory's kind 0);
   * never a network request. Returned strings are borrowed. */
  const gchar *(*display_name)(gpointer data, const gchar *pubkey);
  const gchar *(*claimed_nip05)(gpointer data, const gchar *pubkey);
  gpointer names_data;
  /* Whether this build creates encrypted groups (GH_FEATURE_ENCRYPTED_GROUPS),
   * so the 10-recipient limit may point to them. */
  gboolean encrypted_groups;
} GhNewMessageConfig;

typedef enum {
  GH_NEW_MESSAGE_ITEM_NOTE_TO_SELF,
  GH_NEW_MESSAGE_ITEM_CONTACT,   /* someone you talk to (local) */
  GH_NEW_MESSAGE_ITEM_PUBKEY,    /* a pasted npub / nprofile / nostr: URI */
  GH_NEW_MESSAGE_ITEM_LOOKUP,    /* consent row: look up name@domain */
  GH_NEW_MESSAGE_ITEM_RECIPIENT, /* a chip */
  GH_NEW_MESSAGE_ITEM_PERSON     /* a confirm-page row */
} GhNewMessageItemKind;

/* One row or chip; readable properties for the templates: "title",
 * "subtitle", "avatar-text", "show-avatar", "show-initials", "icon-name",
 * "show-icon", "busy", "status", "has-status" and "enabled". */
#define GH_TYPE_NEW_MESSAGE_ITEM (gh_new_message_item_get_type())
G_DECLARE_FINAL_TYPE(GhNewMessageItem, gh_new_message_item, GH, NEW_MESSAGE_ITEM, GObject)

GhNewMessageItemKind gh_new_message_item_get_kind(GhNewMessageItem *self);
const gchar *gh_new_message_item_get_title(GhNewMessageItem *self);
const gchar *gh_new_message_item_get_subtitle(GhNewMessageItem *self);
const gchar *gh_new_message_item_get_status(GhNewMessageItem *self);
const gchar *gh_new_message_item_get_pubkey(GhNewMessageItem *self); /* hex, or NULL */
gboolean gh_new_message_item_get_busy(GhNewMessageItem *self);
gboolean gh_new_message_item_get_enabled(GhNewMessageItem *self);

#define GH_TYPE_NEW_MESSAGE_DIALOG (gh_new_message_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhNewMessageDialog, gh_new_message_dialog, GH, NEW_MESSAGE_DIALOG,
                     AdwDialog)

GhNewMessageDialog *gh_new_message_dialog_new(const GhNewMessageConfig *config);

/* The item models behind the pick page's rows, its chips and the confirm
 * page's rows (GhNewMessageItem), for tests and assistive checks. */
GListModel *gh_new_message_dialog_get_suggestions(GhNewMessageDialog *self);
GListModel *gh_new_message_dialog_get_recipients(GhNewMessageDialog *self);
GListModel *gh_new_message_dialog_get_people(GhNewMessageDialog *self);

/* Makes win.new-message (Ctrl+N) of window present a GhNewMessageDialog
 * made with config (copied; its objects are referenced until the window is
 * destroyed). The action is enabled while config->conversations has an
 * account. A started conversation opens in the window (selected and shown)
 * with keyboard focus in the composer when sending is possible, else in the
 * conversation (gh_content_page_focus_conversation()). */
void gh_new_message_attach(GhWindow *window, const GhNewMessageConfig *config);

G_END_DECLS
#endif
