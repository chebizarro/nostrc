/**
 * GnostrNwcConnect - NIP-47 Nostr Wallet Connect Dialog
 *
 * Dialog for connecting to a remote lightning wallet via NWC.
 */

#include "gnostr-nwc-connect.h"
#include "../util/nwc.h"
#include <glib/gi18n.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-nwc-connect.ui"

struct _GnostrNwcConnect {
  GtkWindow parent_instance;

  /* Template children */
  GtkWidget *stack;
  GtkWidget *page_disconnected;
  GtkWidget *page_connected;

  /* Disconnected page widgets */
  GtkWidget *entry_connection_uri;
  GtkWidget *btn_connect;
  GtkWidget *btn_paste;
  GtkWidget *spinner_connect;

  /* Connected page widgets */
  GtkWidget *lbl_wallet_pubkey;
  GtkWidget *lbl_relay;
  GtkWidget *lbl_lud16;
  GtkWidget *lbl_balance;
  GtkWidget *btn_disconnect;
  GtkWidget *btn_refresh_balance;
  GtkWidget *spinner_balance;

  /* Toast */
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;

  /* State */
  gboolean connecting;
  gboolean fetching_balance;
};

G_DEFINE_TYPE(GnostrNwcConnect, gnostr_nwc_connect, GTK_TYPE_WINDOW)

/* Signals */
enum {
  SIGNAL_WALLET_CONNECTED,
  SIGNAL_WALLET_DISCONNECTED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_connect_clicked(GtkButton *btn, gpointer user_data);
static void on_disconnect_clicked(GtkButton *btn, gpointer user_data);
static void on_paste_clicked(GtkButton *btn, gpointer user_data);
static void on_refresh_balance_clicked(GtkButton *btn, gpointer user_data);
static void on_close_clicked(GtkButton *btn, gpointer user_data);
static void update_ui_for_state(GnostrNwcConnect *self);

static void show_toast(GnostrNwcConnect *self, const char *msg) {
  if (!self->toast_label || !self->toast_revealer) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
  /* Auto-hide after 3 seconds */
  g_timeout_add_seconds(3, (GSourceFunc)gtk_revealer_set_reveal_child,
                        self->toast_revealer);
}

static void gnostr_nwc_connect_dispose(GObject *obj) {
  GnostrNwcConnect *self = GNOSTR_NWC_CONNECT(obj);
  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_NWC_CONNECT);
  G_OBJECT_CLASS(gnostr_nwc_connect_parent_class)->dispose(obj);
}

static void gnostr_nwc_connect_class_init(GnostrNwcConnectClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_nwc_connect_dispose;

  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

  /* Bind template children */
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, stack);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, page_disconnected);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, page_connected);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, entry_connection_uri);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, btn_connect);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, btn_paste);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, spinner_connect);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, lbl_wallet_pubkey);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, lbl_relay);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, lbl_lud16);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, lbl_balance);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, btn_disconnect);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, btn_refresh_balance);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, spinner_balance);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, toast_revealer);
  gtk_widget_class_bind_template_child(wclass, GnostrNwcConnect, toast_label);

  /* Bind template callbacks */
  gtk_widget_class_bind_template_callback(wclass, on_connect_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_disconnect_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_paste_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_refresh_balance_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_close_clicked);

  /**
   * GnostrNwcConnect::wallet-connected:
   * @self: the NWC connect dialog
   *
   * Emitted when a wallet connection is established.
   */
  signals[SIGNAL_WALLET_CONNECTED] = g_signal_new(
    "wallet-connected",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  /**
   * GnostrNwcConnect::wallet-disconnected:
   * @self: the NWC connect dialog
   *
   * Emitted when the wallet is disconnected.
   */
  signals[SIGNAL_WALLET_DISCONNECTED] = g_signal_new(
    "wallet-disconnected",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void gnostr_nwc_connect_init(GnostrNwcConnect *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->connecting = FALSE;
  self->fetching_balance = FALSE;

  /* Connect button signals */
  g_signal_connect(self->btn_connect, "clicked", G_CALLBACK(on_connect_clicked), self);
  g_signal_connect(self->btn_disconnect, "clicked", G_CALLBACK(on_disconnect_clicked), self);
  g_signal_connect(self->btn_paste, "clicked", G_CALLBACK(on_paste_clicked), self);
  g_signal_connect(self->btn_refresh_balance, "clicked", G_CALLBACK(on_refresh_balance_clicked), self);

  /* Set initial UI state */
  update_ui_for_state(self);
}

GnostrNwcConnect *gnostr_nwc_connect_new(GtkWindow *parent) {
  GnostrNwcConnect *self = g_object_new(GNOSTR_TYPE_NWC_CONNECT,
                                        "transient-for", parent,
                                        "modal", TRUE,
                                        NULL);
  return self;
}

static void update_ui_for_state(GnostrNwcConnect *self) {
  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  gboolean connected = gnostr_nwc_service_is_connected(nwc);

  if (connected) {
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->page_connected);

    /* Update connected page info */
    const gchar *pubkey = gnostr_nwc_service_get_wallet_pubkey(nwc);
    if (pubkey) {
      /* Show truncated pubkey */
      gchar *truncated = g_strdup_printf("%.8s...%.8s", pubkey, pubkey + 56);
      gtk_label_set_text(GTK_LABEL(self->lbl_wallet_pubkey), truncated);
      g_free(truncated);
    }

    const gchar *relay = gnostr_nwc_service_get_relay(nwc);
    gtk_label_set_text(GTK_LABEL(self->lbl_relay), relay ? relay : "Not specified");

    const gchar *lud16 = gnostr_nwc_service_get_lud16(nwc);
    if (lud16) {
      gtk_label_set_text(GTK_LABEL(self->lbl_lud16), lud16);
      gtk_widget_set_visible(gtk_widget_get_parent(self->lbl_lud16), TRUE);
    } else {
      gtk_widget_set_visible(gtk_widget_get_parent(self->lbl_lud16), FALSE);
    }

    gtk_label_set_text(GTK_LABEL(self->lbl_balance), "Click refresh to fetch");

  } else {
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->page_disconnected);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_connection_uri), "");
  }

  /* Update button sensitivity */
  gtk_widget_set_sensitive(self->btn_connect, !self->connecting);
  gtk_widget_set_sensitive(self->entry_connection_uri, !self->connecting);
  gtk_widget_set_visible(self->spinner_connect, self->connecting);

  gtk_widget_set_sensitive(self->btn_refresh_balance, !self->fetching_balance);
  gtk_widget_set_visible(self->spinner_balance, self->fetching_balance);
}

void gnostr_nwc_connect_refresh(GnostrNwcConnect *self) {
  g_return_if_fail(GNOSTR_IS_NWC_CONNECT(self));
  update_ui_for_state(self);
}

static void on_connect_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrNwcConnect *self = GNOSTR_NWC_CONNECT(user_data);

  const gchar *uri = gtk_editable_get_text(GTK_EDITABLE(self->entry_connection_uri));
  if (!uri || !*uri) {
    show_toast(self, "Please enter a connection string");
    return;
  }

  self->connecting = TRUE;
  update_ui_for_state(self);

  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  GError *error = NULL;

  if (gnostr_nwc_service_connect(nwc, uri, &error)) {
    /* Save to settings */
    gnostr_nwc_service_save_to_settings(nwc);

    show_toast(self, "Wallet connected!");
    g_signal_emit(self, signals[SIGNAL_WALLET_CONNECTED], 0);
  } else {
    show_toast(self, error ? error->message : "Connection failed");
    g_clear_error(&error);
  }

  self->connecting = FALSE;
  update_ui_for_state(self);
}

static void on_disconnect_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrNwcConnect *self = GNOSTR_NWC_CONNECT(user_data);

  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  gnostr_nwc_service_disconnect(nwc);

  show_toast(self, "Wallet disconnected");
  g_signal_emit(self, signals[SIGNAL_WALLET_DISCONNECTED], 0);

  update_ui_for_state(self);
}

/* Clipboard paste callback */
static void on_clipboard_read_finish(GObject *source, GAsyncResult *result, gpointer user_data) {
  GdkClipboard *clipboard = GDK_CLIPBOARD(source);
  GnostrNwcConnect *self = GNOSTR_NWC_CONNECT(user_data);

  GError *error = NULL;
  gchar *text = gdk_clipboard_read_text_finish(clipboard, result, &error);

  if (text) {
    /* Check if it looks like a NWC URI */
    if (g_str_has_prefix(text, "nostr+walletconnect://")) {
      gtk_editable_set_text(GTK_EDITABLE(self->entry_connection_uri), text);
    } else {
      show_toast(self, "Clipboard doesn't contain a NWC URI");
    }
    g_free(text);
  } else {
    show_toast(self, "Failed to read clipboard");
    g_clear_error(&error);
  }

  g_object_unref(self);
}

static void on_paste_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrNwcConnect *self = GNOSTR_NWC_CONNECT(user_data);

  GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_read_text_async(clipboard, NULL, on_clipboard_read_finish, g_object_ref(self));
}

/* Balance refresh callback */
static void on_balance_finish(GObject *source, GAsyncResult *result, gpointer user_data) {
  GnostrNwcService *nwc = GNOSTR_NWC_SERVICE(source);
  GnostrNwcConnect *self = GNOSTR_NWC_CONNECT(user_data);

  self->fetching_balance = FALSE;

  GError *error = NULL;
  gint64 balance_msat = 0;

  if (gnostr_nwc_service_get_balance_finish(nwc, result, &balance_msat, &error)) {
    gchar *formatted = gnostr_nwc_format_balance(balance_msat);
    gtk_label_set_text(GTK_LABEL(self->lbl_balance), formatted);
    g_free(formatted);
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_balance), "Unable to fetch balance");
    if (error) {
      show_toast(self, error->message);
      g_clear_error(&error);
    }
  }

  update_ui_for_state(self);
  g_object_unref(self);
}

static void on_refresh_balance_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrNwcConnect *self = GNOSTR_NWC_CONNECT(user_data);

  self->fetching_balance = TRUE;
  update_ui_for_state(self);

  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  gnostr_nwc_service_get_balance_async(nwc, NULL, on_balance_finish, g_object_ref(self));
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrNwcConnect *self = GNOSTR_NWC_CONNECT(user_data);
  gtk_window_close(GTK_WINDOW(self));
}
