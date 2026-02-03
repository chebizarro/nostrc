/**
 * GnostrZapDialog - NIP-57 Zap Dialog
 *
 * Dialog for selecting zap amount and sending lightning zaps.
 */

#include "gnostr-zap-dialog.h"
#include "../util/zap.h"
#include "../util/nwc.h"
#include "../ipc/signer_ipc.h"
#include "../ipc/gnostr-signer-service.h"
#include <glib/gi18n.h>
#include <nostr/nip19/nip19.h>

/* QR code generation - using qrencode if available */
#ifdef HAVE_QRENCODE
#include <qrencode.h>
#endif

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
  GtkWidget *qr_box;
  GtkWidget *qr_frame;
  GtkWidget *qr_picture;
  GtkWidget *lbl_qr_title;
  GtkWidget *lbl_invoice;
  GtkWidget *btn_copy_invoice;

  /* State */
  gchar *recipient_pubkey;
  gchar *recipient_name;
  gchar *lud16;
  gchar *event_id;
  gint event_kind;
  gchar **relays;
  gint64 selected_amount_sats;
  gboolean is_processing;
  gboolean use_qr_fallback;    /* TRUE when NWC not available, show QR code */
  gchar *current_invoice;      /* BOLT11 invoice for copy button */

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
static void on_copy_invoice_clicked(GtkButton *btn, gpointer user_data);
static void update_zap_button_label(GnostrZapDialog *self);
static void set_processing(GnostrZapDialog *self, gboolean processing, const gchar *status);
static void show_qr_invoice(GnostrZapDialog *self, const gchar *bolt11_invoice);

#ifdef HAVE_QRENCODE
static GdkTexture *generate_qr_texture(const char *data) {
  QRcode *qr = QRcode_encodeString(data, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
  if (!qr) return NULL;

  /* Create a pixbuf with border */
  int border = 4;
  int scale = 4;
  int size = (qr->width + border * 2) * scale;

  guchar *pixels = g_malloc(size * size * 3);
  memset(pixels, 255, size * size * 3); /* white background */

  for (int y = 0; y < qr->width; y++) {
    for (int x = 0; x < qr->width; x++) {
      if (qr->data[y * qr->width + x] & 1) {
        /* Black module */
        for (int sy = 0; sy < scale; sy++) {
          for (int sx = 0; sx < scale; sx++) {
            int px = (x + border) * scale + sx;
            int py = (y + border) * scale + sy;
            int idx = (py * size + px) * 3;
            pixels[idx] = 0;
            pixels[idx + 1] = 0;
            pixels[idx + 2] = 0;
          }
        }
      }
    }
  }

  QRcode_free(qr);

  GBytes *bytes = g_bytes_new_take(pixels, size * size * 3);
  GdkTexture *texture = gdk_memory_texture_new(size, size, GDK_MEMORY_R8G8B8, bytes, size * 3);
  g_bytes_unref(bytes);

  return texture;
}
#endif

/* LEGITIMATE TIMEOUT - Toast auto-hide after 3 seconds.
 * nostrc-b0h: Audited - standard toast UX pattern. */
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
  g_clear_pointer(&self->current_invoice, g_free);
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
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, qr_box);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, qr_frame);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, qr_picture);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, lbl_qr_title);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, lbl_invoice);
  gtk_widget_class_bind_template_child(wclass, GnostrZapDialog, btn_copy_invoice);

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

  /* Connect copy invoice button */
  g_signal_connect(self->btn_copy_invoice, "clicked", G_CALLBACK(on_copy_invoice_clicked), self);

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

    /* LEGITIMATE TIMEOUT - Auto-close after success feedback.
     * nostrc-b0h: Audited - brief delay for user to see success is appropriate. */
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

/* Copy invoice to clipboard */
static void on_copy_invoice_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(user_data);

  if (!self->current_invoice) return;

  GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_set_text(clipboard, self->current_invoice);
  show_toast(self, "Invoice copied!");
}

/* Show invoice as QR code for external wallet payment */
static void show_qr_invoice(GnostrZapDialog *self, const gchar *bolt11_invoice) {
  g_clear_pointer(&self->current_invoice, g_free);
  self->current_invoice = g_strdup(bolt11_invoice);

  /* Show QR code section, hide zap button and amount presets */
  if (GTK_IS_WIDGET(self->qr_box)) {
    gtk_widget_set_visible(self->qr_box, TRUE);
  }
  if (GTK_IS_WIDGET(self->btn_zap)) {
    gtk_widget_set_visible(self->btn_zap, FALSE);
  }
  if (GTK_IS_WIDGET(self->preset_flow)) {
    gtk_widget_set_visible(self->preset_flow, FALSE);
  }
  if (GTK_IS_WIDGET(self->custom_amount_box)) {
    gtk_widget_set_visible(self->custom_amount_box, FALSE);
  }

  /* Show truncated invoice */
  if (GTK_IS_LABEL(self->lbl_invoice) && bolt11_invoice) {
    gsize len = strlen(bolt11_invoice);
    if (len > 20) {
      gchar *truncated = g_strdup_printf("%.10s...%.10s", bolt11_invoice, bolt11_invoice + len - 10);
      gtk_label_set_text(GTK_LABEL(self->lbl_invoice), truncated);
      g_free(truncated);
    } else {
      gtk_label_set_text(GTK_LABEL(self->lbl_invoice), bolt11_invoice);
    }
  }

#ifdef HAVE_QRENCODE
  /* Generate and display QR code - uppercase for better QR density */
  gchar *upper_invoice = g_ascii_strup(bolt11_invoice, -1);
  GdkTexture *texture = generate_qr_texture(upper_invoice);
  g_free(upper_invoice);

  if (texture && GTK_IS_PICTURE(self->qr_picture)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->qr_picture), GDK_PAINTABLE(texture));
    g_object_unref(texture);
  }
#else
  /* No QR code library - just show invoice text */
  if (GTK_IS_LABEL(self->lbl_qr_title)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_qr_title), "Copy Invoice");
  }
  if (GTK_IS_WIDGET(self->qr_frame)) {
    gtk_widget_set_visible(self->qr_frame, FALSE);
  }
#endif

  set_processing(self, FALSE, NULL);
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

  /* Check if we're in QR fallback mode (no NWC) */
  if (self->use_qr_fallback) {
    show_qr_invoice(self, bolt11_invoice);
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

/* Context for async zap signing operation */
typedef struct {
  GnostrZapDialog *dialog;
  gint64 amount_msat;
} ZapSignContext;

static void zap_sign_context_free(ZapSignContext *ctx) {
  if (!ctx) return;
  g_free(ctx);
}

/* Callback when signer returns signed zap request */
static void on_zap_request_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  ZapSignContext *ctx = (ZapSignContext *)user_data;
  (void)source;
  if (!ctx) return;

  GnostrZapDialog *self = ctx->dialog;
  if (!GNOSTR_IS_ZAP_DIALOG(self)) {
    zap_sign_context_free(ctx);
    return;
  }

  GError *error = NULL;
  gchar *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    set_processing(self, FALSE, NULL);
    gchar *msg = g_strdup_printf("Signing failed: %s", error ? error->message : "unknown error");
    show_toast(self, msg);
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, msg);
    g_free(msg);
    g_clear_error(&error);
    g_object_unref(self);
    zap_sign_context_free(ctx);
    return;
  }

  g_debug("[ZAP] Signed zap request: %.100s...", signed_event_json);

  /* Now request the invoice with the signed zap request */
  set_processing(self, TRUE, "Requesting invoice...");

  gnostr_zap_request_invoice_async(self->lnurl_info, signed_event_json, ctx->amount_msat,
                                   on_invoice_received, self, self->cancellable);

  g_free(signed_event_json);
  zap_sign_context_free(ctx);
  /* Note: self reference transferred to invoice callback */
}

/* Callback when signer returns sender pubkey */
static void on_get_pubkey_for_zap(GObject *source, GAsyncResult *res, gpointer user_data);

/* Forward declaration for helper that initiates signing after pubkey retrieval */
static void initiate_zap_signing(GnostrZapDialog *self, const gchar *sender_pubkey, gint64 amount_msat);

/**
 * initiate_zap_signing:
 * @self: the zap dialog
 * @sender_pubkey: sender's public key (hex)
 * @amount_msat: amount in millisatoshis
 *
 * Creates an unsigned zap request and sends it to the signer for signing.
 */
static void initiate_zap_signing(GnostrZapDialog *self, const gchar *sender_pubkey, gint64 amount_msat) {
  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    set_processing(self, FALSE, NULL);
    show_toast(self, "Signer not available");
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, "Signer not available");
    g_object_unref(self);
    return;
  }

  /* Create zap request */
  const gchar *comment = gtk_editable_get_text(GTK_EDITABLE(self->entry_comment));

  GnostrZapRequest req = {
    .recipient_pubkey = self->recipient_pubkey,
    .event_id = self->event_id,
    .lnurl = NULL,  /* bech32-encoding lud16 would require additional library */
    .lud16 = self->lud16,
    .amount_msat = amount_msat,
    .comment = (gchar *)(comment && *comment ? comment : NULL),
    .relays = self->relays,
    .event_kind = self->event_kind
  };

  gchar *unsigned_event_json = gnostr_zap_create_request_event(&req, sender_pubkey);

  if (!unsigned_event_json) {
    set_processing(self, FALSE, NULL);
    show_toast(self, "Failed to create zap request");
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, "Failed to create zap request");
    g_object_unref(self);
    return;
  }

  g_debug("[ZAP] Unsigned zap request: %s", unsigned_event_json);

  set_processing(self, TRUE, "Signing zap request...");

  /* Create async context */
  ZapSignContext *ctx = g_new0(ZapSignContext, 1);
  ctx->dialog = self;
  ctx->amount_msat = amount_msat;

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    unsigned_event_json,
    "",        /* current_user: ignored */
    "gnostr",  /* app_id: ignored */
    self->cancellable,
    on_zap_request_sign_complete,
    ctx
  );

  g_free(unsigned_event_json);
  /* Note: self reference transferred to callback */
}

/* Callback when signer returns sender pubkey */
static void on_get_pubkey_for_zap(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrZapDialog *self = GNOSTR_ZAP_DIALOG(user_data);
  if (!GNOSTR_IS_ZAP_DIALOG(self)) {
    return;
  }

  NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

  GError *error = NULL;
  gchar *npub = NULL;
  gboolean ok = nostr_org_nostr_signer_call_get_public_key_finish(proxy, &npub, res, &error);

  if (!ok || !npub || !*npub) {
    set_processing(self, FALSE, NULL);
    gchar *msg = g_strdup_printf("Failed to get pubkey: %s", error ? error->message : "unknown error");
    show_toast(self, msg);
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, msg);
    g_free(msg);
    g_clear_error(&error);
    g_object_unref(self);
    return;
  }

  /* The signer returns npub (bech32) or hex pubkey - we need hex.
   * If it starts with "npub1", decode it using NIP-19; otherwise assume hex. */
  gchar *sender_pubkey_hex = NULL;
  if (g_str_has_prefix(npub, "npub1")) {
    /* Decode npub to 32-byte pubkey, then convert to hex */
    uint8_t pubkey_bytes[32];
    if (nostr_nip19_decode_npub(npub, pubkey_bytes) == 0) {
      /* Convert bytes to hex string */
      GString *hex = g_string_sized_new(64);
      for (int i = 0; i < 32; i++) {
        g_string_append_printf(hex, "%02x", pubkey_bytes[i]);
      }
      sender_pubkey_hex = g_string_free(hex, FALSE);
      g_debug("[ZAP] Decoded npub to hex: %s", sender_pubkey_hex);
    } else {
      set_processing(self, FALSE, NULL);
      show_toast(self, "Failed to decode signer public key");
      g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, "Failed to decode signer public key");
      g_free(npub);
      g_object_unref(self);
      return;
    }
  } else {
    /* Assume hex */
    sender_pubkey_hex = g_strdup(npub);
  }
  g_free(npub);

  gint64 amount_msat = gnostr_zap_sats_to_msat(get_selected_amount_sats(self));

  /* Now initiate signing with the pubkey */
  initiate_zap_signing(self, sender_pubkey_hex, amount_msat);
  g_free(sender_pubkey_hex);
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

  set_processing(self, TRUE, "Getting sender identity...");

  /* Get the sender's pubkey from the signer via D-Bus IPC */
  GError *proxy_err = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&proxy_err);
  if (!proxy) {
    set_processing(self, FALSE, NULL);
    gchar *msg = g_strdup_printf("Signer not available: %s", proxy_err ? proxy_err->message : "not connected");
    show_toast(self, msg);
    g_signal_emit(self, signals[SIGNAL_ZAP_FAILED], 0, msg);
    g_free(msg);
    g_clear_error(&proxy_err);
    g_object_unref(self);
    return;
  }

  /* Request pubkey asynchronously */
  nostr_org_nostr_signer_call_get_public_key(
    proxy,
    self->cancellable,
    on_get_pubkey_for_zap,
    self
  );
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

  /* Check NWC is connected - if not, use QR fallback mode */
  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  self->use_qr_fallback = !gnostr_nwc_service_is_connected(nwc);

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

/**
 * gnostr_zap_dialog_show:
 * @parent: Parent window
 * @pubkey_hex: Recipient's public key (hex)
 * @lud16: Recipient's lightning address (e.g., "user@domain.com")
 * @event_id: (nullable): Event ID being zapped (hex), or NULL for profile zap
 *
 * Convenience function to create, configure, and present a zap dialog.
 * This is the primary entry point for initiating a zap from anywhere in the app.
 *
 * Returns: (transfer full): The presented zap dialog
 */
GnostrZapDialog *gnostr_zap_dialog_show(GtkWindow *parent,
                                        const gchar *pubkey_hex,
                                        const gchar *lud16,
                                        const gchar *event_id) {
  g_return_val_if_fail(pubkey_hex != NULL, NULL);
  g_return_val_if_fail(lud16 != NULL, NULL);

  GnostrZapDialog *dialog = gnostr_zap_dialog_new(parent);

  gnostr_zap_dialog_set_recipient(dialog, pubkey_hex, NULL, lud16);

  if (event_id && *event_id) {
    gnostr_zap_dialog_set_event(dialog, event_id, 1); /* kind 1 = text note */
  }

  gtk_window_present(GTK_WINDOW(dialog));

  return dialog;
}
