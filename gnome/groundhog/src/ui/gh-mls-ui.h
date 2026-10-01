#ifndef GH_MLS_UI_H
#define GH_MLS_UI_H

#include "gh-account-relays.h"
#include "gh-group-ui.h"
#include "gh-mls-context.h"
#include "gh-window.h"

G_BEGIN_DECLS

/*
 * The window's encrypted groups (Marmot MLS; privacy charter §7.5, §7.6,
 * §7.7, §7.9, §7.10, §7.15 #13; nostrc-9xf5, qp24.13 part 2), over the open
 * store's GhMlsService (which follows the account store: none while no store
 * is open). The application attaches it only while
 * GH_FEATURE_ENCRYPTED_GROUPS is on (gh-features.h); the tests attach it
 * directly.
 *
 *  - New Group (gh_mls_ui_extend_new_group(), a GhGroupUiNewGroupFunc):
 *    adds the "Encrypted Group" page (GhMlsNewGroupPage) to each dialog while
 *    a service exists; Open Group opens its conversation.
 *  - Invitations: the sidebar's "Group Invitations · N" entry
 *    (gh_sidebar_page_set_invitations()) and win.group-invitations present
 *    GhMlsInvitesDialog; a new invitation is a toast with View, never a
 *    notification with a name (PD-8). Nothing joins before Accept.
 *  - The composer (gh_send_ui_add_delegate()): an encrypted group's messages
 *    go to gh_mls_service_send() (stored and ratcheted in one transaction,
 *    listed at once, status honest: SENT once a group relay accepted it; a
 *    restart republishes), never the NIP-17 outbox; the disabled reason is
 *    gh_mls_send_reason() and follows the group (left, removed by whom,
 *    offline; nostrc-xrya).
 *  - Group Info: gh_mls_ui_show_info() (GhMlsGroupInfoDialog), with the
 *    group picture when config->files is set.
 *  - The conversation header's "Encrypted group · N members"
 *    (gh_conversation_list_set_member_count_func(), refreshed on
 *    "members-changed") and the view's "Some messages in this group can't be
 *    read yet" (GhMlsGroup:decrypt-pending of the shown group; no number
 *    and no cause: held events may be messages, group changes or junk,
 *    nostrc-oya4, W22 review N4).
 * Nothing here contacts the network before the user acts. Everything is
 * released with window.
 */

typedef GhMlsService *(*GhMlsUiServiceFunc)(gpointer user_data);

typedef struct {
  GhConversationStore *conversations; /* required: the window's model */
  GhAccountController *accounts;      /* required: KeyPackage checks */
  GSettings *settings;                /* nullable: discovery-relays */
  GhMlsUiServiceFunc service;         /* required: the open store's, or NULL */
  gpointer service_data;
  /* Nullable: emits "changed" when the service may have changed (the
   * GhAccountStore). */
  GObject *state_source;
  GhMlsNameFunc display_name;         /* nullable: cached names */
  gpointer names_data;
  /* Nullable: the account's own write relays start a new group's relays. */
  GhAccountRelays *account_relays;
  guint lookup_deadline;              /* 0: the lookup default */
  /* Nullable (W25): the window's encrypted-group files; Group Info shows
   * and changes the group picture with it. */
  struct _GhMlsAttachments *files;
} GhMlsUiConfig;

/* After gh_conversation_list_attach(), gh_send_ui_attach() and
 * gh_group_ui_attach() (it adds a composer delegate beside the relay
 * groups' and extends New Group). */
void gh_mls_ui_attach(GhWindow *window, const GhMlsUiConfig *config);

/* Whether this build runs encrypted groups: GH_FEATURE_ENCRYPTED_GROUPS
 * (gh-features.h) as compiled into this file, never a test's choice. */
gboolean gh_mls_ui_enabled(void);
/* The application's attach, and the flag's guard: gh_mls_ui_attach() when
 * gh_mls_ui_enabled(), else nothing (FALSE), so New Group opens on the relay
 * form with no chooser and no disabled encrypted placeholder (charter
 * §7.9). */
gboolean gh_mls_ui_attach_if_enabled(GhWindow *window, const GhMlsUiConfig *config);

/* A GhConversationInfoGroupFunc for encrypted groups: presents Group Info
 * for conversation; FALSE when it is not a group of the window's service. */
gboolean gh_mls_ui_show_info(GhWindow *window, GhConversation *conversation,
                             gpointer user_data);

/* A GhGroupUiNewGroupFunc: adds the encrypted page to dialog (nothing
 * without a service). gh_mls_ui_attach() installs it. */
void gh_mls_ui_extend_new_group(GhWindow *window, GhNewGroupDialog *dialog, gpointer user_data);

/* The window's current service, for tests (NULL: none). */
GhMlsService *gh_mls_ui_get_service(GhWindow *window);

G_END_DECLS
#endif
