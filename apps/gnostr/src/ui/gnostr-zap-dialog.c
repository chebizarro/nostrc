/**
 * GnostrZapDialog - NIP-57 Zap Dialog
 *
 * Dialog for selecting zap amount and sending lightning zaps.
 */

#include "gnostr-zap-dialog.h"
#include "../util/zap.h"
#include "../util/nwc.h"
#include <glib/gi18n.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-zap-dialog.ui"

struct _GnostrZapDialog {
  GtkWindow parent_instance;

  /* Template children */
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;
  GtkWidget *lbl_recipient;
  GtkWidget *lbl_lud16;
  GtkWidget *preset_flow;
  GtkWidget *btn_21;
  GtkWidget *btn_100;
  GtkWidget *btn_500;
  GtkWidget *btn_1k;
  GtkWidget *btn_5k;
  GtkWidget *btn_10k;
  GtkWidget *btn_21k;
  GtkWidget *btn_custom;
  GtkWidget *custom_amount_box;
  GtkWidget *entry_custom_amount;
  GtkWidget *entry_comment;
  GtkWidget *status_box;
  GtkWidget *spinner;
  GtkWidget *lbl_status;
  GtkWidget *btn_zap;
  GtkWidget *lbl_zap_button;

  /* State */
  gchar *recipient_pubkey;
  gchar *recipient_name;
  gchar *lud16;
  gchar *event_id;
  gint event_kind;
  gchar **relays;
  gint64 selected_amount_sats;
  gboolean is_processing;

  /* Async context */
  GCancellable *cancellable;
  GnostrLnurlPayInfo *lnurl_info;
};

G_DEFINE_TYPE(GnostrZapDialog, gnostr_zap_dialog, GTK_TYPE_WINDOW)

/* Signals */
enum {
  SIGNAL_ZAP_SENT,
  SIGNAL_ZAP_FAILED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_cancel_clicked(GtkButton *btn, gpointer user_data);
static void on_zap_clicked(GtkButton *btn, gpointer user_data);
static void on_amount_toggled(GtkToggleButton *btn, gpointer user_data);
static void update_zap_button_label(GnostrZapDialog *self);
static void set_processing(GnostrZapDialog *self, gboolean processing, const gchar *status);

static void show_toast(GnostrZapDialog *self, const gchar *msg) {
  if (!self->toast_label || !self->toast_revealer) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
  g_timeout_add_seconds(3, (GSourceFunc)gtk_revealer_set_reveal_child,
                        self->toast_revealer);
}

static void gnostr_zap_dialog_dispose(GObject *obj) {
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(obj);

  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }

  if (self->lnurl_info) {
    gnostr_lnurl_pay_info_free(self->lnurl_info);
    self->lnurl_info = NULL;
  }

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_ZAP_DIALOG);
  G_OBJECT_CLASS(gnostr_zap_dialog_parent_class)->dispose(obj);
}

static void gnostr_zap_dialog_finalize(GObject *obj) {
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(obj);

  g_clear_pointer(&self->recipient_pubkey, g_free);
  g_clear_pointer(&self->recipient_name, g_free);
  g_clear_pointer(&self->lud16, g_free);
  g_clear_pointer(&self->event_id, g_free);
  g_strfreev(self->relays);

  G_OBJECT_CLASS(gnostr_zap_dialog_parent_class)->finalize(obj);
}

static void gnostr_zap_dialog_class_init(GnostrZapDialogClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_zap_dialog_dispose;
  gclass->finalize = gnostr_zap_dialog_finalize;

  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

  /* Bind template children */
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, toast_revealer);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, toast_label);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, lbl_recipient);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, lbl_lud16);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, preset_flow);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_21);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_100);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_500);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_1k);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_5k);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_10k);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_21k);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_custom);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, custom_amount_box);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, entry_custom_amount);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, entry_comment);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, status_box);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, spinner);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, lbl_status);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_zap);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, lbl_zap_button);

  /* Bind template callbacks */
  gtk_widget_class_bind_template_callback(wclass, on_cancel_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_zap_clicked);

  /**
   * GnostrZapDialog::zap-sent:
   * @self: the zap dialog
   * @event_id: the event that was zapped
   * @amount_msat: amount in millisatoshis
   */
  signals[SIGNAL_ZAP_SENT] = g_signal_new(
    "zap-sent",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT64);

  /**
   * GnostrZapDialog::zap-failed:
   * @self: the zap dialog
   * @error_message: error description
   */
  signals[SIGNAL_ZAP_FAILED] = g_signal_new(
    "zap-failed",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_zap_dialog_init(GnostrZapDialog *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->selected_amount_sats = 1000;  /* Default to 1k sats */
  self->is_processing = FALSE;
  self->event_kind = 1;  /* Default to text note */

  /* Connect amount button signals */
  g_signal_connect(self->btn_21, "toggled", G_CALLBACK(on_amount_toggled), self);
  g_signal_connect(self->btn_100, "toggled", G_CALLBACK(on_amount_toggled), self);
  g_signal_connect(self->btn_500, "toggled", G_CALLBACK(on_amount_toggled), self);
  g_signal_connect(self->btn_1k, "toggled", G_CALLBACK(on_amount_toggled), self);
  g_signal_connect(self->btn_5k, "toggled", G_CALLBACK(on_amount_toggled), self);
  g_signal_connect(self->btn_10k, "toggled", G_CALLBACK(on_amount_toggled), self);
  g_signal_connect(self->btn_21k, "toggled", G_CALLBACK(on_amount_toggled), self);
  g_signal_connect(self->btn_custom, "toggled", G_CALLBACK(on_amount_toggled), self);

  /* Connect custom amount entry change */
  g_signal_connect_swapped(self->entry_custom_amount, "changed",
                           G_CALLBACK(update_zap_button_label), self);

  update_zap_button_label(self);
}

GnostrZapDialog *gnostr_zap_dialog_new(GtkWindow *parent) {
  GnostrZapDialog *self = g_object_new(GNOSTR_TYPE_ZAP_DIALOG,
                                       "transient-for", parent,
                                       "modal", TRUE,
                                       NULL);
  return self;
}

void gnostr_zap_dialog_set_recipient(GnostrZapDialog *self,
                                     const gchar *pubkey_hex,
                                     const gchar *display_name,
                                     const gchar *lud16) {
  g_return_if_fail(GNOSTR_IS_ZAP_DIALOG(self));

  g_clear_pointer(&self->recipient_pubkey, g_free);
  g_clear_pointer(&self->recipient_name, g_free);
  g_clear_pointer(&self->lud16, g_free);

  self->recipient_pubkey = g_strdup(pubkey_hex);
  self->recipient_name = g_strdup(display_name);
  self->lud16 = g_strdup(lud16);

  /* Update UI */
  if (GTK_IS_LABEL(self->lbl_recipient)) {
    if (display_name && *display_name) {
      gtk_label_set_text(GTK_LABEL(self->lbl_recipient), display_name);
    } else if (pubkey_hex) {
      gchar *truncated = g_strdup_printf("%.8s...%.8s", pubkey_hex, pubkey_hex + 56);
      gtk_label_set_text(GTK_LABEL(self->lbl_recipient), truncated);
      g_free(truncated);
    }
  }

  if (GTK_IS_LABEL(self->lbl_lud16)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_lud16), lud16 ? lud16 : "No lightning address");
  }

  /* Disable zap button if no lud16 */
  if (GTK_IS_WIDGET(self->btn_zap)) {
    gtk_widget_set_sensitive(self->btn_zap, lud16 != NULL && *lud16 != '\0');
  }
}

void gnostr_zap_dialog_set_event(GnostrZapDialog *self,
                                 const gchar *event_id,
                                 gint event_kind) {
  g_return_if_fail(GNOSTR_IS_ZAP_DIALOG(self));

  g_clear_pointer(&self->event_id, g_free);
  self->event_id = g_strdup(event_id);
  self->event_kind = event_kind;
}

void gnostr_zap_dialog_set_relays(GnostrZapDialog *self,
                                  const gchar * const *relays) {
  g_return_if_fail(GNOSTR_IS_ZAP_DIALOG(self));

  g_strfreev(self->relays);
  self->relays = g_strdupv((gchar **)relays);
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(user_data);

  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
  }

  gtk_window_close(GTK_WINDOW(self));
}

static void clear_other_toggles(GnostrZapDialog *self, GtkToggleButton *active) {
  /* Unselect other toggle buttons */
  GtkToggleButton *buttons[] = {
    GTK_TOGGLE_BUTTON(self->btn_21),
    GTK_TOGGLE_BUTTON(self->btn_100),
    GTK_TOGGLE_BUTTON(self->btn_500),
    GTK_TOGGLE_BUTTON(self->btn_1k),
    GTK_TOGGLE_BUTTON(self->btn_5k),
    GTK_TOGGLE_BUTTON(self->btn_10k),
    GTK_TOGGLE_BUTTON(self->btn_21k),
    GTK_TOGGLE_BUTTON(self->btn_custom),
  };

  for (gsize i = 0; i < G_N_ELEMENTS(buttons); i++) {
    if (buttons[i] != active && gtk_toggle_button_get_active(buttons[i])) {
      g_signal_handlers_block_by_func(buttons[i], on_amount_toggled, self);
      gtk_toggle_button_set_active(buttons[i], FALSE);
      g_signal_handlers_unblock_by_func(buttons[i], on_amount_toggled, self);
    }
  }
}

static void on_amount_toggled(GtkToggleButton *btn, gpointer user_data) {
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(user_data);

  if (!gtk_toggle_button_get_active(btn)) {
    /* Prevent deselecting - keep at least one selected */
    gtk_toggle_button_set_active(btn, TRUE);
    return;
  }

  /* Clear other toggles */
  clear_other_toggles(self, btn);

  /* Update selected amount */
  if (btn == GTK_TOGGLE_BUTTON(self->btn_21)) {
    self->selected_amount_sats = 21;
  } else if (btn == GTK_TOGGLE_BUTTON(self->btn_100)) {
    self->selected_amount_sats = 100;
  } else if (btn == GTK_TOGGLE_BUTTON(self->btn_500)) {
    self->selected_amount_sats = 500;
  } else if (btn == GTK_TOGGLE_BUTTON(self->btn_1k)) {
    self->selected_amount_sats = 1000;
  } else if (btn == GTK_TOGGLE_BUTTON(self->btn_5k)) {
    self->selected_amount_sats = 5000;
  } else if (btn == GTK_TOGGLE_BUTTON(self->btn_10k)) {
    self->selected_amount_sats = 10000;
  } else if (btn == GTK_TOGGLE_BUTTON(self->btn_21k)) {
    self->selected_amount_sats = 21000;
  } else if (btn == GTK_TOGGLE_BUTTON(self->btn_custom)) {
    /* Read from custom entry */
    self->selected_amount_sats = 0;
  }

  /* Show/hide custom amount entry */
  gboolean is_custom = (btn == GTK_TOGGLE_BUTTON(self->btn_custom));
  if (GTK_IS_WIDGET(self->custom_amount_box)) {
    gtk_widget_set_visible(self->custom_amount_box, is_custom);
    if (is_custom) {
      gtk_widget_grab_focus(self->entry_custom_amount);
    }
  }

  update_zap_button_label(self);
}

static gint64 get_selected_amount_sats(GnostrZapDialog *self) {
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_custom))) {
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(self->entry_custom_amount));
    if (text && *text) {
      return g_ascii_strtoll(text, NULL, 10);
    }
    return 0;
  }
  return self->selected_amount_sats;
}

static void update_zap_button_label(GnostrZapDialog *self) {
  if (!GTK_IS_LABEL(self->lbl_zap_button)) return;

  gint64 amount = get_selected_amount_sats(self);
  gchar *label = NULL;

  if (amount > 0) {
    if (amount >= 1000) {
      label = g_strdup_printf("Zap %'lld sats", (long long)amount);
    } else {
      label = g_strdup_printf("Zap %lld sats", (long long)amount);
    }
  } else {
    label = g_strdup("Zap");
  }

  gtk_label_set_text(GTK_LABEL(self->lbl_zap_button), label);
  g_free(label);

  /* Update button sensitivity */
  if (GTK_IS_WIDGET(self->btn_zap)) {
    gboolean can_zap = (amount > 0 && self->lud16 != NULL && !self->is_processing);
    gtk_widget_set_sensitive(self->btn_zap, can_zap);
  }
}

static void set_processing(GnostrZapDialog *self, gboolean processing, const gchar *status) {
  self->is_processing = processing;

  if (GTK_IS_WIDGET(self->status_box)) {
    gtk_widget_set_visible(self->status_box, processing);
  }

  if (GTK_IS_LABEL(self->lbl_status) && status) {
    gtk_label_set_text(GTK_LABEL(self->lbl_status), status);
  }

  if (GTK_IS_SPINNER(self->spinner)) {
    if (processing) {
      gtk_spinner_start(GTK_SPINNER(self->spinner));
    } else {
      gtk_spinner_stop(GTK_SPINNER(self->spinner));
    }
  }

  update_zap_button_label(self);
}

/* Payment callback */
static void on_payment_finish(GObject *source, GAsyncResult *result, gpointer user_data) {
  GnostrNwcService *nwc = GNOSTR_NWC_SERVICE(source);
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(user_data);

  GError *error = NULL;
  gchar *preimage = NULL;

  gboolean success = gnostr_nwc_service_pay_invoice_finish(nwc, result, &preimage, &error);

  set_processing(self, FALSE, NULL);

  if (success) {
    gint64 amount_msat = gnostr_zap_sats_to_msat(get_selected_amount_sats(self));
    g_signal_emit(self, signals[SIGNAL_ZAP_SENT], 0, self->event_id, amount_msat);
    show_toast(self, "Zap sent!");

    /* Close dialog after short delay */
    g_timeout_add(1500, (GSourceFunc)gtk_window_close, GTK_WINDOW(self));
  } else {
    const gchar *msg = error ? error->message : "Payment failed";
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, msg);
    show_toast(self, msg);
    g_clear_error(&error);
  }

  g_free(preimage);
  g_object_unref(self);
}

/* Invoice callback */
static void on_invoice_received(const gchar *bolt11_invoice, GError *error, gpointer user_data) {
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(user_data);

  if (error || !bolt11_invoice) {
    set_processing(self, FALSE, NULL);
    const gchar *msg = error ? error->message : "Failed to get invoice";
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, msg);
    show_toast(self, msg);
    g_object_unref(self);
    return;
  }

  /* Pay the invoice via NWC */
  set_processing(self, TRUE, "Paying invoice...");

  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  gint64 amount_msat = gnostr_zap_sats_to_msat(get_selected_amount_sats(self));

  gnostr_nwc_service_pay_invoice_async(nwc, bolt11_invoice, amount_msat,
                                       self->cancellable, on_payment_finish,
                                       self);
  /* Note: self reference transferred to callback */
}

/* LNURL info callback */
static void on_lnurl_info_received(GnostrLnurlPayInfo *info, GError *error, gpointer user_data) {
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(user_data);

  if (error || !info) {
    set_processing(self, FALSE, NULL);
    const gchar *msg = error ? error->message : "Failed to fetch LNURL info";
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, msg);
    show_toast(self, msg);
    g_object_unref(self);
    return;
  }

  /* Store LNURL info */
  if (self->lnurl_info) {
    gnostr_lnurl_pay_info_free(self->lnurl_info);
  }
  self->lnurl_info = info;

  /* Check if zaps are supported */
  if (!info->allows_nostr || !info->nostr_pubkey) {
    set_processing(self, FALSE, NULL);
    show_toast(self, "Recipient doesn't support NIP-57 zaps");
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, "NIP-57 zaps not supported");
    g_object_unref(self);
    return;
  }

  /* Validate amount */
  gint64 amount_msat = gnostr_zap_sats_to_msat(get_selected_amount_sats(self));
  if (amount_msat < info->min_sendable || amount_msat > info->max_sendable) {
    set_processing(self, FALSE, NULL);
    gchar *msg = g_strdup_printf("Amount out of range (%lld-%lld sats)",
                                 (long long)(info->min_sendable / 1000),
                                 (long long)(info->max_sendable / 1000));
    show_toast(self, msg);
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, msg);
    g_free(msg);
    g_object_unref(self);
    return;
  }

  set_processing(self, TRUE, "Creating zap request...");

  /* Get the sender's pubkey from NWC service (or we'd need signer integration) */
  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  const gchar *sender_pubkey = gnostr_nwc_service_get_wallet_pubkey(nwc);

  if (!sender_pubkey) {
    set_processing(self, FALSE, NULL);
    show_toast(self, "Wallet not connected");
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, "Wallet not connected");
    g_object_unref(self);
    return;
  }

  /* Create zap request */
  const gchar *comment = gtk_editable_get_text(GTK_EDITABLE(self->entry_comment));

  GnostrZapRequest req = {
    .recipient_pubkey = self->recipient_pubkey,
    .event_id = self->event_id,
    .lnurl = NULL,  /* We'd need to bech32-encode lud16 for full compliance */
    .lud16 = self->lud16,
    .amount_msat = amount_msat,
    .comment = (gchar *)(comment && *comment ? comment : NULL),
    .relays = self->relays,
    .event_kind = self->event_kind
  };

  gchar *zap_request_json = gnostr_zap_create_request_event(&req, sender_pubkey);

  if (!zap_request_json) {
    set_processing(self, FALSE, NULL);
    show_toast(self, "Failed to create zap request");
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, "Failed to create zap request");
    g_object_unref(self);
    return;
  }

  /* TODO: Sign the zap request via signer IPC */
  /* For now, we'll use the unsigned event - this won't work with real LNURL servers */
  /* but shows the flow. Real implementation needs NIP-46 signer integration. */

  set_processing(self, TRUE, "Requesting invoice...");

  gnostr_zap_request_invoice_async(self->lnurl_info, zap_request_json, amount_msat,
                                   on_invoice_received, self, self->cancellable);

  g_free(zap_request_json);
  /* Note: self reference transferred to callback */
}

static void on_zap_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(user_data);

  if (self->is_processing) return;

  /* Validate we have a lightning address */
  if (!self->lud16 || !*self->lud16) {
    show_toast(self, "No lightning address");
    return;
  }

  /* Check NWC is connected */
  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  if (!gnostr_nwc_service_is_connected(nwc)) {
    show_toast(self, "Connect your wallet first (Settings > Wallet)");
    return;
  }

  /* Validate amount */
  gint64 amount = get_selected_amount_sats(self);
  if (amount <= 0) {
    show_toast(self, "Please enter an amount");
    return;
  }

  /* Start the zap flow */
  set_processing(self, TRUE, "Fetching LNURL info...");

  /* Cancel any previous operation */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  self->cancellable = g_cancellable_new();

  /* Take a reference for the async callbacks */
  g_object_ref(self);

  gnostr_zap_fetch_lnurl_info_async(self->lud16, on_lnurl_info_received, self,
                                    self->cancellable);
}
