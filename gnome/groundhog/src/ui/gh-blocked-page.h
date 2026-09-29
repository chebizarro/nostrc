#ifndef GH_BLOCKED_PAGE_H
#define GH_BLOCKED_PAGE_H

#include <adwaita.h>
#include "gh-preferences-dialog.h"

G_BEGIN_DECLS

/*
 * Blocked Conversations (nostrc-qp24.72; privacy charter §7.9 "Block",
 * §7.11): Preferences › Privacy lists the conversations blocked on this
 * device, each with Unblock. GTK-only: the list and the unblock come from a
 * GhBlockedBackend, in the application the account's encrypted store
 * (gh_store_conversations_list_blocked() and, through
 * gh_conversation_actions_block(), gh_store_conversations_set_blocked(),
 * which kept the history). People are shown by npub only: nothing is looked
 * up for them (PT-8), and nothing is published (P8).
 */

/* One blocked conversation. */
typedef struct {
  gchar *room_id;
  GStrv npubs;           /* the other people, as npubs */
  gboolean has_messages; /* FALSE: its messages were deleted when it was blocked */
} GhBlockedConversation;

void gh_blocked_conversation_free(GhBlockedConversation *conversation);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhBlockedConversation, gh_blocked_conversation_free)

typedef struct {
  /* GhBlockedConversation array, newest first; NULL with error when it can't
   * be read (e.g. no account or its storage is locked). */
  GPtrArray *(*list)(gpointer data, GError **error);
  gboolean (*unblock)(gpointer data, const gchar *room_id, GError **error);
} GhBlockedBackend;

#define GH_TYPE_BLOCKED_PAGE (gh_blocked_page_get_type())
G_DECLARE_FINAL_TYPE(GhBlockedPage, gh_blocked_page, GH, BLOCKED_PAGE, AdwNavigationPage)

/* A page over backend (copied) and data (borrowed: it must outlive the
 * page); it lists at once. */
GhBlockedPage *gh_blocked_page_new(const GhBlockedBackend *backend, gpointer data);
/* Lists again. */
void gh_blocked_page_refresh(GhBlockedPage *self);
/* "list", "empty" or "error". */
const gchar *gh_blocked_page_get_state(GhBlockedPage *self);
GtkListBox *gh_blocked_page_get_list(GhBlockedPage *self);

/* Shows dialog's Privacy › Blocked Conversations row, which pushes a
 * GhBlockedPage over backend and data. data is released with destroy
 * together with dialog. */
void gh_blocked_page_attach(GhPreferencesDialog *dialog, const GhBlockedBackend *backend,
                            gpointer data, GDestroyNotify destroy);

G_END_DECLS
#endif
