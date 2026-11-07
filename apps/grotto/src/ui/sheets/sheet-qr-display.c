/* sheet-qr-display.c - QR code display dialog implementation
 *
 * Shows a QR code with copy button and optional security warnings.
 */
#include "sheet-qr-display.h"
#include <glib/gi18n.h>
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
  GtkWidget *qr_picture;
  GtkWidget *data_label;
  GtkWidget *type_label;
  GtkWidget *data_details;
  GtkWidget *warning_box;
  GtkWidget *warning_label;
  GtkWidget *btn_copy;
  GtkWidget *btn_close;

  /* Current data */
  gchar *current_data;
  GnQrContentType current_type;
  GdkTexture *qr_texture;
  guint expiry, copy_feedback, owner_watch;
  GdkContentProvider *clipboard_content;
  GDBusConnection *connection_bus;
  GCancellable *connection_cancel;
  AdwComboRow *connection_identity;
  GtkWidget *connection_enable;
  gboolean connection_closed, connection_busy, connection_dirty, connection_user_selection;
  guint connection_subscription;
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
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_copy), "edit-copy-symbolic");
    gtk_widget_set_tooltip_text(self->btn_copy, _("Copy to Clipboard"));
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
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_copy), "object-select-symbolic");
  gtk_widget_set_tooltip_text(self->btn_copy, _("Copied"));
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

static void sheet_closed(AdwDialog *dialog, gpointer unused) {
  (void)unused; SheetQrDisplay *self = SHEET_QR_DISPLAY(dialog);
  self->connection_closed = TRUE;
  if (self->connection_cancel) g_cancellable_cancel(self->connection_cancel);
  if (self->connection_subscription && self->connection_bus)
    g_dbus_connection_signal_unsubscribe(self->connection_bus, self->connection_subscription);
  self->connection_subscription = 0;
  g_clear_object(&self->connection_bus); g_clear_object(&self->connection_cancel);
  if (!self->current_data && !self->expiry && !self->clipboard_content && !self->copy_feedback && !self->owner_watch) return;
  if (self->copy_feedback) { guint id = self->copy_feedback; self->copy_feedback = 0; g_source_remove(id); }
  if (self->expiry) { g_source_remove(self->expiry); self->expiry = 0; }
  if (self->owner_watch) { g_bus_unwatch_name(self->owner_watch); self->owner_watch = 0; }
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
static void connection_owner_vanished(GDBusConnection *bus, const gchar *name, gpointer data) {
  (void)bus; (void)name; SheetQrDisplay *self = data;
  sheet_closed(ADW_DIALOG(self), NULL); adw_dialog_close(ADW_DIALOG(self));
}
static void sheet_qr_display_dispose(GObject *object) {
  SheetQrDisplay *self = SHEET_QR_DISPLAY(object);

  sheet_closed(ADW_DIALOG(self), NULL);

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
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, qr_picture);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, data_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, type_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, data_details);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, warning_box);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, warning_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, btn_copy);
  gtk_widget_class_bind_template_child(widget_class, SheetQrDisplay, btn_close);
}

static void sheet_qr_display_init(SheetQrDisplay *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self, "closed", G_CALLBACK(sheet_closed), NULL);

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
  gboolean has_data = self->current_data && *self->current_data;
  gtk_widget_set_visible(self->qr_picture, has_data);
  gtk_widget_set_visible(self->data_details, has_data);
  if (!has_data) {
    if (self->qr_picture) {
      gtk_picture_set_paintable(GTK_PICTURE(self->qr_picture), NULL);
    }
    g_clear_object(&self->qr_texture);
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
    if (len > 60 && self->current_type != GN_QR_TYPE_BUNKER_URI) {
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

  if (g_strcmp0(self->current_data, data)) {
    if (self->copy_feedback) { g_source_remove(self->copy_feedback); self->copy_feedback = 0; }
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_copy), "edit-copy-symbolic");
    gtk_widget_set_tooltip_text(self->btn_copy, _("Copy to Clipboard"));
    gtk_widget_remove_css_class(self->btn_copy, "success");
    if (self->clipboard_content) {
      GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
      if (gdk_clipboard_get_content(clipboard) == self->clipboard_content) gdk_clipboard_set_content(clipboard, NULL);
      g_clear_object(&self->clipboard_content);
    }
  }
  if (self->current_data) secure_wipe(self->current_data, strlen(self->current_data));
  g_free(self->current_data);
  self->current_data = g_strdup(data);
  self->current_type = type;
  gtk_widget_set_sensitive(self->btn_copy, data && *data);
  gtk_label_set_text(GTK_LABEL(self->data_label), "");

  /* Set title */
  adw_dialog_set_title(ADW_DIALOG(self), title ? title : _("QR Code"));

  /* Show appropriate warning for sensitive types */
  switch (type) {
    case GN_QR_TYPE_NSEC:
      show_warning(self,
        _("Anyone with this code controls your identity. Keep it private."));
      break;
    case GN_QR_TYPE_NCRYPTSEC:
      show_warning(self,
        _("Keep this backup and its passphrase safe. You need both to restore your identity."));
      break;
    default:
      hide_warning(self);
      break;
  }

  update_qr_display(self);
}

static void connection_refresh(SheetQrDisplay *self);
static void connection_selected(GObject *object, GParamSpec *pspec, gpointer data);
static gboolean connection_expired(gpointer data) {
  SheetQrDisplay *self = data; self->expiry = 0;
  sheet_qr_display_set_data(self, NULL, GN_QR_TYPE_BUNKER_URI, _("Connection Link"));
  connection_refresh(self); return G_SOURCE_REMOVE;
}
static void connection_offer_ready(GObject *source, GAsyncResult *result, gpointer data) {
  SheetQrDisplay *self = data; self->connection_busy = FALSE;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (!self->connection_closed) {
    if (self->connection_dirty) { self->connection_dirty = FALSE; connection_refresh(self); }
    else if (reply) {
      g_autoptr(GVariant) offer = g_variant_get_child_value(reply, 0);
      const gchar *uri = NULL, *identity = NULL; guint64 expires = 0;
      if (g_variant_lookup(offer, "identity", "&s", &identity)) {
        GListModel *ids = adw_combo_row_get_model(self->connection_identity);
        for (guint i = 0; ids && i < g_list_model_get_n_items(ids); i++) {
          g_autoptr(GtkStringObject) id = g_list_model_get_item(ids, i);
          if (!g_strcmp0(identity, gtk_string_object_get_string(id))) {
            g_signal_handlers_block_by_func(self->connection_identity, connection_selected, self);
            adw_combo_row_set_selected(self->connection_identity, i);
            g_signal_handlers_unblock_by_func(self->connection_identity, connection_selected, self); break;
          }
        }
      }
      g_variant_lookup(offer, "uri", "&s", &uri); g_variant_lookup(offer, "expires_at", "t", &expires);
      sheet_qr_display_set_data(self, uri, GN_QR_TYPE_BUNKER_URI, _("Connection Link"));
      show_warning(self, _("Paste or scan this link in a remote app; single-use links are replaced after an app connects."));
      guint64 now = g_get_real_time() / G_USEC_PER_SEC;
      if (self->expiry) g_source_remove(self->expiry);
      self->expiry = g_timeout_add_seconds(MAX(1, MIN(300, expires > now ? expires - now : 1)), connection_expired, self);
    } else {
      sheet_qr_display_set_data(self, NULL, GN_QR_TYPE_BUNKER_URI, _("Connection Link"));
      show_warning(self, _("A connection link could not be created. Choose a signing identity and check Remote Signing."));
    }
  }
  g_object_unref(self);
}
static void connection_status_ready(GObject *source, GAsyncResult *result, gpointer data) {
  SheetQrDisplay *self = data;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (!self->connection_closed && !self->connection_dirty && reply) {
    g_autoptr(GVariant) status = g_variant_get_child_value(reply, 0);
    const gchar *state = NULL; gboolean enabled = FALSE;
    g_variant_lookup(status, "bunker_state", "&s", &state); g_variant_lookup(status, "bunker_enabled", "b", &enabled);
    gtk_widget_set_visible(self->connection_enable, !enabled);
    if (!g_strcmp0(state, "LISTENING")) {
      GtkStringObject *id = GTK_STRING_OBJECT(adw_combo_row_get_selected_item(self->connection_identity));
      if (id || !self->connection_user_selection) {
        g_dbus_connection_call(self->connection_bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
          "GetConnectionLink", g_variant_new("(s)", self->connection_user_selection ? gtk_string_object_get_string(id) : ""), G_VARIANT_TYPE("(a{sv})"),
          G_DBUS_CALL_FLAGS_NONE, 10000, self->connection_cancel, connection_offer_ready, self); return;
      }
      sheet_qr_display_set_data(self, NULL, GN_QR_TYPE_BUNKER_URI, _("Connection Link"));
      show_warning(self, _("Choose the identity to connect."));
    } else {
      sheet_qr_display_set_data(self, NULL, GN_QR_TYPE_BUNKER_URI, _("Connection Link"));
      show_warning(self, enabled ? _("The connection link will appear when remote signing is ready.") :
        _("Enable remote signing to produce a link that an app can paste or scan to connect."));
    }
  } else if (!self->connection_closed && !reply) {
    sheet_qr_display_set_data(self, NULL, GN_QR_TYPE_BUNKER_URI, _("Connection Link"));
    show_warning(self, _("Grotto could not reach the signer."));
  }
  self->connection_busy = FALSE;
  if (!self->connection_closed && self->connection_dirty) { self->connection_dirty = FALSE; connection_refresh(self); }
  g_object_unref(self);
}
static void connection_refresh(SheetQrDisplay *self) {
  if (self->connection_closed || !self->connection_bus) return;
  if (self->connection_busy) { self->connection_dirty = TRUE; return; }
  self->connection_busy = TRUE;
  g_dbus_connection_call(self->connection_bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "GetStatus",
    NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, self->connection_cancel, connection_status_ready, g_object_ref(self));
}
static void connection_changed(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *interface,
    const gchar *signal, GVariant *parameters, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal; (void)parameters; connection_refresh(data);
}
static void connection_selected(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)object; (void)pspec; SheetQrDisplay *self = data;
  self->connection_user_selection = TRUE;
  sheet_qr_display_set_data(self, NULL, GN_QR_TYPE_BUNKER_URI, _("Connection Link"));
  connection_refresh(self);
}
static void connection_enable(GtkButton *button, gpointer data) {
  (void)button; SheetQrDisplay *self = data;
  if (!gtk_widget_activate_action(GTK_WIDGET(self), "win.enable-remote-signing", NULL))
    show_warning(self, _("Open Remote Signing in Grotto to enable it."));
}
static void connection_identities_ready(GObject *source, GAsyncResult *result, gpointer data) {
  SheetQrDisplay *self = data;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (!self->connection_closed && reply) {
    g_autoptr(GVariant) rows = g_variant_get_child_value(reply, 0);
    g_autoptr(GtkStringList) ids = gtk_string_list_new(NULL); guint selected = GTK_INVALID_LIST_POSITION;
    for (gsize i = 0; i < g_variant_n_children(rows); i++) {
      g_autoptr(GVariant) row = g_variant_get_child_value(rows, i); const gchar *id = NULL;
      gboolean can_sign = FALSE, watch_only = TRUE, is_default = FALSE;
      g_variant_lookup(row, "identity", "&s", &id); g_variant_lookup(row, "can-sign", "b", &can_sign);
      g_variant_lookup(row, "watch-only", "b", &watch_only); g_variant_lookup(row, "is-default", "b", &is_default);
      if (!id || !can_sign || watch_only) continue;
      if (is_default) selected = g_list_model_get_n_items(G_LIST_MODEL(ids));
      gtk_string_list_append(ids, id);
    }
    guint count = g_list_model_get_n_items(G_LIST_MODEL(ids));
    self->connection_user_selection = count == 1;
    g_signal_handlers_block_by_func(self->connection_identity, connection_selected, self);
    adw_combo_row_set_model(self->connection_identity, G_LIST_MODEL(ids));
    adw_combo_row_set_selected(self->connection_identity, count == 1 ? 0 : selected);
    gtk_widget_set_visible(GTK_WIDGET(self->connection_identity), count > 1);
    g_signal_handlers_unblock_by_func(self->connection_identity, connection_selected, self);
    connection_refresh(self);
  } else if (!self->connection_closed) show_warning(self, _("Grotto could not load signing identities."));
  g_object_unref(self);
}
SheetQrDisplay *sheet_qr_display_new_connection(GrottoPanelsService *service) {
  SheetQrDisplay *self = sheet_qr_display_new();
  sheet_qr_display_set_data(self, NULL, GN_QR_TYPE_BUNKER_URI, _("Connection Link"));
  self->connection_identity = ADW_COMBO_ROW(grotto_identity_picker_new(service));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->connection_identity), _("Identity"));
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->connection_identity));
  gtk_box_prepend(GTK_BOX(self->content_box), group);
  self->connection_enable = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->connection_enable), _("Enable Remote Signing"));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->connection_enable), TRUE);
  adw_action_row_add_suffix(ADW_ACTION_ROW(self->connection_enable), gtk_image_new_from_icon_name("go-next-symbolic"));
  gtk_widget_set_visible(self->connection_enable, FALSE);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->connection_enable);
  g_signal_connect(self->connection_enable, "activated", G_CALLBACK(connection_enable), self);
  g_signal_connect(self->connection_identity, "notify::selected", G_CALLBACK(connection_selected), self);
  GApplication *app = g_application_get_default(); GDBusConnection *bus = app ? g_application_get_dbus_connection(app) : NULL;
  if (bus) {
    self->connection_bus = g_object_ref(bus); self->connection_cancel = g_cancellable_new();
    self->connection_subscription = g_dbus_connection_signal_subscribe(bus, "org.nostr.Signer", "org.nostr.Grotto.Control1", "StatusChanged",
      "/org/nostr/signer/grotto", NULL, G_DBUS_SIGNAL_FLAGS_NONE, connection_changed, self, NULL);
    self->owner_watch = g_bus_watch_name_on_connection(bus, "org.nostr.Signer", G_BUS_NAME_WATCHER_FLAGS_NONE,
      NULL, connection_owner_vanished, self, NULL);
    g_dbus_connection_call(bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ListIdentities", NULL,
      G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, self->connection_cancel, connection_identities_ready, g_object_ref(self));
  } else show_warning(self, _("Grotto could not reach the signer."));
  return self;
}

void sheet_qr_display_set_npub(SheetQrDisplay *self,
                                const gchar *npub) {
  g_return_if_fail(SHEET_IS_QR_DISPLAY(self));
  g_return_if_fail(npub != NULL);

  /* Create nostr: URI */
  g_autofree gchar *uri = g_strdup_printf("nostr:%s", npub);

  sheet_qr_display_set_data(self, uri, GN_QR_TYPE_NPUB,
                             _("Share Public Key"));
}

void sheet_qr_display_set_ncryptsec(SheetQrDisplay *self,
                                     const gchar *ncryptsec) {
  g_return_if_fail(SHEET_IS_QR_DISPLAY(self));
  g_return_if_fail(ncryptsec != NULL);

  sheet_qr_display_set_data(self, ncryptsec, GN_QR_TYPE_NCRYPTSEC,
                             _("Encrypted Backup"));
}

void sheet_qr_display_set_bunker_uri(SheetQrDisplay *self,
                                      const gchar *bunker_uri) {
  g_return_if_fail(SHEET_IS_QR_DISPLAY(self));
  g_return_if_fail(bunker_uri != NULL);

  sheet_qr_display_set_data(self, bunker_uri, GN_QR_TYPE_BUNKER_URI,
                             _("Connect an App"));
}
