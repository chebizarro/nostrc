#include "gh-app-services.h"

#if GROUNDHOG_HAVE_ACCOUNTS
#include "gh-account-controller.h"
#include "gh-account-ui.h"
#endif
#if GROUNDHOG_HAVE_RELAYS
#include "gh-account-relays.h"
#endif
#if GROUNDHOG_HAVE_INBOX
#include "gh-conversation-list.h"
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
#include "gh-send-ui.h"
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
#if GROUNDHOG_HAVE_NOTIFIER
#include "gh-notifier.h"
#endif
#if GROUNDHOG_HAVE_NEW_MESSAGE
#include "gh-new-message-dialog.h"
#endif

/* G20b: NIP-29 relay groups (gh-group-ui.h). */
#ifndef GROUNDHOG_HAVE_GROUP_UI
#define GROUNDHOG_HAVE_GROUP_UI 0
#endif
#if GROUNDHOG_HAVE_GROUP_UI
#include "gh-group-ui.h"
#endif

#if GROUNDHOG_HAVE_ACCOUNTS
#include "gh-features.h"
#include "gh-identity.h"
#include "gh-preferences-dialog.h"
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
  GtkApplication *app; /* borrowed: owns the process's services */
  GSettings *settings;
#if GROUNDHOG_HAVE_ACCOUNTS && GROUNDHOG_HAVE_TOR
  GhNetSession *network; /* the network mode of every connection (G09) */
#endif
#if GROUNDHOG_HAVE_ACCOUNTS
  GhAccountController *accounts;
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

/* The active account, its signer and its generation. Without a session bus
 * the signer is reported unreachable, not faked. */
static gboolean
accounts_init(GhAppServices *self, GError **error)
{
  (void)error;
  self->accounts = gh_account_controller_new(self->settings,
    g_application_get_dbus_connection(G_APPLICATION(self->app)));
  return TRUE;
}

/* Revokes the account generation even if a listing is still in flight. */
static void
accounts_teardown(GhAppServices *self)
{
  dispose_object(&self->accounts);
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
  return TRUE;
}

/* Its REQs and pending signer unwraps are cancelled while the relay lists
 * and the account generation they belong to still exist. */
static void
inbox_teardown(GhAppServices *self)
{
  dispose_object(&self->inbox);
}
#endif

#if GROUNDHOG_HAVE_ACCOUNT_STORE
/* One GhStoreKey per process (per-account operations are ordered in it). */
static gboolean
store_key_init(GhAppServices *self, GError **error)
{
  (void)error;
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
                              state == GH_ACCOUNT_STORE_UNAVAILABLE);
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
  const gchar *npub = gh_account_controller_get_active_npub(accounts);
  GPtrArray *identities = gh_account_controller_get_identities(accounts);
  const gchar *label = NULL;
  for (guint i = 0; npub && identities && i < identities->len; i++) {
    GhIdentityInfo *info = g_ptr_array_index(identities, i);
    if (g_strcmp0(info->npub, npub) == 0)
      label = info->label;
  }
  gh_preferences_dialog_set_account(GH_PREFERENCES_DIALOG(dialog), npub, label);
}

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
  sync_preferences_account(self->accounts, dialog);
  g_signal_connect_object(self->accounts, "changed", G_CALLBACK(sync_preferences_account),
                          dialog, 0);
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
static const gchar *
directory_display_name(gpointer data, const gchar *pubkey)
{
  return gh_contact_directory_get_display_name(GH_CONTACT_DIRECTORY(data), pubkey);
}

static const gchar *
directory_claimed_nip05(gpointer data, const gchar *pubkey)
{
  return gh_contact_directory_get_nip05(GH_CONTACT_DIRECTORY(data), pubkey);
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

/* Private conversations' older pages, as gh_store_status_attach_history(). */
static gboolean
group_ui_load_older(GhConversation *conversation, GError **error, gpointer data)
{
  GhAppServices *self = data;
  return gh_account_store_load_older(self->account_store, conversation,
                                     GH_STORE_CONVERSATIONS_PAGE_SIZE, NULL, error);
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
    new_message.inboxes = GH_INBOX_RESOLVER(directory);
    new_message.display_name = directory_display_name;
    new_message.claimed_nip05 = directory_claimed_nip05;
    new_message.names_data = directory;
  }
#endif
  gh_new_message_attach(window, &new_message);
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
    .load_older_data = self,
  };
  gh_group_ui_attach(window, &groups);
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
