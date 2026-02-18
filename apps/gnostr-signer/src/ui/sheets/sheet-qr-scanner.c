/* sheet-qr-scanner.c - QR code scanner dialog implementation
 *
 * Provides multiple ways to scan QR codes:
 * 1. Camera scanning (real-time)
 * 2. Paste from clipboard
 * 3. Load from file
 */
#include "sheet-qr-scanner.h"
#include "../app-resources.h"
#include "../../qr-code.h"
#include "../../qr-scanner.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

struct _SheetQrScanner {
  AdwDialog parent_instance;

  /* UI components */
  GtkWidget *stack;
  GtkWidget *scanner_page;
  GtkWidget *result_page;
  GtkWidget *scanner_widget;
  GtkWidget *status_label;
  GtkWidget *hint_label;
  GtkWidget *btn_camera;
  GtkWidget *btn_paste;
  GtkWidget *btn_file;
  GtkWidget *btn_cancel;
  GtkWidget *btn_import;

  /* Result display */
  GtkWidget *result_type_label;
  GtkWidget *result_data_label;
  GtkWidget *result_icon;

  /* Scanning state */
  gchar *scanned_data;
  GnQrContentType scanned_type;

  /* Accepted types */
  GnQrContentType *accepted_types;
  gsize n_accepted_types;

  /* Success callback */
  SheetQrScannerSuccessCb on_success;
  gpointer on_success_ud;
};

G_DEFINE_TYPE(SheetQrScanner, sheet_qr_scanner, ADW_TYPE_DIALOG)

/* Forward declarations */
static void show_result(SheetQrScanner *self, const gchar *data, GnQrContentType type);
static void show_scanner(SheetQrScanner *self);
static gboolean is_type_accepted(SheetQrScanner *self, GnQrContentType type);

/* Default accepted types for key import */
static const GnQrContentType default_accepted_types[] = {
  GN_QR_TYPE_NSEC,
  GN_QR_TYPE_NCRYPTSEC,
  GN_QR_TYPE_HEX_KEY,
  GN_QR_TYPE_NOSTR_URI,  /* If it contains nsec */
  GN_QR_TYPE_UNKNOWN     /* Terminator */
};

/* Check if a type is accepted */
static gboolean is_type_accepted(SheetQrScanner *self, GnQrContentType type) {
  const GnQrContentType *types = self->accepted_types;
  gsize n = self->n_accepted_types;

  if (!types || n == 0) {
    types = default_accepted_types;
    n = G_N_ELEMENTS(default_accepted_types) - 1;
  }

  for (gsize i = 0; i < n; i++) {
    if (types[i] == type) return TRUE;
  }

  return FALSE;
}

/* Handle scan result */
static void handle_scan_result(SheetQrScanner *self, GnQrScanResult *result) {
  if (!result || !result->data) return;

  /* Check if type is accepted */
  if (!is_type_accepted(self, result->type)) {
    /* For nostr: URI, check if it contains nsec */
    if (result->type == GN_QR_TYPE_NOSTR_URI) {
      if (!strstr(result->data, "nsec1")) {
        gtk_label_set_text(GTK_LABEL(self->status_label),
                           "QR code found but not a valid key");
        return;
      }
    } else {
      gtk_label_set_text(GTK_LABEL(self->status_label),
                         "QR code type not supported for import");
      return;
    }
  }

  show_result(self, result->data, result->type);
}

/* Clipboard scan callback */
static void clipboard_scan_done(GnQrScanResult *result,
                                 GError *error,
                                 gpointer user_data) {
  SheetQrScanner *self = SHEET_QR_SCANNER(user_data);

  if (error) {
    gtk_label_set_text(GTK_LABEL(self->status_label), error->message);
    return;
  }

  if (!result) {
    gtk_label_set_text(GTK_LABEL(self->status_label),
                       "No QR code found in clipboard image");
    return;
  }

  handle_scan_result(self, result);
}

/* Camera QR detected callback */
static void on_qr_detected(GnQrScanner *scanner,
                            GnQrScanResult *result,
                            gpointer user_data) {
  (void)scanner;
  SheetQrScanner *self = SHEET_QR_SCANNER(user_data);
  handle_scan_result(self, result);
}

/* Button handlers */
static void on_camera_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetQrScanner *self = SHEET_QR_SCANNER(user_data);

  if (!gn_qr_camera_available()) {
    gtk_label_set_text(GTK_LABEL(self->status_label),
                       "Camera is not available on this system");
    return;
  }

  /* Toggle camera */
  if (self->scanner_widget) {
    GnQrScanner *scanner = GN_QR_SCANNER(self->scanner_widget);
    if (gn_qr_scanner_is_active(scanner)) {
      gn_qr_scanner_stop(scanner);
      gtk_button_set_label(GTK_BUTTON(self->btn_camera), "Start Camera");
    } else {
      GError *error = NULL;
      if (gn_qr_scanner_start(scanner, &error)) {
        gtk_button_set_label(GTK_BUTTON(self->btn_camera), "Stop Camera");
        gtk_label_set_text(GTK_LABEL(self->status_label),
                           "Point camera at QR code...");
      } else {
        gtk_label_set_text(GTK_LABEL(self->status_label),
                           error ? error->message : "Failed to start camera");
        if (error) g_error_free(error);
      }
    }
  }
}

static void on_paste_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetQrScanner *self = SHEET_QR_SCANNER(user_data);

  gtk_label_set_text(GTK_LABEL(self->status_label), "Scanning clipboard...");

  GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(self));
  GdkClipboard *clipboard = gdk_display_get_clipboard(display);

  gn_qr_scan_clipboard_async(clipboard, clipboard_scan_done, self);
}

static void on_file_response(GObject *source, GAsyncResult *result, gpointer user_data) {
  SheetQrScanner *self = SHEET_QR_SCANNER(user_data);
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source);

  GError *error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_open_finish(dialog, result, &error);

  if (error) {
    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
      gtk_label_set_text(GTK_LABEL(self->status_label), error->message);
    }
    g_error_free(error);
    return;
  }

  if (!file) return;

  /* Load the image */
  gchar *path = g_file_get_path(file);

  if (!path) {
    gtk_label_set_text(GTK_LABEL(self->status_label), "Could not get file path");
    return;
  }

  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &error);
  g_free(path);

  if (!pixbuf) {
    gtk_label_set_text(GTK_LABEL(self->status_label),
                       error ? error->message : "Failed to load image");
    if (error) g_error_free(error);
    return;
  }

  /* Scan the image */
  GnQrScanResult *scan_result = gn_qr_scan_pixbuf(pixbuf, &error);
  g_object_unref(pixbuf);

  if (!scan_result) {
    gtk_label_set_text(GTK_LABEL(self->status_label),
                       error ? error->message : "No QR code found in image");
    if (error) g_error_free(error);
    return;
  }

  handle_scan_result(self, scan_result);
  gn_qr_scan_result_free(scan_result);
}

static void on_file_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetQrScanner *self = SHEET_QR_SCANNER(user_data);

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Open QR Code Image");

  /* Set up file filters */
  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

  GtkFileFilter *image_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(image_filter, "Images");
  gtk_file_filter_add_mime_type(image_filter, "image/*");
  g_list_store_append(filters, image_filter);
  g_object_unref(image_filter);

  GtkFileFilter *all_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(all_filter, "All Files");
  gtk_file_filter_add_pattern(all_filter, "*");
  g_list_store_append(filters, all_filter);
  g_object_unref(all_filter);

  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  g_object_unref(filters);

  gtk_file_dialog_open(dialog,
                       GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))),
                       NULL,
                       on_file_response,
                       self);
  g_object_unref(dialog);
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetQrScanner *self = SHEET_QR_SCANNER(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static void on_import_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetQrScanner *self = SHEET_QR_SCANNER(user_data);

  if (!self->scanned_data) return;

  /* Call success callback */
  if (self->on_success) {
    self->on_success(self->scanned_data, self->scanned_type, self->on_success_ud);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetQrScanner *self = SHEET_QR_SCANNER(user_data);
  show_scanner(self);
}

/* Show result page */
static void show_result(SheetQrScanner *self, const gchar *data, GnQrContentType type) {
  /* Stop camera if running */
  if (self->scanner_widget && GN_IS_QR_SCANNER(self->scanner_widget)) {
    gn_qr_scanner_stop(GN_QR_SCANNER(self->scanner_widget));
  }

  /* Store result */
  g_free(self->scanned_data);
  self->scanned_data = g_strdup(data);
  self->scanned_type = type;

  /* Update result display */
  if (self->result_type_label) {
    gtk_label_set_text(GTK_LABEL(self->result_type_label),
                       gn_qr_content_type_name(type));
  }

  if (self->result_data_label) {
    gsize len = strlen(data);
    if (len > 50) {
      g_autofree gchar *truncated = g_strdup_printf("%.25s...%.15s", data, data + len - 15);
      gtk_label_set_text(GTK_LABEL(self->result_data_label), truncated);
    } else {
      gtk_label_set_text(GTK_LABEL(self->result_data_label), data);
    }
  }

  /* Set appropriate icon */
  if (self->result_icon) {
    const gchar *icon_name = "dialog-information-symbolic";
    switch (type) {
      case GN_QR_TYPE_NSEC:
        icon_name = "dialog-password-symbolic";
        break;
      case GN_QR_TYPE_NCRYPTSEC:
        icon_name = "security-high-symbolic";
        break;
      case GN_QR_TYPE_NPUB:
        icon_name = "avatar-default-symbolic";
        break;
      default:
        break;
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(self->result_icon), icon_name);
  }

  /* Show result page */
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "result");
}

/* Show scanner page */
static void show_scanner(SheetQrScanner *self) {
  g_free(self->scanned_data);
  self->scanned_data = NULL;
  self->scanned_type = GN_QR_TYPE_UNKNOWN;

  gtk_label_set_text(GTK_LABEL(self->status_label), "Ready to scan");
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "scanner");
}

static void sheet_qr_scanner_dispose(GObject *object) {
  SheetQrScanner *self = SHEET_QR_SCANNER(object);

  /* Stop camera */
  if (self->scanner_widget && GN_IS_QR_SCANNER(self->scanner_widget)) {
    gn_qr_scanner_stop(GN_QR_SCANNER(self->scanner_widget));
  }

  g_clear_pointer(&self->scanned_data, g_free);
  g_clear_pointer(&self->accepted_types, g_free);

  G_OBJECT_CLASS(sheet_qr_scanner_parent_class)->dispose(object);
}

static void sheet_qr_scanner_class_init(SheetQrScannerClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = sheet_qr_scanner_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
    APP_RESOURCE_PATH "/ui/sheets/sheet-qr-scanner.ui");

  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, stack);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, scanner_page);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, result_page);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, status_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, hint_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, btn_camera);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, btn_paste);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, btn_file);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, btn_cancel);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, btn_import);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, result_type_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, result_data_label);
  gtk_widget_class_bind_template_child(widget_class, SheetQrScanner, result_icon);
}

static void sheet_qr_scanner_init(SheetQrScanner *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->scanned_data = NULL;
  self->scanned_type = GN_QR_TYPE_UNKNOWN;
  self->accepted_types = NULL;
  self->n_accepted_types = 0;
  self->on_success = NULL;
  self->on_success_ud = NULL;

  /* Create scanner widget and add to scanner page */
  self->scanner_widget = GTK_WIDGET(gn_qr_scanner_new());
  if (self->scanner_page) {
    gtk_box_prepend(GTK_BOX(self->scanner_page), self->scanner_widget);
    gtk_widget_set_vexpand(self->scanner_widget, TRUE);

    /* Connect QR detected signal */
    g_signal_connect(self->scanner_widget, "qr-detected",
                     G_CALLBACK(on_qr_detected), self);
  }

  /* Connect button signals */
  if (self->btn_camera) {
    g_signal_connect(self->btn_camera, "clicked", G_CALLBACK(on_camera_clicked), self);
    /* Disable if camera not available */
    if (!gn_qr_camera_available()) {
      gtk_widget_set_sensitive(self->btn_camera, FALSE);
      gtk_button_set_label(GTK_BUTTON(self->btn_camera), "Camera Unavailable");
    }
  }
  if (self->btn_paste) {
    g_signal_connect(self->btn_paste, "clicked", G_CALLBACK(on_paste_clicked), self);
  }
  if (self->btn_file) {
    g_signal_connect(self->btn_file, "clicked", G_CALLBACK(on_file_clicked), self);
  }
  if (self->btn_cancel) {
    g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);
  }
  if (self->btn_import) {
    g_signal_connect(self->btn_import, "clicked", G_CALLBACK(on_import_clicked), self);
  }

  /* Find and connect back button in result page */
  GtkWidget *btn_back = gtk_widget_get_first_child(self->result_page);
  while (btn_back) {
    if (GTK_IS_BUTTON(btn_back)) {
      const gchar *label = gtk_button_get_label(GTK_BUTTON(btn_back));
      if (label && strstr(label, "Back")) {
        g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_clicked), self);
        break;
      }
    }
    btn_back = gtk_widget_get_next_sibling(btn_back);
  }
}

SheetQrScanner *sheet_qr_scanner_new(void) {
  return g_object_new(TYPE_SHEET_QR_SCANNER, NULL);
}

void sheet_qr_scanner_set_on_success(SheetQrScanner *self,
                                      SheetQrScannerSuccessCb callback,
                                      gpointer user_data) {
  g_return_if_fail(SHEET_IS_QR_SCANNER(self));
  self->on_success = callback;
  self->on_success_ud = user_data;
}

void sheet_qr_scanner_set_accepted_types(SheetQrScanner *self,
                                          const GnQrContentType *types) {
  g_return_if_fail(SHEET_IS_QR_SCANNER(self));

  g_free(self->accepted_types);
  self->accepted_types = NULL;
  self->n_accepted_types = 0;

  if (!types) return;

  /* Count types */
  gsize count = 0;
  while (types[count] != GN_QR_TYPE_UNKNOWN) {
    count++;
  }

  if (count > 0) {
    self->accepted_types = g_new(GnQrContentType, count);
    memcpy(self->accepted_types, types, count * sizeof(GnQrContentType));
    self->n_accepted_types = count;
  }

  /* Update scanner widget if exists */
  if (self->scanner_widget && GN_IS_QR_SCANNER(self->scanner_widget)) {
    gn_qr_scanner_set_accepted_types(GN_QR_SCANNER(self->scanner_widget), types);
  }
}

const gchar *sheet_qr_scanner_get_scanned_data(SheetQrScanner *self) {
  g_return_val_if_fail(SHEET_IS_QR_SCANNER(self), NULL);
  return self->scanned_data;
}

GnQrContentType sheet_qr_scanner_get_scanned_type(SheetQrScanner *self) {
  g_return_val_if_fail(SHEET_IS_QR_SCANNER(self), GN_QR_TYPE_UNKNOWN);
  return self->scanned_type;
}
