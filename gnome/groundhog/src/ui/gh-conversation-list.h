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
 *    window's content page; its retry, unlock and delivery hooks are wired
 *    by their owners (gh-conversation-view.h), its older history through
 *    gh_conversation_list_set_history_source().
 * Nothing here publishes anything. settings may be NULL. Everything is
 * released with window. */
void gh_conversation_list_attach(GhWindow *window, GhConversationStore *store,
                                 GSettings *settings);

/* Lists the next page of older stored messages of conversation into the
 * model (e.g. gh_account_store_load_older()); FALSE with error when they
 * can't be listed, e.g. while no message store is open. */
typedef gboolean (*GhConversationListLoadOlder)(GhConversation *conversation, GError **error,
                                                gpointer user_data);
/* Where the shown conversation's older history comes from (charter §7.6;
 * W13b review B1), for a window gh_conversation_list_attach() bound. When
 * the view asks (scrolling near the top, or "Earlier Messages"), the page is
 * listed from an idle, the view finishes loading (or says it couldn't), and
 * the room is marked read again if it is on screen: what the page listed is
 * read, unread messages still unloaded stay unread. NULL removes it (the
 * view then says earlier messages can't be shown). user_data is released
 * with destroy when replaced or with window. */
void gh_conversation_list_set_history_source(GhWindow *window,
                                             GhConversationListLoadOlder load_older,
                                             gpointer user_data, GDestroyNotify destroy);

/* The settings key that governs list previews (owned by the schema, charter
 * §7.11); absent from schemas that predate it. */
#define GH_CONVERSATION_LIST_PREVIEWS_KEY "show-message-previews"

G_END_DECLS
#endif
