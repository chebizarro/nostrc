#ifndef GH_CONVERSATION_MENU_H
#define GH_CONVERSATION_MENU_H

#include "gh-conversation-info-dialog.h"

G_BEGIN_DECLS

/*
 * The actions behind a conversation row's context menu (privacy charter
 * §7.4, §7.13; nostrc-qp24.74; the menu itself is GhConversationRow's).
 * Each takes the room id (s) of a private (NIP-17) conversation listed in
 * the services' model, and does nothing for one that is not (any more):
 *
 *   win.show-conversation-info  Conversation Info (GhConversationInfoDialog);
 *   win.mute-conversation       asks for how long (data/ui/gh-conversation-
 *                               menu.blp mute_dialog; with Unmute when it is
 *                               muted), then gh_conversation_actions_mute();
 *   win.delete-conversation     asks (delete_dialog), then deletes it from
 *                               this device: gh_conversation_actions_forget()
 *                               (ST-9; the others keep their copies).
 *
 * services_func is the one gh_conversation_info_attach() takes; it is asked
 * when an action runs and again when its confirmation is answered. Mute and
 * delete need the account's open store; without it they say so in a toast.
 * Nothing is published and nobody is told (P8). Shift+F10 and Menu open the
 * context menu of the list's focused row. user_data is released with
 * destroy together with window.
 */
void gh_conversation_menu_attach(GhWindow *window, GhConversationInfoServicesFunc services_func,
                                 gpointer user_data, GDestroyNotify destroy);

/* The text of the last toast these actions showed (NULL before one), for
 * tests. */
const gchar *gh_conversation_menu_get_last_toast(GhWindow *window);

G_END_DECLS
#endif
