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
#if GROUNDHOG_HAVE_OUTBOX
#include "gh-app-outbox.h"
#endif
#if GROUNDHOG_HAVE_BACKGROUND
#include "gh-background.h"
#endif

#define GROUNDHOG_APP_ID "org.nostr.Groundhog"

struct _GhAppServices {
  GtkApplication *app; /* borrowed: owns the process's services */
  GSettings *settings;
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
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  GhStoreKey *store_key;
  GhAccountStore *account_store;
  GSimpleAction *store_actions[3];
#endif
#if GROUNDHOG_HAVE_BACKGROUND
  GhBackground *background;
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
  return TRUE;
}

static void
conversations_teardown(GhAppServices *self)
{
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
  /* No encrypted store in this build: messages stay in memory and the
   * seen-set in $XDG_STATE_HOME/groundhog/nip17. */
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
enum { ACTION_UNLOCK, ACTION_RETRY, ACTION_EPHEMERAL };
static const gchar *const store_action_names[] = {
  [ACTION_UNLOCK] = "store-unlock",
  [ACTION_RETRY] = "store-retry",
  [ACTION_EPHEMERAL] = "store-continue-without-saving",
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
  else if (!gh_account_store_continue_without_saving(self->account_store, &error))
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

typedef struct {
  const gchar *name;
  gboolean (*init)(GhAppServices *self, GError **error);
  void (*teardown)(GhAppServices *self);
} GhAppService;

static const GhAppService services[] = {
  { "application", NULL, NULL },
#if GROUNDHOG_HAVE_ACCOUNTS
  { "settings", settings_init, settings_teardown },
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
#if GROUNDHOG_HAVE_BACKGROUND
  { "background", background_init, background_teardown },
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
#if GROUNDHOG_HAVE_ACCOUNT_STORE
  gh_store_status_attach(gh_window_get_status(window), self->account_store);
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
