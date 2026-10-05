/* sheet-qr-display.c - QR code display dialog implementation
 *
 * Shows a QR code with copy button and optional security warnings.
 */
#include "sheet-qr-display.h"
#include "../app-resources.h"
#include "../../qr-code.h"

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
};

G_DEFINE_TYPE(SheetQrDisplay, sheet_qr_display, ADW_TYPE_DIALOG)

/* Forward declarations */
static void update_qr_display(SheetQrDisplay *self);
static void show_warning(SheetQrDisplay *self, const gchar *warning);
static void hide_warning(SheetQrDisplay *self);

/* Reset copy button callback */
static gboolean reset_copy_button_cb(gpointer data) {
  SheetQrDisplay *self = SHEET_QR_DISPLAY(data);
  if (GTK_IS_BUTTON(self->btn_copy)) {
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
    if (cb) gdk_clipboard_set_text(cb, self->current_data);
  }

  /* Show brief feedback */
  gtk_button_set_label(GTK_BUTTON(self->btn_copy), "Copied!");
  gtk_widget_add_css_class(self->btn_copy, "success");

  /* Reset after delay */
  g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 2, reset_copy_button_cb,
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

static void sheet_qr_display_dispose(GObject *object) {
  SheetQrDisplay *self = SHEET_QR_DISPLAY(object);

  g_clear_pointer(&self->current_data, g_free);
  g_clear_object(&self->qr_texture);

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
