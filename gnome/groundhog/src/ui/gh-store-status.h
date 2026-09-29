#ifndef GH_STORE_STATUS_H
#define GH_STORE_STATUS_H

#include "gh-account-store.h"
#include "gh-status.h"
#include "gh-window.h"

G_BEGIN_DECLS

/* The active account's message store as the window needs it (charter §3.4,
 * §7.6, §7.15 #14-#16). */

/* The status input: each GhAccountStore state maps onto one GhStatusStore. */
GhStatusStore gh_store_status_map(GhAccountStoreState state);

/* Keeps status's store input (and its explanation) in step with store until
 * either is finalized. */
void gh_store_status_attach(GhStatus *status, GhAccountStore *store);

/* Older history for window's conversation view (W13b review B1): each older
 * page of the shown room is listed from store's open store,
 * GH_STORE_CONVERSATIONS_PAGE_SIZE messages at a time
 * (gh_account_store_load_older()); while no store is open (or once store is
 * gone) the view says the earlier messages couldn't be loaded. Needs
 * gh_conversation_list_attach() first. store is not referenced. */
void gh_store_status_attach_history(GhWindow *window, GhAccountStore *store);

/* "Start Fresh on This Device…" (KEY_MISSING) and "Reset Storage…" (CORRUPT),
 * the store banners' GH_STATUS_ACTION_STORE_START_FRESH: an AdwAlertDialog
 * over parent explains what is deleted and what can be downloaded again
 * (charter §3.4), and only its destructive response runs
 * gh_account_store_start_fresh_async(); the KEY_MISSING one also offers Try
 * Again. The outcome is a toast when parent is a GhWindow. Does nothing in
 * any other state. Returns the dialog (borrowed, NULL when none was shown). */
AdwAlertDialog *gh_store_status_confirm_start_fresh(GtkWidget *parent, GhAccountStore *store);

G_END_DECLS
#endif
