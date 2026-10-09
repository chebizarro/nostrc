#include "gh-about-dialog.h"
#if GROUNDHOG_HAVE_ISSUE
#include "gh-issue-dialog.h"
#endif
#include "gh-app-services.h"
#include "gh-diagnostics.h"

#if GROUNDHOG_HAVE_ACCOUNTS
#include "gh-account-controller.h"
#include "gh-account-ui.h"
#include "gh-nip46-credentials.h"
#include "gh-nip46-auth-url.h"
#endif
#if GROUNDHOG_HAVE_RELAYS
#include "gh-account-relays.h"
#include "gh-inbox-setup.h"
#endif
#if GROUNDHOG_HAVE_INBOX
#include "gh-conversation-list.h"
#include "gh-display-name.h"
#include "gh-nip04-inbox.h"
#include "gh-picture-cache.h"
#if GROUNDHOG_HAVE_CONVERSATION_INFO
#include "gh-store-contacts.h"
#endif
#include "gh-dm-inbox.h"
#include "gh-inbox-status.h"
#endif
#if GROUNDHOG_HAVE_ACCOUNT_STORE
#include "gh-account-store.h"
#include "gh-store-key.h"
#include "gh-store-status.h"
#endif
#if GROUNDHOG_HAVE_CONVERSATION_INFO
#include "gh-blocked-conversations.h"
#include "gh-conversation-info-dialog.h"
#include "gh-conversation-menu.h"
#endif
#if GROUNDHOG_HAVE_OUTBOX
#include "gh-app-outbox.h"
#include "gh-contact-titles.h"
#include "gh-outbox.h"
#include "gh-reaction.h"
#include "gh-reaction-store.h"
#include "gh-send-ui.h"
#include "gh-conversation-view.h"
#endif
#include <glib/gi18n.h>
#if GROUNDHOG_HAVE_EXPIRY
#include "gh-expiry.h"
#endif
#if GROUNDHOG_HAVE_BACKGROUND
#include "gh-background.h"
#endif
#if GROUNDHOG_HAVE_ONBOARDING
#include "gh-onboarding-view.h"
#endif
#if GROUNDHOG_HAVE_RELAYS
#include "gh-relay-list-setup.h"
#include <nostr-event.h>
#endif
#if GROUNDHOG_HAVE_NOTIFIER
#include "gh-notifier.h"
#endif
#if GROUNDHOG_HAVE_NEW_MESSAGE
#include "gh-new-message-dialog.h"
#include "gh-add-contact-dialog.h"
#endif

/* G20b: NIP-29 relay groups (gh-group-ui.h). */
#ifndef GROUNDHOG_HAVE_GROUP_UI
#define GROUNDHOG_HAVE_GROUP_UI 0
#endif
#if GROUNDHOG_HAVE_GROUP_UI
#include "gh-group-ui.h"
#endif
#ifndef GROUNDHOG_HAVE_MLS_UI
#define GROUNDHOG_HAVE_MLS_UI 0
#endif
#if GROUNDHOG_HAVE_MLS_UI
#include "gh-mls-ui.h"
#include "gh-poll-ui.h"
#endif

#if GROUNDHOG_HAVE_ACCOUNTS
#include "gh-features.h"
#include "gh-identity.h"
#include "gh-preferences-dialog.h"
#endif

/* G22: encrypted attachments (gh-attachments.h, gh-attachment-ui.h). */
#ifndef GROUNDHOG_HAVE_ATTACHMENTS
#define GROUNDHOG_HAVE_ATTACHMENTS 0
#endif
#if GROUNDHOG_HAVE_ATTACHMENTS
#include "gh-attachment-ui.h"
#include "gh-attachments.h"
#include "gh-net-http.h"
#endif
/* W25: files and pictures in encrypted groups (gh-mls-attachments.h,
 * gh-mls-attachment-ui.h); built with both the attachments and the MLS UI. */
#ifndef GROUNDHOG_HAVE_MLS_FILES
#define GROUNDHOG_HAVE_MLS_FILES 0
#endif
#if GROUNDHOG_HAVE_MLS_FILES
#include "gh-mls-attachment-ui.h"
#include "gh-mls-attachments.h"
#endif
#if GROUNDHOG_HAVE_ACCOUNTS && GROUNDHOG_HAVE_TOR
#include "gh-net-session.h"
#include "gh-relay-net.h"
#endif

/* A build without G09 but with the relay layer: the network-mode guard
 * (gh-relay-guard.h, nostrc-6v0i). */
#ifndef GROUNDHOG_HAVE_RELAY_GUARD
#define GROUNDHOG_HAVE_RELAY_GUARD 0
#endif
#define GH_APP_RELAY_GUARD \
  (GROUNDHOG_HAVE_ACCOUNTS && !GROUNDHOG_HAVE_TOR && GROUNDHOG_HAVE_RELAY_GUARD)
#if GH_APP_RELAY_GUARD
#include "gh-relay-guard.h"
#endif

#define GROUNDHOG_APP_ID "org.nostr.Groundhog"

struct _GhAppServices {
  GhNip04Inbox *nip04;  /* W33: older NIP-04 DMs, read-only */
  GtkApplication *app; /* borrowed: owns the process's services */
  GSettings *settings;
  GhDiagnostics *diagnostics;
#if GROUNDHOG_HAVE_ACCOUNTS && GROUNDHOG_HAVE_TOR
  GhNetSession *network; /* the network mode of every connection (G09) */
#endif
#if GROUNDHOG_HAVE_ACCOUNTS
  GhNip46CredentialStore *nip46_credentials;
  GhAccountController *accounts;
  GhNip46AuthUrl *nip46_auth_url;
  guint64 nip46_auth_generation;
  gboolean nip46_auth_pending;
#endif
#if GROUNDHOG_HAVE_RELAYS
  GhAccountRelays *relays;
#endif
#if GROUNDHOG_HAVE_OUTBOX
  GhAppOutbox *outbox;
#endif
#if GROUNDHOG_HAVE_INBOX
  GhConversationStore *conversations;
  GhDmInbox *inbox;
#endif
#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_OUTBOX
  GhContactTitles *contact_titles; /* names from the directory's cache (qp24.66) */
#endif
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  GhStoreKey *store_key;
  GhAccountStore *account_store;
  GSimpleAction *store_actions[4];
#endif
#if GROUNDHOG_HAVE_EXPIRY
  GhExpiry *expiry; /* the open store's, while there is one */
#endif
#if GROUNDHOG_HAVE_ATTACHMENTS
  GhNetHttp *attachments_http;  /* attachment transfers, in the network mode */
  GhAttachments *attachments;   /* the open store's files (G22) */
#endif
#if GROUNDHOG_HAVE_MLS_FILES
  GhMlsAttachments *mls_files;  /* the open store's encrypted groups' files (W25) */
#endif
#if GROUNDHOG_HAVE_NOTIFIER
  GhNotifier *notifier;
#endif
#if GROUNDHOG_HAVE_BACKGROUND
  GhBackground *background;
#endif
#if GROUNDHOG_HAVE_NEW_MESSAGE
  GhNip05 *nip05; /* New Message's address lookups (G18) */
#endif
#if GROUNDHOG_HAVE_ACCOUNTS
  GSimpleAction *preferences_action;
  GSimpleAction *about_action;
  GSimpleAction *network_settings_action;
  GhPreferencesDialog *preferences_dialog; /* weak: the one that is open */
#endif
  guint started; /* services initialized, from the top of the table */
};

/* ---- services ---------------------------------------------------------------
 * One init/teardown pair each. Teardown is the exact reverse of init. */

static void
dispose_object(gpointer object_ptr)
{
  GObject **object = object_ptr;
  if (*object)
    g_object_run_dispose(*object);
  g_clear_object(object);
}

#if GROUNDHOG_HAVE_ACCOUNTS
static gboolean
settings_init(GhAppServices *self, GError **error)
{
  (void)error;
  self->settings = g_settings_new(GROUNDHOG_APP_ID);
  return TRUE;
}

static void
settings_teardown(GhAppServices *self)
{
  g_clear_object(&self->settings);
}

static gboolean
diagnostics_init(GhAppServices *self, GError **error)
{
  (void)error;
  self->diagnostics = gh_diagnostics_new(self->settings, NULL);
  gh_diagnostics_set_default(self->diagnostics);
  return self->diagnostics != NULL;
}

static void
diagnostics_teardown(GhAppServices *self)
{
  gh_diagnostics_set_default(NULL);
  gh_diagnostics_free(self->diagnostics);
  self->diagnostics = NULL;
}

static void
on_account_auth_changed(GhAccountController *accounts, GhAppServices *self)
{
  if (self->nip46_auth_url && self->nip46_auth_pending &&
      self->nip46_auth_generation != gh_account_controller_get_generation(accounts)) {
    gh_nip46_auth_url_clear(self->nip46_auth_url);
    self->nip46_auth_pending = FALSE;
  }
}

static gboolean
on_account_auth_url(GhAccountController *accounts, const gchar *url,
                    GhAppServices *self)
{
  if (!self->nip46_auth_url) return FALSE;
  gboolean handled = gh_nip46_auth_url_handle(self->nip46_auth_url, url,
    gh_account_controller_get_cancellable(accounts));
  if (handled) {
    self->nip46_auth_generation = gh_account_controller_get_generation(accounts);
    self->nip46_auth_pending = TRUE;
  }
  return handled;
}

static void
on_account_auth_launch_failed(GhNip46AuthUrl *prompt, GError *error,
                              GhAppServices *self)
{
  (void)prompt; (void)self;
  g_warning("Could not open signer authorization page: %s", error->message);
}

/* The active account, its signer and its generation. Without a session bus
 * the signer is reported unreachable, not faked. */
static gboolean
accounts_init(GhAppServices *self, GError **error)
{
  (void)error;
#ifdef GH_MLS_TEST_HOOKS
  if (g_strcmp0(g_getenv("GH_TEST_CONTROL"), "1") == 0)
    self->nip46_credentials = gh_nip46_credential_store_new_secret_service();
  else
#endif
    self->nip46_credentials = gh_nip46_credential_store_new();
  self->accounts = gh_account_controller_new_with_credentials(self->settings,
    g_application_get_dbus_connection(G_APPLICATION(self->app)),
    self->nip46_credentials);
  self->nip46_auth_url = gh_nip46_auth_url_new(self->app, NULL);
  g_signal_connect(self->accounts, "auth-url", G_CALLBACK(on_account_auth_url), self);
  g_signal_connect(self->accounts, "changed", G_CALLBACK(on_account_auth_changed), self);
  g_signal_connect(self->nip46_auth_url, "launch-failed",
                   G_CALLBACK(on_account_auth_launch_failed), self);
  return TRUE;
}

/* Revokes the account generation even if a listing is still in flight. */
static void
accounts_teardown(GhAppServices *self)
{
  dispose_object(&self->accounts);
  dispose_object(&self->nip46_auth_url);
  dispose_object(&self->nip46_credentials);
}
#endif

#if GROUNDHOG_HAVE_ACCOUNTS && GROUNDHOG_HAVE_TOR
/* The network mode (privacy charter §4.2, G09), installed before any relay
 * scope or publish exists: from here on every one of them goes through the
 * network-mode dispatcher (gh-relay-net.h), so Tor mode never connects
 * directly, and a mode change closes every connection of the old mode. */
static gboolean
network_init(GhAppServices *self, GError **error)
{
  (void)error;
  self->network = gh_net_session_new(self->settings);
  gh_relay_net_install(self->network);
  return TRUE;
}

/* After every service that connects has stopped. */
static void
network_teardown(GhAppServices *self)
{
  gh_relay_net_install(NULL);
  dispose_object(&self->network);
}

/* "Can't reach Tor — Groundhog won't connect without it" (§7.15 #6). */
static void
sync_tor_banner(GhNetSession *network, GParamSpec *pspec, gpointer status)
{
  (void)pspec;
  gh_status_set_tor_unreachable(GH_STATUS(status),
                                gh_net_session_get_tor_state(network) == GH_NET_TOR_UNREACHABLE);
}

static void
sync_preferences_tor(GhNetSession *network, GParamSpec *pspec, gpointer dialog)
{
  (void)pspec;
  static const GhPreferencesTorStatus status[] = {
    [GH_NET_TOR_OFF] = GH_PREFERENCES_TOR_STATUS_UNKNOWN,
    [GH_NET_TOR_CHECKING] = GH_PREFERENCES_TOR_STATUS_CHECKING,
    [GH_NET_TOR_READY] = GH_PREFERENCES_TOR_STATUS_REACHABLE,
    [GH_NET_TOR_UNREACHABLE] = GH_PREFERENCES_TOR_STATUS_UNREACHABLE,
  };
  gh_preferences_dialog_set_tor_status(GH_PREFERENCES_DIALOG(dialog),
                                       status[gh_net_session_get_tor_state(network)]);
}
#endif

#if GH_APP_RELAY_GUARD
/* Without G09 (charter P5): installed before any relay scope or publish
 * exists, so network-mode tor, or a mode this build does not know, never
 * connects directly; a change to it closes every connection. */
static gboolean
guard_init(GhAppServices *self, GError **error)
{
  (void)error;
  gh_relay_guard_install(self->settings);
  return TRUE;
}

/* After every service that connects has stopped. */
static void
guard_teardown(GhAppServices *self)
{
  (void)self;
  gh_relay_guard_install(NULL);
}

/* "Tor isn't available in this build — Groundhog won't connect until you
 * choose another network setting". */
static void
sync_tor_unavailable(GSettings *settings, const gchar *key, gpointer status)
{
  (void)key;
  g_autofree gchar *mode = g_settings_get_string(settings, "network-mode");
  gh_status_set_tor_unavailable(GH_STATUS(status), !gh_relay_guard_mode_allowed(mode));
}
#endif

#if GROUNDHOG_HAVE_RELAYS
/* Follows the generation: the previous account's REQs close before the
 * next account's open. */
static gboolean
relays_init(GhAppServices *self, GError **error)
{
  (void)error;
  self->relays = gh_account_relays_new(self->accounts, self->settings, NULL, NULL);
  return TRUE;
}

static void
relays_teardown(GhAppServices *self)
{
  dispose_object(&self->relays);
}
#endif

#if GROUNDHOG_HAVE_OUTBOX
/* Recipient 10050 lookup and the NIP-17 sealer/publisher each account's
 * durable outbox drives. */
static gboolean
sender_init(GhAppServices *self, GError **error)
{
  (void)error;
  GhAppOutboxConfig config = {
    .accounts = self->accounts,
    .account_relays = self->relays,
    .settings = self->settings,
    /* Encrypted groups run only once their UI ships (qp24.13 part 2). */
    .encrypted_groups = GH_FEATURE_ENCRYPTED_GROUPS,
  };
  self->outbox = gh_app_outbox_new(&config);
  return TRUE;
}

static void
sender_teardown(GhAppServices *self)
{
  g_clear_pointer(&self->outbox, gh_app_outbox_free);
}
#endif

#if GROUNDHOG_HAVE_INBOX
static gboolean
conversations_init(GhAppServices *self, GError **error)
{
  (void)error;
  self->conversations = gh_conversation_store_new();
#if GROUNDHOG_HAVE_OUTBOX
  /* The contact directory refreshes the peers of accepted rooms only (G10). */
  gh_app_outbox_set_conversations(self->outbox, self->conversations);
  /* ... and their cached names title those rooms (nostrc-qp24.66). */
  GhContactDirectory *directory = gh_app_outbox_get_directory(self->outbox);
  if (directory)
    self->contact_titles = gh_contact_titles_new(self->conversations, directory);
#endif
  return TRUE;
}

static void
conversations_teardown(GhAppServices *self)
{
#if GROUNDHOG_HAVE_OUTBOX
  dispose_object(&self->contact_titles);
  gh_app_outbox_set_conversations(self->outbox, NULL);
#endif
  g_clear_object(&self->conversations);
}

/* Contacts nothing until an account is active and its own inbox list is
 * known. With the encrypted store it also waits for the account's store. */
static gboolean
inbox_init(GhAppServices *self, GError **error)
{
  (void)error;
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  self->inbox = gh_dm_inbox_new_with_storage(self->accounts, self->relays, self->conversations,
                                             NULL, NULL, NULL);
#else
  /* No encrypted store in this build: messages and their seen keys stay in
   * memory (a restart fetches them again); only rejected wrap ids are kept,
   * under a pseudonymous name in $XDG_STATE_HOME/groundhog/nip17. */
  self->inbox = gh_dm_inbox_new(self->accounts, self->relays, self->conversations, NULL, NULL,
                                NULL, NULL);
#endif
#if GROUNDHOG_HAVE_OUTBOX
  /* Welcomes arrive in the inbox; the MLS service takes them (qp24.13). */
  gh_app_outbox_set_inbox(self->outbox, G_OBJECT(self->inbox));
#endif
  /* W33: older NIP-04 DMs, read-only, while the NIP-17 inbox runs. */
  self->nip04 = gh_nip04_inbox_new(self->accounts, self->relays, self->conversations,
                                   self->inbox, NULL, NULL);
  return TRUE;
}

/* Its REQs and pending signer unwraps are cancelled while the relay lists
 * and the account generation they belong to still exist. */
static void
inbox_teardown(GhAppServices *self)
{
  if (self->nip04)
    g_object_run_dispose(G_OBJECT(self->nip04));
  g_clear_object(&self->nip04);
#if GROUNDHOG_HAVE_OUTBOX
  gh_app_outbox_set_inbox(self->outbox, NULL);
#endif
  dispose_object(&self->inbox);
}
#endif

#if GROUNDHOG_HAVE_ACCOUNT_STORE
/* One GhStoreKey per process (per-account operations are ordered in it). */
static gboolean
store_key_init(GhAppServices *self, GError **error)
{
  (void)error;
#ifdef GH_MLS_TEST_HOOKS
  /* The acceptance harness (tests/app/test_two_instance_acceptance.c) runs
   * this process against its own Secret Service on a private bus. On macOS
   * the platform default is the login Keychain, which a test must never
   * touch (and which cannot open the harness's store). */
  if (g_strcmp0(g_getenv("GH_TEST_CONTROL"), "1") == 0) {
    self->store_key = gh_store_key_new_secret_service();
    return TRUE;
  }
#endif
  self->store_key = gh_store_key_new(NULL);
  return TRUE;
}

static void
store_key_teardown(GhAppServices *self)
{
  dispose_object(&self->store_key);
}

static gboolean
account_store_init(GhAppServices *self, GError **error)
{
  (void)error;
  GhAccountStoreConfig config = {
    .accounts = self->accounts,
    .store_key = self->store_key,
    .conversations = self->conversations,
    .inbox = self->inbox,
    .settings = self->settings,
#if GROUNDHOG_HAVE_OUTBOX
    .create_outbox = gh_app_outbox_create,
    .outbox_data = self->outbox,
#endif
  };
  self->account_store = gh_account_store_new(&config);
  return TRUE;
}

/* Stops the inbox and the outbox, detaches the model and closes the store,
 * in that order. */
static void
account_store_teardown(GhAppServices *self)
{
  dispose_object(&self->account_store);
}

/* The store banners' buttons (GH_STATUS_ACTION_STORE_*). */
enum { ACTION_UNLOCK, ACTION_RETRY, ACTION_EPHEMERAL, ACTION_START_FRESH };
static const gchar *const store_action_names[] = {
  [ACTION_UNLOCK] = "store-unlock",
  [ACTION_RETRY] = "store-retry",
  [ACTION_EPHEMERAL] = "store-continue-without-saving",
  [ACTION_START_FRESH] = "store-start-fresh",
};

static void
sync_store_actions(GhAppServices *self)
{
  GhAccountStoreState state = gh_account_store_get_state(self->account_store);
  g_simple_action_set_enabled(self->store_actions[ACTION_UNLOCK],
                              state == GH_ACCOUNT_STORE_LOCKED);
  g_simple_action_set_enabled(self->store_actions[ACTION_RETRY],
                              state == GH_ACCOUNT_STORE_LOCKED ||
                              state == GH_ACCOUNT_STORE_UNAVAILABLE ||
                              state == GH_ACCOUNT_STORE_KEY_MISSING ||
                              state == GH_ACCOUNT_STORE_ERROR);
  g_simple_action_set_enabled(self->store_actions[ACTION_EPHEMERAL],
                              state == GH_ACCOUNT_STORE_UNAVAILABLE &&
                              gh_account_controller_get_active_backend(self->accounts) !=
                                GH_SIGNER_BACKEND_NIP46);
  g_simple_action_set_enabled(self->store_actions[ACTION_START_FRESH],
                              state == GH_ACCOUNT_STORE_KEY_MISSING ||
                              state == GH_ACCOUNT_STORE_CORRUPT);
}

static void
on_store_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GhAppServices *self = data;
  const gchar *name = g_action_get_name(G_ACTION(action));
  g_autoptr(GError) error = NULL;
  (void)parameter;
  if (g_str_equal(name, store_action_names[ACTION_UNLOCK]))
    gh_account_store_unlock(self->account_store);
  else if (g_str_equal(name, store_action_names[ACTION_RETRY]))
    gh_account_store_retry(self->account_store);
  else if (g_str_equal(name, store_action_names[ACTION_START_FRESH])) {
    /* Deletes messages: only after the dialog's confirmation. */
    GtkWindow *window = gtk_application_get_active_window(self->app);
    if (window)
      gh_store_status_confirm_start_fresh(GTK_WIDGET(window), self->account_store);
  } else if (!gh_account_store_continue_without_saving(self->account_store, &error))
    g_message("Groundhog could not keep messages in memory: %s", error->message);
}

static gboolean
store_actions_init(GhAppServices *self, GError **error)
{
  (void)error;
  for (guint i = 0; i < G_N_ELEMENTS(store_action_names); i++) {
    self->store_actions[i] = g_simple_action_new(store_action_names[i], NULL);
    g_signal_connect(self->store_actions[i], "activate", G_CALLBACK(on_store_action), self);
    g_action_map_add_action(G_ACTION_MAP(self->app), G_ACTION(self->store_actions[i]));
  }
  g_signal_connect_swapped(self->account_store, "changed", G_CALLBACK(sync_store_actions), self);
  sync_store_actions(self);
  return TRUE;
}

static void
store_actions_teardown(GhAppServices *self)
{
  g_signal_handlers_disconnect_by_func(self->account_store, sync_store_actions, self);
  for (guint i = 0; i < G_N_ELEMENTS(store_action_names); i++) {
    g_action_map_remove_action(G_ACTION_MAP(self->app), store_action_names[i]);
    g_clear_object(&self->store_actions[i]);
  }
}
#endif

#if GROUNDHOG_HAVE_EXPIRY
/* Disappearing messages and retention (G07): a GhExpiry for each open store
 * (saved, in memory or damaged), purging at open, on time and daily, and gone
 * as soon as that store closes. It is the one expiry timer: the conversation
 * view shows what the model holds. */
static gint64
expiry_default_timer(GhAppServices *self)
{
  return g_settings_get_int(self->settings, "default-disappearing-seconds");
}

/* The outbox lets go of entries the purge deleted with their messages. */
static void
expiry_purged(GhExpiry *expiry, const gchar *const *rumor_ids, guint n_outbox, gpointer data)
{
  GhAppServices *self = data;
  (void)expiry;
  (void)rumor_ids;
#if GROUNDHOG_HAVE_OUTBOX
  if (n_outbox > 0)
    gh_app_outbox_prune(gh_account_store_get_outbox(self->account_store));
#else
  (void)self;
  (void)n_outbox;
#endif
}

/* Every window's composer shows the shown conversation's timer before
 * sending (W14 review B1): each gets the current GhExpiry, or NULL before
 * it is disposed. */
static void
expiry_share(GhAppServices *self)
{
#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_OUTBOX
  for (GList *l = gtk_application_get_windows(self->app); l; l = l->next)
    if (GH_IS_WINDOW(l->data))
      gh_send_ui_set_expiry(GH_WINDOW(l->data), self->expiry);
#else
  (void)self;
#endif
}

static void
expiry_stop(GhAppServices *self)
{
  if (!self->expiry)
    return;
  GhExpiry *expiry = g_steal_pointer(&self->expiry);
  expiry_share(self); /* the windows let go of it first */
  g_object_run_dispose(G_OBJECT(expiry));
  g_object_unref(expiry);
}

static void
expiry_sync(GhAppServices *self)
{
  GhAccountStoreState state = gh_account_store_get_state(self->account_store);
  GhStore *store = gh_account_store_get_store(self->account_store);
  /* A damaged (read-only) store still gets one: expired messages must leave
   * the conversation view even when nothing can be deleted. */
  if (!store || (state != GH_ACCOUNT_STORE_OPEN && state != GH_ACCOUNT_STORE_EPHEMERAL &&
                 state != GH_ACCOUNT_STORE_CORRUPT)) {
    expiry_stop(self);
    return;
  }
  if (self->expiry)
    return;
  GhExpiryConfig config = {
    .store = store,
    .conversations = gh_account_store_get_conversations(self->account_store),
    .retention_days = g_settings_get_int(self->settings, "retention-days"),
    .default_timer = expiry_default_timer(self),
  };
  self->expiry = gh_expiry_new(&config);
  g_signal_connect(self->expiry, "purged", G_CALLBACK(expiry_purged), self);
  g_autoptr(GError) error = NULL;
  if (!gh_expiry_purge(self->expiry, &error))
    g_warning("Groundhog could not delete expired messages: %s", error->message);
  expiry_share(self);
}

static void
expiry_settings_changed(GSettings *settings, const gchar *key, gpointer data)
{
  GhAppServices *self = data;
  if (!self->expiry)
    return;
  if (g_str_equal(key, "retention-days"))
    gh_expiry_set_retention_days(self->expiry, g_settings_get_int(settings, key));
  else if (g_str_equal(key, "default-disappearing-seconds")) {
    gh_expiry_set_default_timer(self->expiry, expiry_default_timer(self));
    expiry_share(self); /* a conversation not stored yet shows the default */
  }
}

static gboolean
expiry_init(GhAppServices *self, GError **error)
{
  (void)error;
  g_signal_connect_swapped(self->account_store, "changed", G_CALLBACK(expiry_sync), self);
  /* Right after the close, before any other store can open. */
  g_signal_connect_swapped(self->account_store, "store-closed", G_CALLBACK(expiry_stop), self);
  g_signal_connect(self->settings, "changed::retention-days",
                   G_CALLBACK(expiry_settings_changed), self);
  g_signal_connect(self->settings, "changed::default-disappearing-seconds",
                   G_CALLBACK(expiry_settings_changed), self);
  expiry_sync(self);
  return TRUE;
}

static void
expiry_teardown(GhAppServices *self)
{
  g_signal_handlers_disconnect_by_func(self->account_store, expiry_sync, self);
  g_signal_handlers_disconnect_by_func(self->account_store, expiry_stop, self);
  g_signal_handlers_disconnect_by_func(self->settings, expiry_settings_changed, self);
  expiry_stop(self);
}
#endif

/* W26 slice B (nostrc-191r): reaction chips on message bubbles. Push the
 * current reaction store from the outbox to every window's conversation
 * view. The store follows the account store lifecycle (created on open,
 * freed on close). */
#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_OUTBOX
static void on_react(GhConversation *, GhMessage *, const gchar *, gboolean, gpointer);

static void
reactions_share(GhAppServices *self)
{
  GhReactionStore *reactions = gh_app_outbox_get_reactions(self->outbox);
  for (GList *l = gtk_application_get_windows(self->app); l; l = l->next) {
    if (!GH_IS_WINDOW(l->data))
      continue;
    GhContentPage *content = gh_window_get_content(GH_WINDOW(l->data));
    GtkWidget *view = gh_content_page_get_view(content);
    if (GH_IS_CONVERSATION_VIEW(view)) {
      gh_conversation_view_set_reaction_store(GH_CONVERSATION_VIEW(view), reactions);
      gh_conversation_view_set_reaction_func(GH_CONVERSATION_VIEW(view),
                                             on_react, self, NULL);
    }
  }
}

static gboolean
reactions_init(GhAppServices *self, GError **error)
{
  (void)error;
  g_signal_connect_swapped(self->account_store, "changed",
                           G_CALLBACK(reactions_share), self);
  reactions_share(self);
  return TRUE;
}

static void
reactions_teardown(GhAppServices *self)
{
  g_signal_handlers_disconnect_by_func(self->account_store, reactions_share, self);
  /* Clear every window's store so nothing references the freed outbox. */
  for (GList *l = gtk_application_get_windows(self->app); l; l = l->next) {
    if (!GH_IS_WINDOW(l->data))
      continue;
    GhContentPage *content = gh_window_get_content(GH_WINDOW(l->data));
    GtkWidget *view = gh_content_page_get_view(content);
    if (GH_IS_CONVERSATION_VIEW(view)) {
      gh_conversation_view_set_reaction_store(GH_CONVERSATION_VIEW(view), NULL);
      gh_conversation_view_set_reaction_func(GH_CONVERSATION_VIEW(view),
                                             NULL, NULL, NULL);
    }
  }
}
#endif

/* W26 slice B (nostrc-191r): the user toggled a reaction. Dispatch to the
 * appropriate backend based on the conversation type. */
#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_OUTBOX
static void
on_react(GhConversation *conversation, GhMessage *target, const gchar *emoji,
         gboolean add, gpointer user_data)
{
  GhAppServices *self = user_data;
  GhReactionStore *reactions = gh_app_outbox_get_reactions(self->outbox);
  const gchar *room_id = gh_conversation_get_room_id(conversation);
  const gchar *target_id = gh_message_get_rumor_id(target);
  const gchar *target_pubkey = gh_message_get_sender(target);
  gint target_kind = gh_message_get_kind(target);
  g_autofree gchar *target_kind_str = g_strdup_printf("%d", target_kind);

  if (!add) {
    /* Remove our own reaction with this emoji. */
    g_autofree gchar *reaction_id = reactions
      ? gh_reaction_store_remove_own(reactions, target_id, emoji, NULL) : NULL;
    if (!reaction_id)
      return;
    switch (gh_conversation_get_backend(conversation)) {
    case GH_CONVERSATION_BACKEND_NIP17: {
      const gchar *const *recipients = gh_conversation_get_peers(conversation);
      GObject *outbox_obj = gh_account_store_get_outbox(self->account_store);
      GhOutbox *outbox = outbox_obj ? GH_OUTBOX(outbox_obj) : NULL;
      if (outbox && recipients)
        gh_outbox_send_deletion_room(outbox, recipients, reaction_id, NULL);
      break;
    }
    case GH_CONVERSATION_BACKEND_NIP29: {
      /* W26 slice B review fix (F2): use kind-5 (NIP-09 author deletion),
       * not kind 9005 (admin delete-event). A regular member can remove
       * their own reaction without admin permission. */
      GObject *service_obj = gh_app_outbox_get_nip29_service(self->outbox);
      if (GH_IS_NIP29_SERVICE(service_obj)) {
        GhNip29Room *room = gh_nip29_service_lookup_room(GH_NIP29_SERVICE(service_obj), room_id);
        if (room)
          gh_nip29_service_send_deletion(GH_NIP29_SERVICE(service_obj), room, reaction_id,
                                         NULL);
      }
      break;
    }
    case GH_CONVERSATION_BACKEND_MLS: {
      GObject *service_obj = gh_app_outbox_get_mls_service(self->outbox);
      if (GH_IS_MLS_SERVICE(service_obj)) {
        GhMlsGroup *group = gh_mls_service_lookup(GH_MLS_SERVICE(service_obj), room_id);
        if (group)
          gh_mls_service_send_deletion(GH_MLS_SERVICE(service_obj), group, reaction_id, NULL);
      }
      break;
    }
    default:
      break;
    }
    return;
  }

  /* W26 slice B review fix (F4): if the user already reacted with this emoji,
   * toggle (remove) instead of double-adding. The picker always sends
   * add=TRUE, so without this guard the same emoji can be added twice. */
  if (reactions) {
    GhReactionSummary *summary = gh_reaction_store_lookup(reactions, target_id);
    if (summary && gh_reaction_summary_own_reaction_id(summary, emoji)) {
      /* Recurse with add=FALSE to trigger the removal path. */
      on_react(conversation, target, emoji, FALSE, user_data);
      return;
    }
  }

  /* Add a reaction. */
  switch (gh_conversation_get_backend(conversation)) {
  case GH_CONVERSATION_BACKEND_NIP17: {
    const gchar *const *recipients = gh_conversation_get_peers(conversation);
    GObject *outbox_obj2 = gh_account_store_get_outbox(self->account_store);
    GhOutbox *outbox = outbox_obj2 ? GH_OUTBOX(outbox_obj2) : NULL;
    if (outbox && recipients) {
      g_autofree gchar *rumor_id =
        gh_outbox_send_reaction_room(outbox, recipients, emoji, target_id,
                                     target_kind_str, NULL);
      if (rumor_id && reactions) {
        const gchar *account = gh_conversation_store_get_account(self->conversations);
        g_autoptr(GhReaction) reaction =
          gh_reaction_new(target_id, rumor_id, account, emoji,
                          g_get_real_time() / G_USEC_PER_SEC, room_id);
        if (reaction)
          gh_reaction_store_admit(reactions, reaction, NULL);
      }
    }
    break;
  }
  case GH_CONVERSATION_BACKEND_NIP29: {
    GObject *service_obj = gh_app_outbox_get_nip29_service(self->outbox);
    if (GH_IS_NIP29_SERVICE(service_obj)) {
      GhNip29Room *room = gh_nip29_service_lookup_room(GH_NIP29_SERVICE(service_obj), room_id);
      if (room)
        gh_nip29_service_send_reaction(GH_NIP29_SERVICE(service_obj), room,
                                       target_id, target_pubkey, target_kind_str,
                                       emoji, reactions, NULL);
    }
    break;
  }
  case GH_CONVERSATION_BACKEND_MLS: {
    GObject *service_obj = gh_app_outbox_get_mls_service(self->outbox);
    if (GH_IS_MLS_SERVICE(service_obj)) {
      GhMlsGroup *group = gh_mls_service_lookup(GH_MLS_SERVICE(service_obj), room_id);
      if (group)
        gh_mls_service_send_reaction(GH_MLS_SERVICE(service_obj), group,
                                     target_id, target_pubkey, target_kind_str,
                                     emoji, NULL);
    }
    break;
  }
  default:
    break;
  }
}
#endif

#if GROUNDHOG_HAVE_ATTACHMENTS
/* Encrypted attachments (charter §6, G22): one GhAttachments following the
 * open store (saved, in memory or damaged), whose downloads and uploads are
 * cancelled when the store goes, over its own GhNetHttp in the network mode.
 * The account signs an upload only for a server it consented to. Nothing
 * is fetched or uploaded but on the user's action. */
static void
attachments_sign_async(gpointer data, const gchar *unsigned_event_json,
                       GCancellable *cancellable, GAsyncReadyCallback callback,
                       gpointer callback_data)
{
  gh_account_controller_sign_with_cancellable_async(GH_ACCOUNT_CONTROLLER(data),
                                                    unsigned_event_json, cancellable, callback,
                                                    callback_data);
}

#if GROUNDHOG_HAVE_MLS_FILES
static GhMlsService *mls_ui_service(gpointer data);
#endif

static void picture_consent_sync(GhStore *store);

static void
attachments_sync(GhAppServices *self)
{
  GhAccountStoreState state = gh_account_store_get_state(self->account_store);
  GhStore *store = state == GH_ACCOUNT_STORE_OPEN || state == GH_ACCOUNT_STORE_EPHEMERAL ||
                       state == GH_ACCOUNT_STORE_CORRUPT
                     ? gh_account_store_get_store(self->account_store)
                     : NULL;
  picture_consent_sync(store);
  if (store == gh_attachments_get_store(self->attachments))
    return;
  gh_attachments_set_store(self->attachments, store);
  if (self->preferences_dialog)
    gh_preferences_dialog_refresh_attachments(self->preferences_dialog);
}

/* Right after a close, before any other store can open (the downloads
 * borrowed it). */
/* Profile-picture consent lives in the open store's contacts table (W32,
 * schema v9); the cache in each window reads and writes it through this. */
#if GROUNDHOG_HAVE_CONVERSATION_INFO
static GStrv
picture_consent_list(gpointer data, GError **error)
{ return gh_store_contacts_list_picture_allowed(data, error); }
static gboolean
picture_consent_set(gpointer data, const gchar *pubkey, gint64 allowed_at, GError **error)
{ return gh_store_contacts_set_picture_allowed(data, pubkey, allowed_at, error); }
static gboolean
picture_consent_clear(gpointer data, GError **error)
{ return gh_store_contacts_clear_picture_allowed(data, error); }
static const GhPictureConsentBackend picture_consent_backend = {
  picture_consent_list, picture_consent_set, picture_consent_clear
};
#endif

static void
picture_consent_sync(GhStore *store)
{
  for (GList *w = gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));
       w; w = w->next) {
    if (!GH_IS_WINDOW(w->data)) continue;
#if GROUNDHOG_HAVE_CONVERSATION_INFO
    gh_conversation_list_set_picture_consent(GH_WINDOW(w->data),
                                             store ? &picture_consent_backend : NULL, store);
#else
    (void)store;
    gh_conversation_list_set_picture_consent(GH_WINDOW(w->data), NULL, NULL);
#endif
  }
}

static void
attachments_store_closed(GhAppServices *self)
{
  gh_attachments_set_store(self->attachments, NULL);
  picture_consent_sync(NULL);
}

static gboolean
attachments_init(GhAppServices *self, GError **error)
{
  (void)error;
  self->attachments_http = gh_net_http_new(self->settings);
  GhAttachmentsConfig config = {
    .settings = self->settings,
    .http = self->attachments_http,
    .sign_async = attachments_sign_async,
    .sign_finish = gh_account_controller_sign_finish,
    .sign_data = self->accounts,
  };
  self->attachments = gh_attachments_new(&config);
#if GROUNDHOG_HAVE_MLS_FILES
  /* The same client, store and consents; the store's own MLS service. */
  self->mls_files = gh_mls_attachments_new(self->attachments);
  gh_mls_attachments_set_service_func(self->mls_files, mls_ui_service, self);
#endif
  g_signal_connect_swapped(self->account_store, "changed", G_CALLBACK(attachments_sync), self);
  g_signal_connect_swapped(self->account_store, "store-closed",
                           G_CALLBACK(attachments_store_closed), self);
  attachments_sync(self);
  return TRUE;
}

static void
attachments_teardown(GhAppServices *self)
{
  g_signal_handlers_disconnect_by_func(self->account_store, attachments_sync, self);
  g_signal_handlers_disconnect_by_func(self->account_store, attachments_store_closed, self);
  gh_attachments_set_store(self->attachments, NULL);
#if GROUNDHOG_HAVE_MLS_FILES
  dispose_object(&self->mls_files);
#endif
  dispose_object(&self->attachments);
  dispose_object(&self->attachments_http);
}

/* Preferences › Attachments: the cache and the consents of the open store. */
static gboolean
preferences_cache_size(gpointer data, gint64 *out_bytes, GError **error)
{
  return gh_attachments_get_cache_size(GH_ATTACHMENTS(data), out_bytes, error);
}

static gboolean
preferences_clear_cache(gpointer data, GError **error)
{
  return gh_attachments_clear_cache(GH_ATTACHMENTS(data), error);
}

static gboolean
preferences_get_consent(gpointer data, const gchar *server)
{
  return gh_attachments_get_consent(GH_ATTACHMENTS(data), server);
}

static gboolean
preferences_revoke_consent(gpointer data, const gchar *server, GError **error)
{
  return gh_attachments_set_consent(GH_ATTACHMENTS(data), server, FALSE, error);
}
#endif

#if GROUNDHOG_HAVE_NOTIFIER
/* Private notifications (charter §5, G16): mutes and blocks are read from
 * the account's encrypted store; with none open nothing is notified anyway
 * (the model is empty), and an unreadable state is never notified. */
static gboolean
notifier_room_state(gpointer data, const gchar *room_id, GhNotifierRoomState *state)
{
  GhStoreConversations *store = gh_account_store_get_conversations(GH_ACCOUNT_STORE(data));
  GhStoreNotifyState stored = { 0 };
  if (store && !gh_store_conversations_get_notify_state(store, room_id, &stored, NULL))
    return FALSE;
  state->muted_until = stored.muted_until;
  state->blocked = stored.blocked;
  return TRUE;
}

/* N1 (nostrc-qp24.84): the account's last-seen marker is the DM inbox's
 * checkpoint in its encrypted store, read when the model binds it (before
 * this session's inbox moves it). None without an open store of that
 * account. */
static gint64
notifier_last_seen(gpointer data, const gchar *account)
{
  GhAccountStore *account_store = GH_ACCOUNT_STORE(data);
  GhStore *store = gh_account_store_get_store(account_store);
  gint64 since = 0;
  g_autoptr(GError) error = NULL;
  if (!store || g_strcmp0(gh_store_get_account_pubkey(store), account) != 0)
    return 0;
  if (!gh_store_get_cursor(store, GH_ACCOUNT_STORE_INBOX_CURSOR, "", &since, &error)) {
    g_message("Groundhog could not read when messages were last received: %s", error->message);
    return 0;
  }
  return since;
}

/* NO-11: a locked store gets one hidden notice while no window shows it. */
static void
sync_notifier_locked(GhAppServices *self)
{
  gh_notifier_set_store_locked(self->notifier, gh_account_store_get_state(self->account_store) ==
                                                 GH_ACCOUNT_STORE_LOCKED);
}

static gboolean
notifier_init(GhAppServices *self, GError **error)
{
  (void)error;
  GhNotifierConfig config = {
    .settings = self->settings,
    .conversations = self->conversations,
    .room_state = notifier_room_state,
    .room_state_data = self->account_store,
    .last_seen = notifier_last_seen,
    .last_seen_data = self->account_store,
  };
  self->notifier = gh_notifier_new(G_APPLICATION(self->app), &config);
  g_signal_connect_swapped(self->account_store, "changed", G_CALLBACK(sync_notifier_locked),
                           self);
  sync_notifier_locked(self);
  return TRUE;
}

/* Withdraws what it shows and removes app.open-conversation. */
static void
notifier_teardown(GhAppServices *self)
{
  g_signal_handlers_disconnect_by_func(self->account_store, sync_notifier_locked, self);
  dispose_object(&self->notifier);
}
#endif

#if GROUNDHOG_HAVE_CONVERSATION_INFO
/* ---- Conversation Info (G19) ---------------------------------------------------
 * The dialog acts on the active account's open store; names come from the
 * contact directory's cache (display only, nothing is fetched). */

static void
conversation_info_profile(const gchar *pubkey, GhConversationInfoProfile *profile,
                          gpointer data)
{
  GhAppServices *self = data;
  GhContactDirectory *directory = gh_app_outbox_get_directory(self->outbox);
  if (!directory)
    return;
  profile->name = gh_contact_directory_get_display_name(directory, pubkey);
  profile->nip05 = gh_contact_directory_get_nip05(directory, pubkey);
  /* The consented picture, if the window's cache has it (W32). */
  GtkWindow *window = gtk_application_get_active_window(self->app);
  if (GH_IS_WINDOW(window)) {
    GdkTexture *picture = gh_conversation_list_get_picture(GH_WINDOW(window), pubkey);
    profile->picture = picture ? GDK_PAINTABLE(picture) : NULL;
  }
}

static gboolean
conversation_info_services(GhConversationInfoServices *services, gpointer data)
{
  GhAppServices *self = data;
  GhStoreConversations *conversations = gh_account_store_get_conversations(self->account_store);
  GhStore *store = gh_account_store_get_store(self->account_store);
  services->model = self->conversations;
  services->conversations = store ? conversations : NULL;
  services->store = conversations ? store : NULL;
  services->expiry = self->expiry;
  services->notifier = self->notifier;
  services->profile = conversation_info_profile;
  services->profile_data = self;
  return TRUE;
}
#endif

#if GROUNDHOG_HAVE_BACKGROUND
/* Background delivery (charter §5.3, G15): holds the application while
 * run-in-background is on, so the services above outlive the window, and
 * keeps autostart in line with the user's choice. Last in the table, so its
 * hold is released and its portal requests cancelled before anything it
 * watches stops. Onboarding and Preferences reach it with
 * gh_background_get_for_application(). */
static gboolean
background_init(GhAppServices *self, GError **error)
{
  (void)error;
  GhBackgroundConfig config = {
    .settings = self->settings,
#if GROUNDHOG_HAVE_ACCOUNT_STORE
    .account_store = G_OBJECT(self->account_store),
#endif
  };
  self->background = gh_background_new(G_APPLICATION(self->app), &config);
  return TRUE;
}

static void
background_teardown(GhAppServices *self)
{
  dispose_object(&self->background);
}
#endif

#if GROUNDHOG_HAVE_ACCOUNTS
/* app.preferences (Ctrl+, and the primary menu; charter §7.11, G17): the
 * dialog binds the settings itself; the active account and, with the
 * encrypted store, its forget (§3.8, ST-8) come from here. */
#if GROUNDHOG_HAVE_ACCOUNT_STORE
static void
preferences_forget_async(GObject *target, const gchar *npub, GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer user_data)
{
  /* An npub that does not decode fails as "Not an account public key". */
  g_autofree gchar *hex = gh_identity_pubkey_hex(npub);
  gh_account_store_forget_async(GH_ACCOUNT_STORE(target), hex, cancellable, callback,
                                user_data);
}

static GhPreferencesForgetResult
preferences_forget_finish(GObject *target, GAsyncResult *result, GError **error)
{
  g_autoptr(GError) local = NULL;
  if (gh_account_store_forget_finish(GH_ACCOUNT_STORE(target), result, &local))
    return GH_PREFERENCES_FORGET_DELETED;
  gboolean key_kept = g_error_matches(local, GH_ACCOUNT_STORE_SHRED_ERROR,
                                      GH_ACCOUNT_STORE_SHRED_ERROR_KEY_KEPT);
  g_propagate_error(error, g_steal_pointer(&local));
  return key_kept ? GH_PREFERENCES_FORGET_KEY_KEPT : GH_PREFERENCES_FORGET_FAILED;
}
#endif

#if GROUNDHOG_HAVE_CONVERSATION_INFO
/* Privacy › Blocked Conversations (nostrc-qp24.72): the open store's blocks,
 * data being the GhAccountStore. */
static GhStoreConversations *
blocked_store(gpointer data, const gchar **account, GError **error)
{
  GhStore *store = gh_account_store_get_store(GH_ACCOUNT_STORE(data));
  GhStoreConversations *conversations = gh_account_store_get_conversations(GH_ACCOUNT_STORE(data));
  if (!store || !conversations) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        _("Blocks are kept in the account's encrypted message storage, which "
                          "is not open."));
    return NULL;
  }
  if (account)
    *account = gh_store_get_account_pubkey(store);
  return conversations;
}

static GPtrArray *
preferences_blocked_list(gpointer data, GError **error)
{
  const gchar *account = NULL;
  GhStoreConversations *conversations = blocked_store(data, &account, error);
  return conversations ? gh_blocked_conversations_list(conversations, account, error) : NULL;
}

static gboolean
preferences_unblock(gpointer data, const gchar *room_id, GError **error)
{
  GhStoreConversations *conversations = blocked_store(data, NULL, error);
  return conversations && gh_blocked_conversations_unblock(conversations, room_id, error);
}
#endif

static void
sync_preferences_account(GhAccountController *accounts, gpointer dialog)
{
#if GROUNDHOG_HAVE_INBOX
  GhAppServices *self = g_object_get_data(G_OBJECT(dialog), "gh-app-services");
#endif
  const gchar *npub = gh_account_controller_get_active_npub(accounts);
#if GROUNDHOG_HAVE_INBOX
  g_autofree gchar *pubkey = npub ? gh_identity_pubkey_hex(npub) : NULL;
#endif
  GPtrArray *identities = gh_account_controller_get_identities(accounts);
  const gchar *label = NULL;
  for (guint i = 0; npub && identities && i < identities->len; i++) {
    GhIdentityInfo *info = g_ptr_array_index(identities, i);
    if (g_strcmp0(info->npub, npub) == 0)
      label = info->label;
  }
#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_OUTBOX
  const gchar *name = pubkey ? gh_display_name_lookup(pubkey) : NULL;
  if (name && *name)
    label = name;
#endif
  gh_preferences_dialog_set_account(GH_PREFERENCES_DIALOG(dialog), npub, label);
#if GROUNDHOG_HAVE_INBOX
  GtkWindow *window = self ? gtk_application_get_active_window(self->app) : NULL;
  if (pubkey && GH_IS_WINDOW(window) &&
      g_settings_get_boolean(self->settings, "load-profile-pictures")) {
    GhPictureCache *cache = gh_conversation_list_get_picture_cache(GH_WINDOW(window));
    if (cache && !gh_picture_cache_is_allowed(cache, pubkey))
      gh_picture_cache_allow(cache, pubkey);
  }
  GdkTexture *picture = pubkey && GH_IS_WINDOW(window)
    ? gh_conversation_list_get_picture(GH_WINDOW(window), pubkey) : NULL;
  gh_preferences_dialog_set_account_picture(GH_PREFERENCES_DIALOG(dialog),
                                            picture ? GDK_PAINTABLE(picture) : NULL);
#endif
}

#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_OUTBOX
static void
on_preferences_profile_changed(GhContactDirectory *directory, const gchar *pubkey,
                               gpointer dialog)
{
  (void)directory;
  g_autofree gchar *own = gh_identity_pubkey_hex(
    gh_account_controller_get_active_npub(g_object_get_data(G_OBJECT(dialog), "gh-accounts")));
  if (g_strcmp0(pubkey, own) == 0)
    sync_preferences_account(g_object_get_data(G_OBJECT(dialog), "gh-accounts"), dialog);
}

static void
on_preferences_picture_changed(GhPictureCache *cache, const gchar *pubkey,
                               gpointer dialog)
{
  (void)cache;
  on_preferences_profile_changed(NULL, pubkey, dialog);
}
#endif

#if GROUNDHOG_HAVE_GROUP_UI && GROUNDHOG_HAVE_MLS_UI
static GhMlsService *mls_ui_service(gpointer data);

/* Network › Encrypted Groups (nostrc-f8a5): the open store's KeyPackage
 * state, live while the dialog is shown. */
static GhPreferencesKeyPackage
preferences_key_package(GhMlsService *service, GhAccountRelays *relays)
{
  GhPreferencesKeyPackage state = GH_PREFERENCES_KEY_PACKAGE_UNKNOWN;
  switch (service ? gh_mls_service_get_key_package_state(service) : GH_MLS_KEY_PACKAGE_NONE) {
  case GH_MLS_KEY_PACKAGE_NO_RELAYS:
    /* A list without a relay the account publishes to is not "no list"
     * (re-review R3): the fix is adding relays to it. */
    state = relays && gh_account_relays_has_relay_list(relays)
              ? GH_PREFERENCES_KEY_PACKAGE_NO_WRITE_RELAYS
              : GH_PREFERENCES_KEY_PACKAGE_NO_RELAYS;
    break;
  case GH_MLS_KEY_PACKAGE_PUBLISHING: state = GH_PREFERENCES_KEY_PACKAGE_PUBLISHING; break;
  case GH_MLS_KEY_PACKAGE_PUBLISHED:
    state = gh_mls_service_get_key_package_held(service) ? GH_PREFERENCES_KEY_PACKAGE_HELD
                                                         : GH_PREFERENCES_KEY_PACKAGE_PUBLISHED;
    break;
  case GH_MLS_KEY_PACKAGE_FAILED: state = GH_PREFERENCES_KEY_PACKAGE_FAILED; break;
  case GH_MLS_KEY_PACKAGE_NONE:
    /* When the key package is NONE, the identity state says why: waiting
     * for the account proof, declined or failed (nostrc-q74l). */
    if (service) {
      switch (gh_mls_service_get_identity_state(service)) {
      case GH_MLS_IDENTITY_WAITING:
        state = GH_PREFERENCES_KEY_PACKAGE_IDENTITY_WAITING;
        break;
      case GH_MLS_IDENTITY_DECLINED:
        state = GH_PREFERENCES_KEY_PACKAGE_IDENTITY_DECLINED;
        break;
      case GH_MLS_IDENTITY_FAILED:
        state = GH_PREFERENCES_KEY_PACKAGE_IDENTITY_FAILED;
        break;
      default: break;
      }
    }
    break;
  default: break;
  }
  return state;
}

/* Live while the dialog exists: the service's state and hold, and whether
 * the account has a relay list (handlers go with the dialog). */
static void
sync_preferences_key_package(GObject *source, GParamSpec *pspec, gpointer dialog)
{
  (void)source;
  (void)pspec;
  GhAppServices *self = g_object_get_data(G_OBJECT(dialog), "gh-app-services");
  if (!self)
    return;
  gh_preferences_dialog_set_key_package_state(GH_PREFERENCES_DIALOG(dialog),
                                              preferences_key_package(mls_ui_service(self),
                                                                      self->relays));
}

static void
sync_preferences_relays(GhAccountRelays *relays, gpointer dialog)
{
  sync_preferences_key_package(G_OBJECT(relays), NULL, dialog);
  /* nostrc-mi1z: update the "Where people reach you" section. */
  if (relays)
    gh_preferences_dialog_set_published_relays(
      GH_PREFERENCES_DIALOG(dialog),
      gh_account_relays_get_inbox_relays(relays),
      gh_account_relays_get_write_relays(relays));
}

/* nostrc-q74l: the preferences dialog asked to retry the account-proof
 * enrollment. */
static void
on_retry_identity(GhMlsService *mls)
{
  g_autoptr(GError) error = NULL;
  if (!gh_mls_service_retry_identity(mls, &error))
    g_info("retry-identity: %s", error->message);
}

/* nostrc-mi1z: "Change Relays…" from Preferences opens the onboarding
 * relay step after closing the dialog. */
static void
on_change_relays(GhAppServices *self, GhPreferencesDialog *dialog)
{
  GtkWindow *window = gtk_application_get_active_window(self->app);
  if (window)
    gtk_widget_activate_action(GTK_WIDGET(window), "win.setup-inbox", NULL);
  adw_dialog_close(ADW_DIALOG(dialog));
}

/* nostrc-mi1z phase 2: inline add/remove relay in Preferences.
 * Builds the modified event, then runs a GhRelayListSetup in EDIT mode. */
static void
on_relay_edit_changed(GhRelayListSetup *setup, gpointer dialog)
{
  GhRelayListSetupState state = gh_relay_list_setup_get_state(setup);
  switch (state) {
  case GH_RELAY_LIST_SETUP_DONE: {
    guint n = gh_relay_list_setup_get_n_accepted(setup);
    gchar *msg = g_strdup_printf(ngettext("Published to %u relay",
                                          "Published to %u relays", n), n);
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog),
                                     adw_toast_new(msg));
    g_free(msg);
    break;
  }
  case GH_RELAY_LIST_SETUP_SKIPPED:
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog),
      adw_toast_new(_("Another app published a newer relay list — not overwritten")));
    break;
  case GH_RELAY_LIST_SETUP_FAILED: {
    const GError *error = gh_relay_list_setup_get_error(setup);
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog),
      adw_toast_new(error ? error->message : _("Publishing failed")));
    break;
  }
  default:
    break;
  }
}

/* Build a modified 10050 event: current relays ± url. */
static gchar *
build_modified_inbox(GhAccountRelays *relays, GhAccountController *accounts,
                     const gchar *url, gboolean add, gint64 created_at)
{
  const gchar *const *current = gh_account_relays_get_inbox_relays(relays);
  g_autoptr(GPtrArray) urls = g_ptr_array_new();
  for (guint i = 0; current && current[i]; i++) {
    if (!add && g_str_equal(current[i], url))
      continue;
    g_ptr_array_add(urls, (gchar *)current[i]);
  }
  if (add)
    g_ptr_array_add(urls, (gchar *)url);
  g_ptr_array_add(urls, NULL);
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(
    gh_account_controller_get_active_npub(accounts));
  return pubkey ? gh_inbox_setup_build_unsigned(pubkey,
    (const gchar *const *)urls->pdata, created_at) : NULL;
}

/* Build a modified 10002 event: current relays ± url (as write). */
static gchar *
build_modified_relay_list(GhAccountRelays *relays, GhAccountController *accounts,
                          const gchar *url, gboolean add, gint64 created_at)
{
  const gchar *const *current = gh_account_relays_get_write_relays(relays);
  g_autoptr(GPtrArray) urls = g_ptr_array_new();
  for (guint i = 0; current && current[i]; i++) {
    if (!add && g_str_equal(current[i], url))
      continue;
    g_ptr_array_add(urls, (gchar *)current[i]);
  }
  if (add)
    g_ptr_array_add(urls, (gchar *)url);
  g_ptr_array_add(urls, NULL);
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(
    gh_account_controller_get_active_npub(accounts));
  return pubkey ? gh_inbox_setup_build_relay_list_edit_unsigned(
    gh_account_relays_get_relay_list_json(relays), pubkey,
    (const gchar *const *)urls->pdata, created_at) : NULL;
}

/* Publish to the updated list and the discovery relays where it is found.
 * A relay removed from this role must not block its own removal if offline. */
static GStrv
relay_edit_targets(GhAppServices *self, gint kind, const gchar *url, gboolean add)
{
  g_autoptr(GPtrArray) targets = g_ptr_array_new_with_free_func(g_free);
  if (self->relays) {
    const gchar *const *write = gh_account_relays_get_write_relays(self->relays);
    for (guint i = 0; write && write[i]; i++)
      if (add || kind != 10002 || !g_str_equal(write[i], url))
        g_ptr_array_add(targets, g_strdup(write[i]));
    const gchar *const *inbox = gh_account_relays_get_inbox_relays(self->relays);
    for (guint i = 0; inbox && inbox[i]; i++) {
      if (!add && kind == 10050 && g_str_equal(inbox[i], url))
        continue;
      gboolean dup = FALSE;
      for (guint j = 0; !dup && j < targets->len; j++)
        dup = g_str_equal(g_ptr_array_index(targets, j), inbox[i]);
      if (!dup)
        g_ptr_array_add(targets, g_strdup(inbox[i]));
    }
  }
  if (add) {
    gboolean dup = FALSE;
    for (guint j = 0; !dup && j < targets->len; j++)
      dup = g_str_equal(g_ptr_array_index(targets, j), url);
    if (!dup)
      g_ptr_array_add(targets, g_strdup(url));
  }
  if (self->settings) {
    g_auto(GStrv) discovery = g_settings_get_strv(self->settings, "discovery-relays");
    for (guint i = 0; discovery[i]; i++) {
      gboolean dup = FALSE;
      for (guint j = 0; !dup && j < targets->len; j++)
        dup = g_str_equal(g_ptr_array_index(targets, j), discovery[i]);
      if (!dup)
        g_ptr_array_add(targets, g_strdup(discovery[i]));
    }
  }
  g_ptr_array_add(targets, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&targets), FALSE);
}

/* The base event's id and created_at from the raw JSON (GhAccountRelays). */
static void
base_event_info(const gchar *json, gchar **out_id, gint64 *out_created_at)
{
  *out_id = NULL;
  *out_created_at = 0;
  if (!json)
    return;
  NostrEvent *event = nostr_event_new();
  if (event && nostr_event_deserialize_compact(event, json, NULL) == 1) {
    char *id = nostr_event_get_id(event);
    *out_id = id ? g_strdup(id) : NULL;
    free(id);
    *out_created_at = nostr_event_get_created_at(event);
  }
  if (event)
    nostr_event_free(event);
}

static void
on_edit_relay(GhPreferencesDialog *dialog, gint kind, const gchar *url, gboolean add,
              GhAppServices *self)
{
  if (!self->relays || !self->accounts)
    return;
  g_autofree gchar *base_id = NULL;
  gint64 base_created_at = 0;
  if (kind == 10050) {
    const gchar *inbox_id = gh_account_relays_get_inbox_event_id(self->relays);
    base_id = inbox_id ? g_strdup(inbox_id) : NULL;
    base_created_at = gh_account_relays_get_inbox_created_at(self->relays);
  } else if (kind == 10002) {
    base_event_info(gh_account_relays_get_relay_list_json(self->relays),
                    &base_id, &base_created_at);
  } else
    return;
  gint64 created_at = MAX(g_get_real_time() / G_USEC_PER_SEC, base_created_at + 1);
  g_autofree gchar *unsigned_json = kind == 10050
    ? build_modified_inbox(self->relays, self->accounts, url, add, created_at)
    : build_modified_relay_list(self->relays, self->accounts, url, add, created_at);
  if (!unsigned_json) {
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog),
      adw_toast_new(_("Could not build the relay list")));
    return;
  }
  g_auto(GStrv) targets = relay_edit_targets(self, kind, url, add);
  GhInboxSetupConfig config = {
    .accounts = self->accounts,
    .account_relays = self->relays,
    .settings = self->settings,
    .offer_relay_list = TRUE,
  };
  g_autoptr(GhRelayListSetup) setup = gh_relay_list_setup_new(&config);
  g_signal_connect_object(setup, "changed", G_CALLBACK(on_relay_edit_changed), dialog, 0);
  g_autoptr(GError) error = NULL;
  if (!gh_relay_list_setup_start_edit(setup, unsigned_json, base_id, base_created_at,
                                       kind, (const gchar *const *)targets, &error)) {
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog),
      adw_toast_new(error->message));
    return;
  }
  /* Keep it alive until it settles: the ref passes to the dialog's qdata,
   * cleared when the dialog closes or the setup reaches a terminal state. */
  g_object_set_data_full(G_OBJECT(dialog), "relay-edit-setup",
                         g_object_ref(setup), g_object_unref);
}

static void
on_add_relay(GhPreferencesDialog *dialog, gint kind, const gchar *url, GhAppServices *self)
{
  on_edit_relay(dialog, kind, url, TRUE, self);
}

static void
on_remove_relay(GhPreferencesDialog *dialog, gint kind, const gchar *url, GhAppServices *self)
{
  on_edit_relay(dialog, kind, url, FALSE, self);
}

static void
key_package_sync_attach(GhAppServices *self, GhPreferencesDialog *dialog)
{
  g_object_set_data(G_OBJECT(dialog), "gh-app-services", self);
  sync_preferences_key_package(NULL, NULL, dialog);
  GhMlsService *mls = mls_ui_service(self);
  /* "retry-identity" (nostrc-q74l): retry the account-proof enrollment. */
  if (mls)
    g_signal_connect_object(dialog, "retry-identity",
                            G_CALLBACK(on_retry_identity), mls, G_CONNECT_SWAPPED);
  if (mls) {
    g_signal_connect_object(mls, "notify::key-package-state",
                            G_CALLBACK(sync_preferences_key_package), dialog, 0);
    g_signal_connect_object(mls, "notify::key-package-held",
                            G_CALLBACK(sync_preferences_key_package), dialog, 0);
    /* The identity state (account proof) affects the key package section
     * when the key package is NONE (nostrc-q74l). */
    g_signal_connect_object(mls, "notify::identity-state",
                            G_CALLBACK(sync_preferences_key_package), dialog, 0);
  }
  if (self->relays) {
    g_signal_connect_object(self->relays, "changed", G_CALLBACK(sync_preferences_relays), dialog,
                            0);
    /* nostrc-mi1z: initial sync. */
    sync_preferences_relays(self->relays, dialog);
  }
  /* nostrc-mi1z: "Change Relays…" closes the dialog and opens the relay
   * setup step, the same as the key-package [Set Up] button. */
  g_signal_connect_swapped(dialog, "change-relays",
                           G_CALLBACK(on_change_relays), self);
  /* nostrc-mi1z phase 2: inline add/remove relays in Preferences. */
  g_signal_connect(dialog, "add-relay", G_CALLBACK(on_add_relay), self);
  g_signal_connect(dialog, "remove-relay", G_CALLBACK(on_remove_relay), self);
}
#endif

/* page: the page to show (NULL: the dialog's first). */
static void
present_preferences(GhAppServices *self, const gchar *page)
{
  GtkWindow *window = gtk_application_get_active_window(self->app);
  if (!ADW_IS_APPLICATION_WINDOW(window))
    return;
  /* One at a time, even under its own confirmation dialog: a second one
   * could start a second deletion. */
  if (self->preferences_dialog && gtk_widget_get_root(GTK_WIDGET(self->preferences_dialog))) {
    if (page)
      adw_preferences_dialog_set_visible_page_name(
        ADW_PREFERENCES_DIALOG(self->preferences_dialog), page);
    return;
  }
  /* Rows whose feature this build lacks say so (src/app/gh-features.h). */
  GhPreferencesDialog *dialog = gh_preferences_dialog_new(self->settings,
                                                          gh_features_for_preferences());
  g_set_weak_pointer(&self->preferences_dialog, dialog);
  g_object_set_data(G_OBJECT(dialog), "gh-app-services", self);
  g_object_set_data(G_OBJECT(dialog), "gh-accounts", self->accounts);
  sync_preferences_account(self->accounts, dialog);
  g_signal_connect_object(self->accounts, "changed", G_CALLBACK(sync_preferences_account),
                          dialog, 0);
#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_OUTBOX
  GhContactDirectory *directory = gh_app_outbox_get_directory(self->outbox);
  if (directory)
    g_signal_connect_object(directory, "profile-changed",
                            G_CALLBACK(on_preferences_profile_changed), dialog, 0);
  GhPictureCache *pictures = gh_conversation_list_get_picture_cache(GH_WINDOW(window));
  if (pictures)
    g_signal_connect_object(pictures, "picture-changed",
                            G_CALLBACK(on_preferences_picture_changed), dialog, 0);
#endif
#if GROUNDHOG_HAVE_TOR
  sync_preferences_tor(self->network, NULL, dialog);
  g_signal_connect_object(self->network, "notify::tor-state", G_CALLBACK(sync_preferences_tor),
                          dialog, 0);
#endif
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  gh_preferences_dialog_set_forget_func(dialog, preferences_forget_async,
                                        preferences_forget_finish,
                                        G_OBJECT(self->account_store));
#endif
#if GROUNDHOG_HAVE_CONVERSATION_INFO
  static const GhBlockedBackend blocked = { preferences_blocked_list, preferences_unblock };
  gh_blocked_page_attach(dialog, &blocked, g_object_ref(self->account_store), g_object_unref);
#endif
#if GROUNDHOG_HAVE_ATTACHMENTS
  static const GhPreferencesAttachments attachments = {
    .max_file_size = GH_BLOSSOM_MAX_FILE_SIZE,
    .get_cache_size = preferences_cache_size,
    .clear_cache = preferences_clear_cache,
    .get_consent = preferences_get_consent,
    .revoke_consent = preferences_revoke_consent,
  };
  gh_preferences_dialog_set_attachments(dialog, &attachments, g_object_ref(self->attachments),
                                        g_object_unref);
#endif
#if GROUNDHOG_HAVE_GROUP_UI && GROUNDHOG_HAVE_MLS_UI
  key_package_sync_attach(self, dialog);
#endif
  if (page)
    adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(dialog), page);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
}

static void
on_preferences(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  present_preferences(data, NULL);
}

/* app.network-settings: the "Can't reach Tor" banner's [Network Settings]. */
static void
on_network_settings(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  present_preferences(data, "network");
}

static gboolean
preferences_init(GhAppServices *self, GError **error)
{
  (void)error;
  self->preferences_action = g_simple_action_new("preferences", NULL);
  g_signal_connect(self->preferences_action, "activate", G_CALLBACK(on_preferences), self);
  g_action_map_add_action(G_ACTION_MAP(self->app), G_ACTION(self->preferences_action));
  self->network_settings_action = g_simple_action_new("network-settings", NULL);
  g_signal_connect(self->network_settings_action, "activate", G_CALLBACK(on_network_settings),
                   self);
  g_action_map_add_action(G_ACTION_MAP(self->app), G_ACTION(self->network_settings_action));
  return TRUE;
}

static void
preferences_teardown(GhAppServices *self)
{
  g_action_map_remove_action(G_ACTION_MAP(self->app), "network-settings");
  g_clear_object(&self->network_settings_action);
  g_action_map_remove_action(G_ACTION_MAP(self->app), "preferences");
  g_clear_object(&self->preferences_action);
  g_clear_weak_pointer(&self->preferences_dialog);
}
#endif

/* app.about (the primary menu): the About dialog, over the active window.
 * It reads only build-time constants and contacts no network. */
#if GROUNDHOG_HAVE_ISSUE
static void
on_report_issue(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  GhAppServices *self = data;
  GtkWindow *window = gtk_application_get_active_window(self->app);
  if (window)
    adw_dialog_present(ADW_DIALOG(gh_issue_dialog_new(self->accounts, self->settings)), GTK_WIDGET(window));
}
#endif

static void
on_about(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GhAppServices *self = data;
  (void)action;
  (void)parameter;
  GtkWindow *window = gtk_application_get_active_window(self->app);
  gh_about_dialog_present(window ? GTK_WIDGET(window) : NULL);
}

static gboolean
about_init(GhAppServices *self, GError **error)
{
  (void)error;
#if GROUNDHOG_HAVE_ISSUE
  g_autoptr(GSimpleAction) report = g_simple_action_new("report-issue", NULL);
  g_signal_connect(report, "activate", G_CALLBACK(on_report_issue), self);
  g_action_map_add_action(G_ACTION_MAP(self->app), G_ACTION(report));
#endif
  self->about_action = g_simple_action_new("about", NULL);
  g_signal_connect(self->about_action, "activate", G_CALLBACK(on_about), self);
  g_action_map_add_action(G_ACTION_MAP(self->app), G_ACTION(self->about_action));
  return TRUE;
}

static void
about_teardown(GhAppServices *self)
{
  g_action_map_remove_action(G_ACTION_MAP(self->app), "about");
  g_action_map_remove_action(G_ACTION_MAP(self->app), "report-issue");
  g_clear_object(&self->about_action);
}

#if GROUNDHOG_HAVE_NEW_MESSAGE
/* NIP-05 lookups for New Message (charter §7.9, G18): one HTTPS GET per
 * lookup the user chose, in the configured network mode. Nothing is
 * contacted until then. */
static gboolean
new_message_init(GhAppServices *self, GError **error)
{
  (void)error;
  self->nip05 = gh_nip05_new(self->settings, NULL, NULL);
  return TRUE;
}

static void
new_message_teardown(GhAppServices *self)
{
  dispose_object(&self->nip05);
}
#endif

#if GROUNDHOG_HAVE_NEW_MESSAGE && GROUNDHOG_HAVE_OUTBOX
/* What the contact directory has cached; never a lookup. */
static gchar *
directory_picture_uri(const gchar *pubkey, gpointer data)
{
  return gh_contact_directory_dup_picture_uri(GH_CONTACT_DIRECTORY(data), pubkey);
}

static const gchar *
directory_display_name(gpointer data, const gchar *pubkey)
{
  return gh_contact_directory_get_display_name(GH_CONTACT_DIRECTORY(data), pubkey);
}

static void
relay_display_name_changed(GObject *directory, const gchar *pubkey, gpointer data)
{
  (void)directory; (void)data;
  gh_display_name_changed(pubkey);
}

static const gchar *
display_name_resolver(const gchar *pubkey, gpointer data)
{
  return gh_contact_directory_get_display_name(GH_CONTACT_DIRECTORY(data), pubkey);
}

static const gchar *
directory_claimed_nip05(gpointer data, const gchar *pubkey)
{
  return gh_contact_directory_get_nip05(GH_CONTACT_DIRECTORY(data), pubkey);
}
#endif

#if GROUNDHOG_HAVE_NEW_MESSAGE && GROUNDHOG_HAVE_MLS_UI
/* ---- Marmot DM creation (nostrc-fmbt) -----------------------------------------
 * New Message creates a 2-member MLS group with empty name (WN's DM shape)
 * when the default-dm-protocol is "marmot". The callback gets the MLS
 * service from the outbox, uses the account's write relays, and returns a
 * GhConversation for the new DM. On failure, the dialog falls back to
 * NIP-17 (gh-new-message-dialog.c marmot_dm_done()). */

typedef struct {
  GhAppServices *services;   /* borrowed through the window's lifetime */
  GTask *task;
  gchar *pubkey;
} MarmotDmCreate;

static void
marmot_dm_create_done(GObject *source, GAsyncResult *result, gpointer data)
{
  MarmotDmCreate *ctx = data;
  GhMlsService *mls = GH_MLS_SERVICE(source);
  g_autoptr(GError) error = NULL;
  GhMlsGroup *group = gh_mls_service_create_group_finish(mls, result, &error);
  if (!group) {
    g_task_return_error(ctx->task, g_steal_pointer(&error));
  } else {
    /* The group_list_room() call inside create_group already ensured a
     * GhConversation with is_direct=TRUE in the store. Look it up. */
    const gchar *room_id = gh_mls_group_get_room_id(group);
    GhConversation *conv = room_id
      ? gh_conversation_store_lookup(ctx->services->conversations, room_id)
      : NULL;
    if (conv)
      g_task_return_pointer(ctx->task, g_object_ref(conv), g_object_unref);
    else
      g_task_return_new_error(ctx->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Marmot DM group created but no conversation found");
  }
  g_object_unref(ctx->task);
  g_free(ctx->pubkey);
  g_free(ctx);
}

static void
app_create_marmot_dm(const gchar *pubkey, GCancellable *cancellable,
                     GAsyncReadyCallback callback, gpointer user_data,
                     gpointer config_data)
{
  GhAppServices *self = config_data;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, app_create_marmot_dm);

  GhMlsService *mls = mls_ui_service(self);
  if (!mls) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "Encrypted messaging is not available");
    g_object_unref(task);
    return;
  }

  const gchar *const *write_relays =
    gh_account_relays_get_write_relays(self->relays);
  if (!write_relays || !write_relays[0]) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "No write relays configured");
    g_object_unref(task);
    return;
  }

  MarmotDmCreate *ctx = g_new0(MarmotDmCreate, 1);
  ctx->services = self;
  ctx->task = task;
  ctx->pubkey = g_strdup(pubkey);

  const gchar *invitees[] = { pubkey, NULL };
  /* WN DM shape: empty name, no description, the account's write relays,
   * one invitee (the peer). The resulting group has 2 members, empty name,
   * so group_is_dm() returns TRUE and group_list_room() sets is_direct. */
  gh_mls_service_create_group_async(mls, "", NULL,
                                     write_relays, invitees,
                                     cancellable,
                                     marmot_dm_create_done, ctx);
}
#endif

#if GROUNDHOG_HAVE_ACCOUNT_STORE
/* Message Requests' Delete and Block (charter §7.9, G18) on the open store:
 * local only, nothing is published. */
static GhStoreConversations *
requests_store(gpointer data, GError **error)
{
  GhStoreConversations *store = gh_account_store_get_conversations(GH_ACCOUNT_STORE(data));
  if (!store)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "The account's message storage is not open");
  return store;
}

static gboolean
requests_forget(gpointer data, GhConversation *request, GError **error)
{
  GhStoreConversations *store = requests_store(data, error);
  return store && gh_store_conversations_forget(store, gh_conversation_get_room_id(request),
                                                error);
}

static gboolean
requests_block(gpointer data, GhConversation *request, GError **error)
{
  GhStoreConversations *store = requests_store(data, error);
  return store && gh_store_conversations_block_and_forget(store, gh_conversation_get_room_id(request),
                                               error);
}
#endif

#if GROUNDHOG_HAVE_GROUP_UI
/* ---- NIP-29 relay groups (G20b) ---------------------------------------------------
 * The open store's GhNip29Service (made beside its outbox, gh-app-outbox.c);
 * Group Info names people with the contact directory's cache only. */
static GhNip29Service *
group_ui_service(gpointer data)
{
  GhAppServices *self = data;
  GObject *service = gh_app_outbox_get_nip29_service(self->outbox);
  return GH_IS_NIP29_SERVICE(service) ? GH_NIP29_SERVICE(service) : NULL;
}

static const gchar *
group_ui_name(const gchar *pubkey, gpointer data)
{
  GhAppServices *self = data;
  GhContactDirectory *directory = gh_app_outbox_get_directory(self->outbox);
  return directory ? gh_contact_directory_get_display_name(directory, pubkey) : NULL;
}

#if GROUNDHOG_HAVE_MLS_UI
/* ---- encrypted groups (qp24.13 part 2) -------------------------------------------------
 * The open store's GhMlsService (made beside its outbox while
 * GH_FEATURE_ENCRYPTED_GROUPS is on, gh-app-outbox.c). */
static GhMlsService *
mls_ui_service(gpointer data)
{
  GhAppServices *self = data;
  GObject *service = gh_app_outbox_get_mls_service(self->outbox);
  return GH_IS_MLS_SERVICE(service) ? GH_MLS_SERVICE(service) : NULL;
}
#endif

/* Store-backed pages for private and encrypted-group rooms. Relay groups
 * are dispatched by gh-group-ui.c. */
static gboolean
group_ui_load_older(GhConversation *conversation, GError **error, gpointer data)
{
  GhAppServices *self = data;
#if GROUNDHOG_HAVE_MLS_UI
  if (gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_MLS) {
    GhMlsService *service = mls_ui_service(self);
    return service && gh_mls_service_load_older(service, conversation,
                                                 GH_CONVERSATION_WINDOW_PAGE, NULL, error);
  }
#endif
  return gh_account_store_load_older(self->account_store, conversation,
                                     GH_CONVERSATION_WINDOW_PAGE, NULL, error);
}

static gboolean
group_ui_load_newer(GhConversation *conversation, GError **error, gpointer data)
{
  GhAppServices *self = data;
#if GROUNDHOG_HAVE_MLS_UI
  if (gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_MLS) {
    GhMlsService *service = mls_ui_service(self);
    return service && gh_mls_service_load_newer(service, conversation,
                                                 GH_CONVERSATION_WINDOW_PAGE, NULL, error);
  }
#endif
  return gh_account_store_load_newer(self->account_store, conversation,
                                     GH_CONVERSATION_WINDOW_PAGE, NULL, error);
}

static gboolean
group_ui_reset_latest(GhConversation *conversation, GError **error, gpointer data)
{
  GhAppServices *self = data;
#if GROUNDHOG_HAVE_MLS_UI
  if (gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_MLS) {
    GhMlsService *service = mls_ui_service(self);
    return service && gh_mls_service_reset_latest(service, conversation, error);
  }
#endif
  return gh_account_store_reset_latest(self->account_store, conversation, error);
}
#endif

typedef struct {
  const gchar *name;
  gboolean (*init)(GhAppServices *self, GError **error);
  void (*teardown)(GhAppServices *self);
} GhAppService;

static const GhAppService services[] = {
  { "application", NULL, NULL },
#if GROUNDHOG_HAVE_ACCOUNTS
  { "settings", settings_init, settings_teardown },
  { "diagnostics", diagnostics_init, diagnostics_teardown },
#if GROUNDHOG_HAVE_TOR
  { "network", network_init, network_teardown },
#elif GH_APP_RELAY_GUARD
  { "network", guard_init, guard_teardown },
#endif
  { "accounts", accounts_init, accounts_teardown },
#endif
#if GROUNDHOG_HAVE_RELAYS
  { "relays", relays_init, relays_teardown },
#endif
#if GROUNDHOG_HAVE_OUTBOX
  { "sender", sender_init, sender_teardown },
#endif
#if GROUNDHOG_HAVE_INBOX
  { "conversations", conversations_init, conversations_teardown },
  { "inbox", inbox_init, inbox_teardown },
#endif
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  { "store-key", store_key_init, store_key_teardown },
  { "account-store", account_store_init, account_store_teardown },
  { "store-actions", store_actions_init, store_actions_teardown },
#endif
#if GROUNDHOG_HAVE_EXPIRY
  { "expiry", expiry_init, expiry_teardown },
#endif
#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_OUTBOX
  { "reactions", reactions_init, reactions_teardown },
#endif
#if GROUNDHOG_HAVE_ATTACHMENTS
  { "attachments", attachments_init, attachments_teardown },
#endif
  { "about", about_init, about_teardown },
#if GROUNDHOG_HAVE_ACCOUNTS
  { "preferences", preferences_init, preferences_teardown },
#endif
#if GROUNDHOG_HAVE_NOTIFIER
  { "notifier", notifier_init, notifier_teardown },
#endif
#if GROUNDHOG_HAVE_BACKGROUND
  { "background", background_init, background_teardown },
#endif
#if GROUNDHOG_HAVE_NEW_MESSAGE
  { "new-message", new_message_init, new_message_teardown },
#endif
};

/* ---- public ------------------------------------------------------------------ */

GhAppServices *
gh_app_services_new(GtkApplication *app, GError **error)
{
  g_return_val_if_fail(GTK_IS_APPLICATION(app), NULL);
  g_autoptr(GhAppServices) self = g_new0(GhAppServices, 1);
  self->app = app;
  for (; self->started < G_N_ELEMENTS(services); self->started++) {
    const GhAppService *service = &services[self->started];
    g_autoptr(GError) local = NULL;
    if (service->init && !service->init(self, &local)) {
      g_propagate_prefixed_error(error, g_steal_pointer(&local),
                                 "Groundhog could not start its %s service: ", service->name);
      return NULL; /* the autoptr tears down what started */
    }
  }
  return g_steal_pointer(&self);
}

void
gh_app_services_free(GhAppServices *self)
{
  if (!self)
    return;
  while (self->started > 0) {
    const GhAppService *service = &services[--self->started];
    if (service->teardown)
      service->teardown(self);
  }
  g_free(self);
}

#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_ACCOUNT_STORE
static gboolean
search_stored_messages(const gchar *query, GHashTable **out_rooms, GError **error,
                       gpointer data)
{
  GhAppServices *self = data;
  GhStore *store = gh_account_store_get_store(self->account_store);
  if (!store) {
    *out_rooms = NULL;
    return TRUE;
  }
  return gh_store_search_message_rooms(store, query, out_rooms, error);
}
#endif

void
gh_app_services_attach_window(GhAppServices *self, GhWindow *window)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(GH_IS_WINDOW(window));
#if GROUNDHOG_HAVE_ACCOUNTS
  gh_account_ui_attach(window, self->accounts, self->settings);
#else
  gh_sidebar_page_show_onboarding(gh_window_get_sidebar(window));
#endif
#if GROUNDHOG_HAVE_INBOX
  gh_conversation_list_attach(window, self->conversations, self->settings);
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  gh_conversation_list_set_message_search_source(window, search_stored_messages, self, NULL);
#endif
  {
    GhAccountStoreState state = gh_account_store_get_state(self->account_store);
    GhStore *open = state == GH_ACCOUNT_STORE_OPEN || state == GH_ACCOUNT_STORE_EPHEMERAL
                      ? gh_account_store_get_store(self->account_store) : NULL;
    picture_consent_sync(open);
  }
  gh_inbox_status_attach(gh_window_get_status(window), self->inbox, self->relays);
#endif
#if GROUNDHOG_HAVE_ACCOUNTS && GROUNDHOG_HAVE_TOR
  GhStatus *status = gh_window_get_status(window);
  sync_tor_banner(self->network, NULL, status);
  g_signal_connect_object(self->network, "notify::tor-state", G_CALLBACK(sync_tor_banner),
                          status, 0);
#elif GH_APP_RELAY_GUARD
  GhStatus *status = gh_window_get_status(window);
  sync_tor_unavailable(self->settings, "network-mode", status);
  g_signal_connect_object(self->settings, "changed::network-mode",
                          G_CALLBACK(sync_tor_unavailable), status, 0);
#endif
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  static const GhRequestsBackend requests_backend = { requests_forget, requests_block };
  gh_requests_view_set_backend(gh_conversation_list_get_requests_view(window), &requests_backend,
                               g_object_ref(self->account_store), g_object_unref);
#endif
#if GROUNDHOG_HAVE_NEW_MESSAGE
  /* New Message (G18): local suggestions from the directory's cache; the
   * directory resolves inboxes and NIP-05 looks addresses up only when the
   * user chooses the row that says so. */
  GhNewMessageConfig new_message = {
    .conversations = self->conversations,
    .nip05 = self->nip05,
    .settings = self->settings,
    .encrypted_groups = GH_FEATURE_ENCRYPTED_GROUPS,
  };
#if GROUNDHOG_HAVE_OUTBOX
  GhContactDirectory *directory = gh_app_outbox_get_directory(self->outbox);
  if (directory) {
    gh_account_ui_set_name_source(window, directory_display_name, G_OBJECT(directory));
    /* W33: names everywhere a person is shown. */
    gh_display_name_set_resolver(display_name_resolver, directory);
    g_signal_connect(directory, "profile-changed", G_CALLBACK(relay_display_name_changed), NULL);
    gh_conversation_view_set_picture_source(GH_CONVERSATION_VIEW(gh_content_page_get_view(gh_window_get_content(window))),
                                             directory_picture_uri, G_OBJECT(directory));
    new_message.inboxes = GH_INBOX_RESOLVER(directory);
    new_message.display_name = directory_display_name;
    new_message.claimed_nip05 = directory_claimed_nip05;
    new_message.names_data = directory;
  }
#endif
#if GROUNDHOG_HAVE_MLS_UI
  new_message.create_marmot_dm = app_create_marmot_dm;
  new_message.create_dm_data = self;
#endif
  gh_new_message_attach(window, &new_message);
  /* Add Contact (nostrc-txnu): a separate dialog to add a contact by
   * npub / nostr: URI / NIP-05 without composing a message. */
  {
    GhAddContactConfig add_contact = {
      .conversations = self->conversations,
      .nip05 = self->nip05,
    };
#if GROUNDHOG_HAVE_OUTBOX
    if (directory) {
      add_contact.display_name = directory_display_name;
      add_contact.names_data = directory;
    }
#endif
    gh_add_contact_attach(window, &add_contact);
  }
#endif
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  gh_store_status_attach(gh_window_get_status(window), self->account_store);
  /* Stored rooms list their newest page; older pages come from the store
   * when the view asks (W13b review B1). */
  gh_store_status_attach_history(window, self->account_store);
#elif GROUNDHOG_HAVE_INBOX
  /* Without the encrypted store every message is in memory only: say so
   * with the in-memory banner (charter §3.4, P4). */
  gh_status_set_store(gh_window_get_status(window), GH_STATUS_STORE_EPHEMERAL, NULL);
#endif
#if GROUNDHOG_HAVE_ONBOARDING
  /* First-run onboarding and the banners' [Set Up] (charter G14): it
   * contacts no relay before the user confirms one. */
  GhInboxSetupConfig onboarding = {
    .accounts = self->accounts,
    .account_relays = self->relays,
    .settings = self->settings,
    /* Encrypted groups find people through their kind 10002 (nostrc-0bdg):
     * offered, with consent, only when they run and the account has none. */
    .offer_relay_list = GH_FEATURE_ENCRYPTED_GROUPS,
  };
  gh_onboarding_attach(window, &onboarding);
#endif
#if GROUNDHOG_HAVE_NOTIFIER
  gh_notifier_attach_window(self->notifier, window);
#endif
  /* The composer (G13): sending through the account's durable outbox. */
#if GROUNDHOG_HAVE_INBOX && GROUNDHOG_HAVE_OUTBOX
  GhSendUiConfig send = {
    .accounts = self->accounts,
    .conversations = self->conversations,
    .account_store = self->account_store,
    .inbox = self->inbox,
    .settings = self->settings,
#if GROUNDHOG_HAVE_EXPIRY
    .expiry = self->expiry,
#endif
  };
  gh_send_ui_attach(window, &send);
#if GROUNDHOG_HAVE_MLS_UI && GROUNDHOG_HAVE_GROUP_UI
  /* Shared NIP-88 projection and composer for private, relay-group and
   * Marmot conversations. Attach before the legacy MLS UI so it can leave
   * the row enricher and poll button to this transport-neutral owner. */
  GhPollUiConfig polls = {
    .account_store = self->account_store,
    .mls_service = mls_ui_service,
    .nip29_service = group_ui_service,
    .service_data = self,
  };
  gh_poll_ui_attach(window, &polls);
#endif
  /* W26 slice B (nostrc-191r): reaction chips on message bubbles. Set the
   * current reaction store (may be NULL before the account store opens;
   * reactions_share() re-sets it when it does). */
  {
    GhReactionStore *reactions = gh_app_outbox_get_reactions(self->outbox);
    GhContentPage *content = gh_window_get_content(window);
    GtkWidget *view = gh_content_page_get_view(content);
    if (GH_IS_CONVERSATION_VIEW(view)) {
      gh_conversation_view_set_reaction_store(GH_CONVERSATION_VIEW(view), reactions);
      gh_conversation_view_set_reaction_func(GH_CONVERSATION_VIEW(view),
                                             on_react, self, NULL);
    }
  }
#if GROUNDHOG_HAVE_ATTACHMENTS
  /* G22: the attach button, the sheet and the attachment cards. */
  GhAttachmentUiConfig attachments = {
    .account_store = self->account_store,
    .conversations = self->conversations,
    .attachments = self->attachments,
    .settings = self->settings,
    .allow_onion = GH_FEATURE_TOR,
  };
  gh_attachment_ui_attach(window, &attachments);
#endif
#else
  gh_composer_set_disabled_reason(
    gh_content_page_get_composer(gh_window_get_content(window)),
    _("Sending needs the encrypted message store, which this build doesn't have."));
#endif
#if GROUNDHOG_HAVE_CONVERSATION_INFO
  gh_conversation_info_attach(window, conversation_info_services, self, NULL);
  /* The conversation rows' context menu (nostrc-qp24.74). */
  gh_conversation_menu_attach(window, conversation_info_services, self, NULL);
#endif
#if GROUNDHOG_HAVE_GROUP_UI
  /* G20b: after the send UI (its composer delegate) and the store's history
   * source (which it replaces with one that also pages relay groups). */
  GhGroupUiConfig groups = {
    .conversations = self->conversations,
    .service = group_ui_service,
    .service_data = self,
    .state_source = G_OBJECT(self->account_store),
    .display_name = group_ui_name,
    .names_data = self,
    .load_older = group_ui_load_older,
    .load_newer = group_ui_load_newer,
    .reset_latest = group_ui_reset_latest,
    .load_older_data = self,
  };
  gh_group_ui_attach(window, &groups);
#if GROUNDHOG_HAVE_MLS_UI
  /* Encrypted groups (qp24.13 part 2), only while the flag is on
   * (gh-features.h; the guard is gh_mls_ui_attach_if_enabled(), tested):
   * after the group UI, whose New Group it extends. */
  {
    GhMlsUiConfig mls = {
      .conversations = self->conversations,
      .accounts = self->accounts,
      .settings = self->settings,
      .service = mls_ui_service,
      .service_data = self,
      .state_source = G_OBJECT(self->account_store),
      .display_name = group_ui_name,
      .names_data = self,
      .account_relays = self->relays,
#if GROUNDHOG_HAVE_MLS_FILES
      .files = self->mls_files,
#endif
    };
    gboolean mls_on = gh_mls_ui_attach_if_enabled(window, &mls);
#if GROUNDHOG_HAVE_CONVERSATION_INFO
    if (mls_on)
      gh_conversation_info_set_encrypted_group_handler(window, gh_mls_ui_show_info, NULL);
#endif
#if GROUNDHOG_HAVE_MLS_FILES
    /* Files in encrypted groups: the attach button, sheet and cards. */
    if (mls_on)
      gh_mls_attachment_ui_attach(window, self->mls_files);
#endif
  }
#endif
#if GROUNDHOG_HAVE_CONVERSATION_INFO
  gh_conversation_info_set_group_handler(window, gh_group_ui_show_info, NULL);
#endif
#endif
}

GSettings *
gh_app_services_get_settings(GhAppServices *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  return self->settings;
}

GObject *
gh_app_services_get_accounts(GhAppServices *self)
{
  g_return_val_if_fail(self != NULL, NULL);
#if GROUNDHOG_HAVE_ACCOUNTS
  return G_OBJECT(self->accounts);
#else
  return NULL;
#endif
}

GObject *
gh_app_services_get_conversations(GhAppServices *self)
{
  g_return_val_if_fail(self != NULL, NULL);
#if GROUNDHOG_HAVE_INBOX
  return G_OBJECT(self->conversations);
#else
  return NULL;
#endif
}

GObject *
gh_app_services_get_inbox(GhAppServices *self)
{
  g_return_val_if_fail(self != NULL, NULL);
#if GROUNDHOG_HAVE_INBOX
  return G_OBJECT(self->inbox);
#else
  return NULL;
#endif
}

GObject *
gh_app_services_get_account_store(GhAppServices *self)
{
  g_return_val_if_fail(self != NULL, NULL);
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  return G_OBJECT(self->account_store);
#else
  return NULL;
#endif
}

#ifdef GH_MLS_TEST_HOOKS
GObject *
gh_app_services_get_mls_service(GhAppServices *self)
{
  g_return_val_if_fail(self != NULL, NULL);
#if GROUNDHOG_HAVE_OUTBOX
  return gh_app_outbox_get_mls_service(self->outbox);
#else
  return NULL;
#endif
}

GObject *
gh_app_services_get_account_relays(GhAppServices *self)
{
  g_return_val_if_fail(self != NULL, NULL);
#if GROUNDHOG_HAVE_RELAYS
  return G_OBJECT(self->relays);
#else
  return NULL;
#endif
}
#endif
