#ifndef GH_APP_SERVICES_H
#define GH_APP_SERVICES_H

#include <adwaita.h>

#include "gh-window.h"

G_BEGIN_DECLS

/*
 * GhAppServices: the process-owned services of one Groundhog process (privacy
 * charter §8.2 G04, §8.4). They outlive windows: the active account, its
 * relay lists, the conversation model, the NIP-17 inbox, the store key
 * custody and the account's encrypted store with its outbox.
 *
 * Every service is one init/teardown pair in a single ordered table
 * (gh-app-services.c): init runs top to bottom at application startup,
 * teardown bottom to top at shutdown, so a service is always torn down
 * before anything it uses. Later items add their pair to the table instead
 * of editing src/main.c.
 *
 * Which services exist depends on the build (the GROUNDHOG_HAVE_* feature
 * macros of the executable); the getters return NULL for absent ones. With
 * the encrypted store, the inbox runs only while the account's store is open
 * or the user chose "Continue Without Saving Messages" (gh-account-store.h).
 * A build without SQLCipher and libsecret keeps the pre-store receive path:
 * messages in memory, the seen-set in GhDmInbox's own state file.
 */

typedef struct _GhAppServices GhAppServices;

/* Initializes every service for app (a GtkApplication; its D-Bus connection
 * reaches the signer and the Secret Service) and installs the application
 * actions the store banners activate (GH_STATUS_ACTION_STORE_*). NULL with
 * error if a service could not start (e.g. the settings schema is missing). */
GhAppServices *gh_app_services_new(GtkApplication *app, GError **error);
/* Tears every service down in reverse order and removes the actions. */
void gh_app_services_free(GhAppServices *self);

/* Attaches the window's account pages, conversation list and status inputs
 * (account, inbox, store) to the services; released with the window. */
void gh_app_services_attach_window(GhAppServices *self, GhWindow *window);

/* Borrowed; NULL when the build or the platform lacks the service. The
 * account store is a GhAccountStore (gh-account-store.h). */
GSettings *gh_app_services_get_settings(GhAppServices *self);
GObject *gh_app_services_get_accounts(GhAppServices *self);
GObject *gh_app_services_get_conversations(GhAppServices *self);
GObject *gh_app_services_get_inbox(GhAppServices *self);
GObject *gh_app_services_get_account_store(GhAppServices *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhAppServices, gh_app_services_free)

G_END_DECLS
#endif
