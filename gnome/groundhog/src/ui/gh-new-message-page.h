#ifndef GH_NEW_MESSAGE_PAGE_H
#define GH_NEW_MESSAGE_PAGE_H

#include <adwaita.h>

#include "gh-conversation-store.h"
#include "gh-inbox-resolver.h"
#include "gh-nip05.h"
#include "gh-window.h"

G_BEGIN_DECLS

/*
 * GhNewMessagePage (privacy charter §7.9, §2 PD-2/PD-8, §8.2 G18;
 * data/ui/gh-new-message-page.blp): starting a conversation, as a panel the
 * sidebar slides to (gh_sidebar_page_push_panel(), modelled on Signal's "New
 * chat"): Back (new-message.close) or Escape slides back, from the confirm
 * page first to the search. Above the contacts are rows for the window's
 * win.new-group, win.join-group and win.add-contact (each shown only while
 * the window has that action, hidden while searching); choosing one leaves
 * the panel and runs it. Contacts show their profile picture only when the
 * shared cache has it with that contact's consent (config->picture, W32);
 * otherwise initials of a cached name, never of an npub.
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
 * the panel says "A private conversation can include up to 10 people." and
 * then, only when config->encrypted_groups, "For larger groups, create an
 * encrypted group.", else "Larger groups aren't available in this version
 * yet." (P4: nothing points to a missing feature). It adds nobody else. "Note to
 * Self" opens the account's own room. Start Conversation opens (or creates,
 * empty and accepted) the room with gh_conversation_store_open_room(),
 * emits "conversation-started" (GhConversation) and closes; nothing is sent
 * or published. A conversation the user blocked is unblocked by starting it,
 * and the confirm page says so first ("You blocked this conversation.
 * Starting it unblocks it: …"). Pending lookups are cancelled when the panel
 * leaves the window.
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
  /* Nullable: creates a Marmot DM (2-member MLS group with empty name) for
   * pubkey (hex). On success, the result is a GhConversation (transfer full).
   * When NULL, the panel always creates NIP-17 rooms. When the async call
   * fails, the panel falls back to NIP-17 with honest copy. */
  void (*create_marmot_dm)(const gchar *pubkey, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data,
                           gpointer config_data);
  gpointer create_dm_data;
  /* Nullable: the consented profile picture of pubkey, borrowed (W32);
   * never a fetch without consent. gh_new_message_attach() supplies the
   * conversation list's cache when unset. */
  GdkPaintable *(*picture)(gpointer data, const gchar *pubkey);
  gpointer picture_data;
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
 * "subtitle", "avatar-text", "show-avatar", "show-initials", "picture", "icon-name",
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

#define GH_TYPE_NEW_MESSAGE_PAGE (gh_new_message_page_get_type())
G_DECLARE_FINAL_TYPE(GhNewMessagePage, gh_new_message_page, GH, NEW_MESSAGE_PAGE,
                     AdwNavigationPage)

GhNewMessagePage *gh_new_message_page_new(const GhNewMessageConfig *config);

/* The item models behind the pick page's rows, its chips and the confirm
 * page's rows (GhNewMessageItem), for tests and assistive checks. */
GListModel *gh_new_message_page_get_suggestions(GhNewMessagePage *self);
GListModel *gh_new_message_page_get_recipients(GhNewMessagePage *self);
GListModel *gh_new_message_page_get_people(GhNewMessagePage *self);

/* Chooses pubkey (hex) as if picked; FALSE when refused (own key, already
 * chosen, the limit). */
gboolean gh_new_message_page_add_recipient(GhNewMessagePage *self, const gchar *pubkey);
/* Moves keyboard focus to the search entry (unless the confirm page shows). */
void gh_new_message_page_focus_entry(GhNewMessagePage *self);
/* Leaves the panel: cancels what it asked for and pops it (and anything over
 * it) from the navigation view it was pushed onto. */
void gh_new_message_page_close(GhNewMessagePage *self);
/* Refreshes pubkey's picture in the rows (the cache finished loading it). */
void gh_new_message_page_update_picture(GhNewMessagePage *self, const gchar *pubkey);

/* Makes win.new-message (Ctrl+N, the sidebar's New Message buttons) of
 * window slide its sidebar to a GhNewMessagePage made with config (copied;
 * its objects are referenced until the window is destroyed), or focus the
 * one already open; win.message-uri opens one with the link's person
 * chosen. The action is enabled while config->conversations has an
 * account. A started conversation opens in the window (selected and shown)
 * with keyboard focus in the composer when sending is possible, else in the
 * conversation (gh_content_page_focus_conversation()). */
void gh_new_message_attach(GhWindow *window, const GhNewMessageConfig *config);

G_END_DECLS
#endif
