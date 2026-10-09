#include "gh-store-status.h"
#include "gh-conversation-list.h"

#include <glib/gi18n.h>

#define ADAPTER_DATA "groundhog-store-status"

GhStatusStore
gh_store_status_map(GhAccountStoreState state)
{
  switch (state) {
  case GH_ACCOUNT_STORE_OPENING: return GH_STATUS_STORE_OPENING;
  case GH_ACCOUNT_STORE_OPEN: return GH_STATUS_STORE_OPEN;
  case GH_ACCOUNT_STORE_EPHEMERAL: return GH_STATUS_STORE_EPHEMERAL;
  case GH_ACCOUNT_STORE_LOCKED: return GH_STATUS_STORE_LOCKED;
  case GH_ACCOUNT_STORE_UNAVAILABLE: return GH_STATUS_STORE_UNAVAILABLE;
  case GH_ACCOUNT_STORE_KEY_MISSING: return GH_STATUS_STORE_KEY_MISSING;
  case GH_ACCOUNT_STORE_CORRUPT: return GH_STATUS_STORE_CORRUPT;
  case GH_ACCOUNT_STORE_ERROR: return GH_STATUS_STORE_ERROR;
  case GH_ACCOUNT_STORE_INACTIVE:
  default:
    return GH_STATUS_STORE_NONE;
  }
}

static void
weak_ref_free(gpointer data)
{
  g_weak_ref_clear(data);
  g_free(data);
}

static void
sync_status(GhStatus *status)
{
  GWeakRef *ref = g_object_get_data(G_OBJECT(status), ADAPTER_DATA);
  g_autoptr(GhAccountStore) store = g_weak_ref_get(ref);
  if (!store) {
    gh_status_set_store(status, GH_STATUS_STORE_NONE, NULL);
    return;
  }
  gh_status_set_store(status, gh_store_status_map(gh_account_store_get_state(store)),
                      gh_account_store_get_error(store));
}

void
gh_store_status_attach(GhStatus *status, GhAccountStore *store)
{
  g_return_if_fail(GH_IS_STATUS(status));
  g_return_if_fail(GH_IS_ACCOUNT_STORE(store));
  g_return_if_fail(g_object_get_data(G_OBJECT(status), ADAPTER_DATA) == NULL);
  GWeakRef *ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(ref, store);
  g_object_set_data_full(G_OBJECT(status), ADAPTER_DATA, ref, weak_ref_free);
  g_signal_connect_object(store, "changed", G_CALLBACK(sync_status), status,
                          G_CONNECT_SWAPPED);
  sync_status(status);
}

/* ---- older history ------------------------------------------------------------ */

static gboolean
load_older_page(GhConversation *conversation, GError **error, gpointer data)
{
  g_autoptr(GhAccountStore) store = g_weak_ref_get(data);
  if (!store) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                        "No message storage is open");
    return FALSE;
  }
  return gh_account_store_load_older(store, conversation, GH_CONVERSATION_WINDOW_PAGE,
                                     NULL, error);
}

static gboolean
load_newer_page(GhConversation *conversation, GError **error, gpointer data)
{
  g_autoptr(GhAccountStore) store = g_weak_ref_get(data);
  if (!store) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                        "No message storage is open");
    return FALSE;
  }
  return gh_account_store_load_newer(store, conversation, GH_CONVERSATION_WINDOW_PAGE,
                                     NULL, error);
}

static gboolean
reset_latest_page(GhConversation *conversation, GError **error, gpointer data)
{
  g_autoptr(GhAccountStore) store = g_weak_ref_get(data);
  if (!store) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                        "No message storage is open");
    return FALSE;
  }
  return gh_account_store_reset_latest(store, conversation, error);
}

void
gh_store_status_attach_history(GhWindow *window, GhAccountStore *store)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(GH_IS_ACCOUNT_STORE(store));
  GWeakRef *ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(ref, store);
  gh_conversation_list_set_history_source(window, load_older_page, ref, weak_ref_free);
  GWeakRef *window_ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(window_ref, store);
  gh_conversation_list_set_window_source(window, load_newer_page, reset_latest_page,
                                         window_ref, weak_ref_free);
}

/* ---- start fresh / reset storage (charter §3.4, §7.15 #16) ----------------------- */

typedef struct {
  GWeakRef store;
  GWeakRef parent;
} FreshAsk;

typedef struct {
  GWeakRef window; /* for the toast */
  gboolean reset;  /* CORRUPT's Reset Storage, else KEY_MISSING's Start Fresh */
} FreshOp;

static void
fresh_ask_free(gpointer data, GClosure *closure)
{
  FreshAsk *ask = data;
  (void)closure;
  g_weak_ref_clear(&ask->store);
  g_weak_ref_clear(&ask->parent);
  g_free(ask);
}

static void
fresh_done(GObject *source, GAsyncResult *result, gpointer data)
{
  FreshOp *op = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = gh_account_store_start_fresh_finish(GH_ACCOUNT_STORE(source), result, &error);
  g_autoptr(GObject) window = g_weak_ref_get(&op->window);
  g_autofree gchar *message = NULL;
  if (ok)
    message = g_strdup(op->reset ? _("Message storage was reset")
                                 : _("Started fresh: new messages are saved from now on"));
  else
    message = g_strdup_printf(op->reset ? _("Couldn't reset message storage: %s")
                                        : _("Couldn't start fresh: %s"),
                              error->message);
  g_weak_ref_clear(&op->window);
  g_free(op);
  if (!ok)
    g_message("%s", message);
  if (GH_IS_WINDOW(window))
    adw_toast_overlay_add_toast(gh_window_get_toasts(GH_WINDOW(window)), adw_toast_new(message));
}

static void
on_fresh_response(AdwAlertDialog *dialog, const gchar *response, FreshAsk *ask)
{
  (void)dialog;
  g_autoptr(GhAccountStore) store = g_weak_ref_get(&ask->store);
  if (!store)
    return;
  if (g_str_equal(response, "retry")) {
    gh_account_store_retry(store);
    return;
  }
  if (!g_str_equal(response, "start-fresh"))
    return;
  GhAccountStoreState state = gh_account_store_get_state(store);
  if (state != GH_ACCOUNT_STORE_KEY_MISSING && state != GH_ACCOUNT_STORE_CORRUPT)
    return; /* it changed while the user read the dialog */
  g_autoptr(GObject) parent = g_weak_ref_get(&ask->parent);
  FreshOp *op = g_new0(FreshOp, 1);
  g_weak_ref_init(&op->window, GH_IS_WINDOW(parent) ? parent : NULL);
  op->reset = state == GH_ACCOUNT_STORE_CORRUPT;
  gh_account_store_start_fresh_async(store, NULL, fresh_done, op);
}

AdwAlertDialog *
gh_store_status_confirm_start_fresh(GtkWidget *parent, GhAccountStore *store)
{
  g_return_val_if_fail(GTK_IS_WIDGET(parent), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(store), NULL);
  GhAccountStoreState state = gh_account_store_get_state(store);
  if (state != GH_ACCOUNT_STORE_KEY_MISSING && state != GH_ACCOUNT_STORE_CORRUPT)
    return NULL;
  gboolean corrupt = state == GH_ACCOUNT_STORE_CORRUPT;
  /* Charter §3.4: what is deleted, and what can and can't come back. */
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(adw_alert_dialog_new(
    corrupt ? _("Reset Message Storage?") : _("Start Fresh on This Device?"),
    corrupt ? _("Message storage on this device is damaged. Groundhog shows what it could "
                "still read, but it can't save or receive messages until the storage is reset. "
                "Resetting deletes everything stored on this device and starts over.\n\n"
                "Private messages can be downloaded again from your relays with your key; "
                "encrypted group history can't.")
            : _("The key to your saved messages is missing from the keyring, so they can't be "
                "opened. Starting fresh deletes them from this device and saves new messages "
                "from now on.\n\n"
                "Private messages can be downloaded again from your relays with your key; "
                "encrypted group history can't. If you can bring back the keyring that held "
                "the key, do that and choose Try Again instead.")));
  adw_alert_dialog_add_response(alert, "cancel", _("_Cancel"));
  if (!corrupt)
    adw_alert_dialog_add_response(alert, "retry", _("_Try Again"));
  adw_alert_dialog_add_response(alert, "start-fresh",
                                corrupt ? _("_Reset Storage") : _("_Start Fresh"));
  adw_alert_dialog_set_response_appearance(alert, "start-fresh", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  FreshAsk *ask = g_new0(FreshAsk, 1);
  g_weak_ref_init(&ask->store, store);
  g_weak_ref_init(&ask->parent, parent);
  g_signal_connect_data(alert, "response", G_CALLBACK(on_fresh_response), ask, fresh_ask_free,
                        0);
  adw_dialog_present(ADW_DIALOG(alert), parent);
  return alert;
}
