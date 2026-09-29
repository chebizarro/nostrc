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
 *  - the content page shows the selected conversation in a
 *    GhConversationView (charter G12), which it installs and which takes
 *    settings for link previews; its messages are marked read locally once
 *    they are on screen, as are messages arriving while they are on screen
 *    in the active window. The view is gh_content_page_get_view() of the
 *    window's content page; its retry, unlock, history and delivery hooks
 *    are wired by their owners (gh-conversation-view.h).
 * Nothing here publishes anything. settings may be NULL. Everything is
 * released with window. */
void gh_conversation_list_attach(GhWindow *window, GhConversationStore *store,
                                 GSettings *settings);

/* The settings key that governs list previews (owned by the schema, charter
 * §7.11); absent from schemas that predate it. */
#define GH_CONVERSATION_LIST_PREVIEWS_KEY "show-message-previews"

G_END_DECLS
#endif
