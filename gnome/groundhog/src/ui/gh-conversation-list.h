#ifndef GH_CONVERSATION_LIST_H
#define GH_CONVERSATION_LIST_H

#include "gh-conversation-store.h"
#include "gh-window.h"

G_BEGIN_DECLS

/* Binds store (the active account's NIP-17 conversations) to window
 * (charter §7.5):
 *  - the sidebar lists the conversations that are not message requests in
 *    the store's order (newest activity first) and counts the requests in its
 *    Message Requests entry, both filtered by the search text, which matches
 *    the title and the participants' full npubs (profile names are never
 *    fetched for this);
 *  - rows are GhConversationRow; they show message previews only while the
 *    sidebar's show-previews is set, which follows the settings key
 *    show-message-previews when settings has it and stays off otherwise;
 *  - the selected conversation's messages fill the content page (plain,
 *    read-only rows from gh-message-item.blp) and are marked read locally
 *    once they are on screen, as are messages arriving while they are on
 *    screen in the active window.
 * Nothing here publishes anything. settings may be NULL. Everything is
 * released with window. */
void gh_conversation_list_attach(GhWindow *window, GhConversationStore *store,
                                 GSettings *settings);

/* The settings key that governs list previews (owned by the schema, charter
 * §7.11); absent from schemas that predate it. */
#define GH_CONVERSATION_LIST_PREVIEWS_KEY "show-message-previews"

/* The message list's text for message: "You · 10:42" or the sender's
 * abbreviated npub and time, and the list item's accessible label, e.g.
 * "You, 10:42: Hello" (charter §7.14). Exposed for tests. */
gchar *gh_conversation_list_message_heading(GhMessage *message, GDateTime *now);
gchar *gh_conversation_list_message_label(GhMessage *message, GDateTime *now);

G_END_DECLS
#endif
