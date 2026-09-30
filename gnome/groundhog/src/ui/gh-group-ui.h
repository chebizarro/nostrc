#ifndef GH_GROUP_UI_H
#define GH_GROUP_UI_H

#include "gh-conversation-list.h"
#include "gh-group-info-dialog.h"
#include "gh-new-group-dialog.h"
#include "gh-window.h"

G_BEGIN_DECLS

/*
 * The window's NIP-29 relay groups (privacy charter §7.5, §7.7, §7.9, §7.10;
 * item G20b), over the open store's GhNip29Service (which follows the
 * account store: none while no store is open):
 *
 *  - win.join-group and win.new-group (the main menu) present
 *    GhGroupJoinDialog and GhNewGroupDialog; each is enabled while a service
 *    exists, and "Open Group" opens the group's conversation in the window.
 *  - The composer (gh_send_ui_set_delegate()): a relay group's messages go
 *    to gh_nip29_service_send() (the durable NIP-29 outbox: listed at once,
 *    honest status), never the NIP-17 outbox; Retry is the outbox's retry;
 *    the delivery details name the group's relay and quote its answer; the
 *    disabled reason is gh_group_room_send_reason() ("closed to you" when
 *    the group declined, is invite only, or removed the account) and follows
 *    the rooms' join state.
 *  - Group Info: gh_group_ui_show_info() is the conversation-info group
 *    handler (gh_conversation_info_set_group_handler()).
 *  - Older history (charter §7.6): a relay group's older stored messages come
 *    from gh_nip29_service_load_older(); other rooms' from load_older.
 *    (Pages the relay never sent are nostrc-x055.)
 * Nothing here contacts the network before the user acts in a dialog or the
 * composer. Everything is released with window.
 */

typedef GhNip29Service *(*GhGroupUiServiceFunc)(gpointer user_data);

typedef struct {
  GhConversationStore *conversations; /* required: the window's model */
  GhGroupUiServiceFunc service;       /* required: the open store's, or NULL */
  gpointer service_data;
  /* Nullable: emits "changed" when the service may have changed (the
   * GhAccountStore). */
  GObject *state_source;
  GhGroupNameFunc display_name;       /* nullable: cached names for Group Info */
  gpointer names_data;
  /* The other rooms' older history (e.g. the account store's), or NULL. */
  GhConversationListLoadOlder load_older;
  gpointer load_older_data;
} GhGroupUiConfig;

/* After gh_conversation_list_attach() and, when present, gh_send_ui_attach()
 * (it replaces the window's history source). */
void gh_group_ui_attach(GhWindow *window, const GhGroupUiConfig *config);

/* qp24.13 part 2: called with each New Group dialog before it is presented,
 * e.g. to add the encrypted page (gh_new_group_dialog_add_encrypted_page()).
 * NULL clears; user_data is borrowed until replaced or the window goes. */
typedef void (*GhGroupUiNewGroupFunc)(GhWindow *window, GhNewGroupDialog *dialog,
                                      gpointer user_data);
void gh_group_ui_set_new_group_extension(GhWindow *window, GhGroupUiNewGroupFunc func,
                                         gpointer user_data);

/* A GhConversationInfoGroupFunc: presents Group Info for conversation (a
 * relay group of the window's service). FALSE when there is none. */
gboolean gh_group_ui_show_info(GhWindow *window, GhConversation *conversation,
                               gpointer user_data);

G_END_DECLS
#endif
