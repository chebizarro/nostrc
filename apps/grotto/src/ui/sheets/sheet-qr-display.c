/* sheet-qr-display.c - QR code display dialog implementation
 *
 * Shows a QR code with copy button and optional security warnings.
 */
#include "sheet-qr-display.h"
#include "../app-resources.h"
#include "../../qr-code.h"
#include <secure_buf.h>

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

/* QR code display size in pixels */
#define QR_DISPLAY_SIZE 280

struct _SheetQrDisplay {
  AdwDialog parent_instance;

  /* UI components */
  GtkWidget *header_bar;
  GtkWidget *content_box;
  GtkWidget *title_label;
  GtkWidget *qr_picture;
  GtkWidget *data_label;
  GtkWidget *type_label;
  GtkWidget *warning_box;
  GtkWidget *warning_label;
  GtkWidget *btn_copy;
  GtkWidget *btn_close;

  /* Current data */
  gchar *current_data;
  GnQrContentType current_type;
  GdkTexture *qr_texture;
  GDBusConnection *pairing_bus;
  gchar *pairing_id;
  guint expiry, copy_feedback, status_subscription, owner_watch;
  gboolean status_pending, status_dirty;
  GdkContentProvider *clipboard_content;
};

G_DEFINE_TYPE(SheetQrDisplay, sheet_qr_display, ADW_TYPE_DIALOG)

/* Forward declarations */
static void update_qr_display(SheetQrDisplay *self);
static void show_warning(SheetQrDisplay *self, const gchar *warning);
static void hide_warning(SheetQrDisplay *self);

/* Reset copy button callback */
static gboolean reset_copy_button_cb(gpointer data) {
  SheetQrDisplay *self = SHEET_QR_DISPLAY(data);
  self->copy_feedback = 0;
  if (self->current_data && GTK_IS_BUTTON(self->btn_copy)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_copy), "Copy to Clipboard");
    gtk_widget_remove_css_class(self->btn_copy, "success");
  }
  return G_SOURCE_REMOVE;
}

/* Copy data to clipboard */
static void copy_to_clipboard(SheetQrDisplay *self) {
  if (!self->current_data) return;

  GtkWidget *w = GTK_WIDGET(self);
  GdkDisplay *dpy = gtk_widget_get_display(w);
  if (dpy) {
    GdkClipboard *cb = gdk_display_get_clipboard(dpy);
    if (cb) {
      g_clear_object(&self->clipboard_content);
      self->clipboard_content = gdk_content_provider_new_typed(G_TYPE_STRING, self->current_data);
      gdk_clipboard_set_content(cb, self->clipboard_content);
    }
  }

  /* Show brief feedback */
  gtk_button_set_label(GTK_BUTTON(self->btn_copy), "Copied!");
  gtk_widget_add_css_class(self->btn_copy, "success");

  /* Reset after delay */
  if (self->copy_feedback) g_source_remove(self->copy_feedback);
  self->copy_feedback = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 2, reset_copy_button_cb,
                             g_object_ref(self), g_object_unref);
}

/* Button handlers */
static void on_copy_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetQrDisplay *self = SHEET_QR_DISPLAY(user_data);
  copy_to_clipboard(self);
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetQrDisplay *self = SHEET_QR_DISPLAY(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static void pairing_closed(AdwDialog *dialog, gpointer unused) {
  (void)unused; SheetQrDisplay *self = SHEET_QR_DISPLAY(dialog);
  if (!self->current_data && !self->pairing_id && !self->expiry && !self->clipboard_content && !self->copy_feedback && !self->status_subscription && !self->owner_watch) return;
  if (self->copy_feedback) { guint id = self->copy_feedback; self->copy_feedback = 0; g_source_remove(id); }
  if (self->expiry) { g_source_remove(self->expiry); self->expiry = 0; }
  if (self->pairing_id && self->pairing_bus) g_dbus_connection_call(self->pairing_bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "CancelPairing", g_variant_new("(s)", self->pairing_id), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
  if (self->status_subscription && self->pairing_bus) g_dbus_connection_signal_unsubscribe(self->pairing_bus, self->status_subscription);
  self->status_subscription = 0;
  if (self->owner_watch) { g_bus_unwatch_name(self->owner_watch); self->owner_watch = 0; }
  g_clear_pointer(&self->pairing_id, g_free); g_clear_object(&self->pairing_bus);
  if (self->clipboard_content) {
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    if (gdk_clipboard_get_content(clipboard) == self->clipboard_content) gdk_clipboard_set_content(clipboard, NULL);
    g_clear_object(&self->clipboard_content);
  }
  if (self->current_data) secure_wipe(self->current_data, strlen(self->current_data));
  g_clear_pointer(&self->current_data, g_free);
  gtk_label_set_text(GTK_LABEL(self->data_label), "");
  gtk_picture_set_paintable(GTK_PICTURE(self->qr_picture), NULL); g_clear_object(&self->qr_texture);
}
static gboolean pairing_expired(gpointer data) {
  SheetQrDisplay *self = data; self->expiry = 0; adw_dialog_close(ADW_DIALOG(self)); return G_SOURCE_REMOVE;
}
static void pairing_status_changed(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *, gpointer);
static void pairing_owner_vanished(GDBusConnection *bus, const gchar *name, gpointer data) {
  (void)bus; (void)name; SheetQrDisplay *self = data;
  pairing_closed(ADW_DIALOG(self), NULL); adw_dialog_close(ADW_DIALOG(self));
}
static void pairing_status_ready(GObject *source, GAsyncResult *result, gpointer data) {
  SheetQrDisplay *self = data; self->status_pending = FALSE;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (self->pairing_id) {
    const gchar *state = NULL; g_autoptr(GVariant) row = reply ? g_variant_get_child_value(reply, 0) : NULL;
    if (!row || !g_variant_lookup(row, "bunker_state", "&s", &state) || g_strcmp0(state, "LISTENING")) {
      pairing_closed(ADW_DIALOG(self), NULL); adw_dialog_close(ADW_DIALOG(self));
    }
  }
  if (self->pairing_id && self->status_dirty) {
    self->status_dirty = FALSE;
    pairing_status_changed(G_DBUS_CONNECTION(source), NULL, NULL, NULL, NULL, NULL, self);
  }
  g_object_unref(self);
}
static void pairing_status_changed(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *signal, GVariant *parameters, gpointer data) {
  (void)sender; (void)path; (void)interface; (void)signal; (void)parameters;
  SheetQrDisplay *self = data;
  if (!self->pairing_id) return;
  if (self->status_pending) { self->status_dirty = TRUE; return; }
  self->status_pending = TRUE;
  g_dbus_connection_call(bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "GetStatus",
      NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, pairing_status_ready, g_object_ref(self));
}
void sheet_qr_display_set_pairing(SheetQrDisplay *self, GDBusConnection *bus, const gchar *id, guint64 expires_at) {
  g_set_object(&self->pairing_bus, bus); self->pairing_id = g_strdup(id);
  self->status_subscription = g_dbus_connection_signal_subscribe(bus, "org.nostr.Signer", "org.nostr.Grotto.Control1",
      "StatusChanged", "/org/nostr/signer/grotto", NULL, G_DBUS_SIGNAL_FLAGS_NONE, pairing_status_changed, self, NULL);
  self->owner_watch = g_bus_watch_name_on_connection(bus, "org.nostr.Signer", G_BUS_NAME_WATCHER_FLAGS_NONE,
      NULL, pairing_owner_vanished, self, NULL);
  pairing_status_changed(bus, NULL, NULL, NULL, NULL, NULL, self);
  guint64 now = g_get_real_time() / G_USEC_PER_SEC;
  self->expiry = g_timeout_add_seconds(MAX(1, MIN(300, expires_at > now ? expires_at - now : 1)), pairing_expired, self);
  show_warning(self, "Single-use link. Expires in five minutes or when this sheet closes. Sharing it only requests a pairing review; it never grants signing permissions.");
}
static void sheet_qr_display_dispose(GObject *object) {
  SheetQrDisplay *self = SHEET_QR_DISPLAY(object);

  pairing_closed(ADW_DIALOG(self), NULL);

  G_OBJECT_CLASS(sheet_qr_display_parent_class)->dispose(object);
}

static void sheet_qr_display_class_init(SheetQrDisplayClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = sheet_qr_display_dispose;

  /* Load UI template */
  gtk_widget_class_set_template_from_resource(widget_class,
    APP_RESOURCE_PATH "/ui/sheets/sheet-qr-display.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, content_box);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, title_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, qr_picture);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, data_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, type_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, warning_box);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, warning_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, btn_copy);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, btn_close);
}

static void sheet_qr_display_init(SheetQrDisplay *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self, "closed", G_CALLBACK(pairing_closed), NULL);

  self->current_data = NULL;
  self->current_type = GN_QR_TYPE_UNKNOWN;
  self->qr_texture = NULL;

  /* Connect signals */
  if (self->btn_copy) {
    g_signal_connect(self->btn_copy, "clicked", G_CALLBACK(on_copy_clicked), self);
  }
  if (self->btn_close) {
    g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close_clicked), self);
  }

  /* Initially hide warning */
  hide_warning(self);
}

SheetQrDisplay *sheet_qr_display_new(void) {
  return g_object_new(TYPE_SHEET_QR_DISPLAY, NULL);
}

/* Update the QR code display */
static void update_qr_display(SheetQrDisplay *self) {
  if (!self->current_data || !*self->current_data) {
    if (self->qr_picture) {
      gtk_picture_set_paintable(GTK_PICTURE(self->qr_picture), NULL);
    }
    return;
  }

  /* Generate QR code */
  GError *error = NULL;
  g_clear_object(&self->qr_texture);

  self->qr_texture = gn_qr_generate_texture(self->current_data, QR_DISPLAY_SIZE, &error);

  if (self->qr_texture && self->qr_picture) {
    gtk_picture_set_paintable(GTK_PICTURE(self->qr_picture),
                               GDK_PAINTABLE(self->qr_texture));
  } else if (error) {
    g_warning("Failed to generate QR code: %s", error->message);
    g_error_free(error);
  }

  /* Update type label */
  if (self->type_label) {
    gtk_label_set_text(GTK_LABEL(self->type_label),
                       gn_qr_content_type_name(self->current_type));
  }

  /* Update data label (truncated for long data) */
  if (self->data_label) {
    gsize len = strlen(self->current_data);
    if (len > 60) {
      /* Show truncated version */
      g_autofree gchar *truncated = g_strdup_printf("%.30s...%.20s",
                                                     self->current_data,
                                                     self->current_data + len - 20);
      gtk_label_set_text(GTK_LABEL(self->data_label), truncated);
    } else {
      gtk_label_set_text(GTK_LABEL(self->data_label), self->current_data);
    }
  }
}

static void show_warning(SheetQrDisplay *self, const gchar *warning) {
  if (!self->warning_box || !self->warning_label) return;

  gtk_label_set_text(GTK_LABEL(self->warning_label), warning);
  gtk_widget_set_visible(self->warning_box, TRUE);
}

static void hide_warning(SheetQrDisplay *self) {
  if (self->warning_box) {
    gtk_widget_set_visible(self->warning_box, FALSE);
  }
}

void sheet_qr_display_set_data(SheetQrDisplay *self,
                                const gchar *data,
                                GnQrContentType type,
                                const gchar *title) {
  g_return_if_fail(SHEET_IS_QR_DISPLAY(self));

  if (self->current_data) secure_wipe(self->current_data, strlen(self->current_data));
  g_free(self->current_data);
  self->current_data = g_strdup(data);
  self->current_type = type;

  /* Set title */
  if (self->title_label) {
    gtk_label_set_text(GTK_LABEL(self->title_label),
                       title ? title : "QR Code");
  }

  /* Show appropriate warning for sensitive types */
  switch (type) {
    case GN_QR_TYPE_NSEC:
      show_warning(self,
        "WARNING: This QR code contains your private key. "
        "Never share it with anyone. Keep it secure.");
      break;
    case GN_QR_TYPE_NCRYPTSEC:
      show_warning(self,
        "This is an encrypted backup of your key. "
        "You'll need the password to restore it.");
      break;
    default:
      hide_warning(self);
      break;
  }

  update_qr_display(self);
}

void sheet_qr_display_set_npub(SheetQrDisplay *self,
                                const gchar *npub) {
  g_return_if_fail(SHEET_IS_QR_DISPLAY(self));
  g_return_if_fail(npub != NULL);

  /* Create nostr: URI */
  g_autofree gchar *uri = g_strdup_printf("nostr:%s", npub);

  sheet_qr_display_set_data(self, uri, GN_QR_TYPE_NPUB,
                             "Share Your Public Key");
}

void sheet_qr_display_set_ncryptsec(SheetQrDisplay *self,
                                     const gchar *ncryptsec) {
  g_return_if_fail(SHEET_IS_QR_DISPLAY(self));
  g_return_if_fail(ncryptsec != NULL);

  sheet_qr_display_set_data(self, ncryptsec, GN_QR_TYPE_NCRYPTSEC,
                             "Encrypted Key Backup");
}

void sheet_qr_display_set_bunker_uri(SheetQrDisplay *self,
                                      const gchar *bunker_uri) {
  g_return_if_fail(SHEET_IS_QR_DISPLAY(self));
  g_return_if_fail(bunker_uri != NULL);

  sheet_qr_display_set_data(self, bunker_uri, GN_QR_TYPE_BUNKER_URI,
                             "Connect Remote Signer");
}
