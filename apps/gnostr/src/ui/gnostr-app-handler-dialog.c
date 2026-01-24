/**
 * GnostrAppHandlerDialog - "Open with..." dialog implementation
 */

#define G_LOG_DOMAIN "gnostr-app-handler-dialog"

#include "gnostr-app-handler-dialog.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip89_handlers.h"
#include <nostr/nip19/nip19.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-app-handler-dialog.ui"

struct _GnostrAppHandlerDialog {
  AdwDialog parent_instance;

  /* Template widgets */
  GtkListBox *handler_list;
  GtkCheckButton *remember_check;
  GtkButton *open_button;
  GtkButton *cancel_button;
  GtkLabel *kind_label;
  GtkStack *content_stack;
  GtkSpinner *loading_spinner;

  /* State */
  char *event_id_hex;
  guint event_kind;
  char *event_pubkey_hex;
  char *d_tag;
  GPtrArray *handlers;
  GnostrNip89HandlerInfo *selected_handler;
};

G_DEFINE_FINAL_TYPE(GnostrAppHandlerDialog, gnostr_app_handler_dialog, ADW_TYPE_DIALOG)

enum {
  SIGNAL_HANDLER_SELECTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ============== Handler Row Widget ============== */

#define GNOSTR_TYPE_HANDLER_ROW (gnostr_handler_row_get_type())
G_DECLARE_FINAL_TYPE(GnostrHandlerRow, gnostr_handler_row, GNOSTR, HANDLER_ROW, GtkListBoxRow)

struct _GnostrHandlerRow {
  GtkListBoxRow parent_instance;
  GtkImage *icon;
  GtkLabel *name_label;
  GtkLabel *description_label;
  GtkLabel *platforms_label;
  GnostrNip89HandlerInfo *handler;
};

G_DEFINE_FINAL_TYPE(GnostrHandlerRow, gnostr_handler_row, GTK_TYPE_LIST_BOX_ROW)

static void
gnostr_handler_row_finalize(GObject *object)
{
  /* Handler info is owned by the cache, don't free */
  G_OBJECT_CLASS(gnostr_handler_row_parent_class)->finalize(object);
}

static void
gnostr_handler_row_class_init(GnostrHandlerRowClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gnostr_handler_row_finalize;
}

static void
gnostr_handler_row_init(GnostrHandlerRow *self)
{
  /* Create layout */
  GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
  gtk_widget_set_margin_top(GTK_WIDGET(hbox), 8);
  gtk_widget_set_margin_bottom(GTK_WIDGET(hbox), 8);
  gtk_widget_set_margin_start(GTK_WIDGET(hbox), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(hbox), 12);

  /* Icon */
  self->icon = GTK_IMAGE(gtk_image_new_from_icon_name("application-x-executable-symbolic"));
  gtk_image_set_pixel_size(self->icon, 48);
  gtk_widget_add_css_class(GTK_WIDGET(self->icon), "app-handler-icon");
  gtk_box_append(hbox, GTK_WIDGET(self->icon));

  /* Text container */
  GtkBox *vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
  gtk_widget_set_hexpand(GTK_WIDGET(vbox), TRUE);
  gtk_widget_set_valign(GTK_WIDGET(vbox), GTK_ALIGN_CENTER);

  /* Name */
  self->name_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->name_label, 0);
  gtk_widget_add_css_class(GTK_WIDGET(self->name_label), "title-4");
  gtk_box_append(vbox, GTK_WIDGET(self->name_label));

  /* Description */
  self->description_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->description_label, 0);
  gtk_label_set_wrap(self->description_label, TRUE);
  gtk_label_set_wrap_mode(self->description_label, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(self->description_label, 50);
  gtk_label_set_ellipsize(self->description_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_lines(self->description_label, 2);
  gtk_widget_add_css_class(GTK_WIDGET(self->description_label), "dim-label");
  gtk_box_append(vbox, GTK_WIDGET(self->description_label));

  /* Platforms */
  self->platforms_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->platforms_label, 0);
  gtk_widget_add_css_class(GTK_WIDGET(self->platforms_label), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(self->platforms_label), "accent");
  gtk_box_append(vbox, GTK_WIDGET(self->platforms_label));

  gtk_box_append(hbox, GTK_WIDGET(vbox));

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(self), GTK_WIDGET(hbox));
}

static GnostrHandlerRow *
gnostr_handler_row_new(GnostrNip89HandlerInfo *handler)
{
  GnostrHandlerRow *row = g_object_new(GNOSTR_TYPE_HANDLER_ROW, NULL);
  row->handler = handler;

  /* Set name */
  const char *name = handler->display_name ? handler->display_name :
                     handler->name ? handler->name : handler->d_tag;
  gtk_label_set_text(row->name_label, name);

  /* Set description */
  if (handler->about) {
    gtk_label_set_text(row->description_label, handler->about);
    gtk_widget_set_visible(GTK_WIDGET(row->description_label), TRUE);
  } else {
    gtk_widget_set_visible(GTK_WIDGET(row->description_label), FALSE);
  }

  /* Set platforms */
  if (handler->platforms && handler->platforms->len > 0) {
    GString *platforms = g_string_new("Available on: ");
    for (guint i = 0; i < handler->platforms->len; i++) {
      GnostrNip89PlatformHandler *ph = g_ptr_array_index(handler->platforms, i);
      if (i > 0) g_string_append(platforms, ", ");
      g_string_append(platforms, gnostr_nip89_platform_to_string(ph->platform));
    }
    gtk_label_set_text(row->platforms_label, platforms->str);
    g_string_free(platforms, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(row->platforms_label), TRUE);
  } else {
    gtk_widget_set_visible(GTK_WIDGET(row->platforms_label), FALSE);
  }

  /* Load icon - try cache first, then async download */
  if (handler->picture && *handler->picture) {
    GdkTexture *texture = gnostr_avatar_try_load_cached(handler->picture);
    if (texture) {
      gtk_image_set_from_paintable(row->icon, GDK_PAINTABLE(texture));
      g_object_unref(texture);
    } else {
      /* Download async - will update the image widget directly */
      gnostr_avatar_download_async(handler->picture, GTK_WIDGET(row->icon), NULL);
    }
  }

  return row;
}

/* ============== Dialog Implementation ============== */

static void
on_row_selected(GtkListBox *box, GtkListBoxRow *row, GnostrAppHandlerDialog *self)
{
  (void)box;

  if (!row) {
    self->selected_handler = NULL;
    gtk_widget_set_sensitive(GTK_WIDGET(self->open_button), FALSE);
    return;
  }

  if (GNOSTR_IS_HANDLER_ROW(row)) {
    GnostrHandlerRow *handler_row = GNOSTR_HANDLER_ROW(row);
    self->selected_handler = handler_row->handler;
    gtk_widget_set_sensitive(GTK_WIDGET(self->open_button), TRUE);
    g_debug("app-handler: selected %s", self->selected_handler->name);
  }
}

static char *
build_event_bech32(GnostrAppHandlerDialog *self)
{
  /* For addressable events, use naddr. Otherwise use nevent. */
  if (gnostr_nip89_is_addressable_kind(self->event_kind) && self->d_tag && self->event_pubkey_hex) {
    /* Build naddr */
    /* TODO: Use proper nip19_encode_naddr when available */
    return g_strdup_printf("naddr1...%s", self->d_tag);
  }

  /* Use nevent for regular events */
  if (self->event_id_hex && strlen(self->event_id_hex) == 64) {
    uint8_t id_bytes[32];
    for (int i = 0; i < 32; i++) {
      unsigned int byte;
      if (sscanf(self->event_id_hex + i * 2, "%2x", &byte) != 1) {
        return NULL;
      }
      id_bytes[i] = (uint8_t)byte;
    }

    char *nevent = NULL;
    /* Try to encode as nevent */
    /* For now, use a simple format - proper NIP-19 encoding would be better */
    nevent = g_strdup_printf("nevent1...%s", self->event_id_hex);
    return nevent;
  }

  return NULL;
}

static void
on_open_clicked(GtkButton *button, GnostrAppHandlerDialog *self)
{
  (void)button;

  if (!self->selected_handler) return;

  /* Build bech32 event reference */
  char *event_bech32 = build_event_bech32(self);
  if (!event_bech32) {
    g_warning("app-handler: failed to build event bech32");
    return;
  }

  /* Build handler URL */
  GnostrNip89Platform platform = gnostr_nip89_get_current_platform();
  char *url = gnostr_nip89_build_handler_url(self->selected_handler, platform, event_bech32);
  g_free(event_bech32);

  if (!url) {
    g_warning("app-handler: no URL for platform %s",
              gnostr_nip89_platform_to_string(platform));
    return;
  }

  g_debug("app-handler: opening %s", url);

  /* Remember choice if requested */
  if (gtk_check_button_get_active(self->remember_check)) {
    char *a_tag = g_strdup_printf("%d:%s:%s",
                                   GNOSTR_NIP89_KIND_HANDLER_INFO,
                                   self->selected_handler->pubkey_hex,
                                   self->selected_handler->d_tag);
    gnostr_nip89_set_preferred_handler(self->event_kind, a_tag);
    g_free(a_tag);
  }

  /* Open URL in default browser/app */
  GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
  GtkWindow *window = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL;

  GtkUriLauncher *launcher = gtk_uri_launcher_new(url);
  gtk_uri_launcher_launch(launcher, window, NULL, NULL, NULL);
  g_object_unref(launcher);

  g_free(url);

  /* Emit signal and close */
  g_signal_emit(self, signals[SIGNAL_HANDLER_SELECTED], 0, self->selected_handler);
  adw_dialog_close(ADW_DIALOG(self));
}

static void
on_cancel_clicked(GtkButton *button, GnostrAppHandlerDialog *self)
{
  (void)button;
  adw_dialog_close(ADW_DIALOG(self));
}

static void
gnostr_app_handler_dialog_dispose(GObject *object)
{
  GnostrAppHandlerDialog *self = GNOSTR_APP_HANDLER_DIALOG(object);

  g_clear_pointer(&self->event_id_hex, g_free);
  g_clear_pointer(&self->event_pubkey_hex, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  g_clear_pointer(&self->handlers, g_ptr_array_unref);

  G_OBJECT_CLASS(gnostr_app_handler_dialog_parent_class)->dispose(object);
}

static void
gnostr_app_handler_dialog_class_init(GnostrAppHandlerDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_app_handler_dialog_dispose;

  /* Signals */
  signals[SIGNAL_HANDLER_SELECTED] = g_signal_new(
    "handler-selected",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);

  /* Template */
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  gtk_widget_class_bind_template_child(widget_class, GnostrAppHandlerDialog, handler_list);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppHandlerDialog, remember_check);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppHandlerDialog, open_button);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppHandlerDialog, cancel_button);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppHandlerDialog, kind_label);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppHandlerDialog, content_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppHandlerDialog, loading_spinner);
}

static void
gnostr_app_handler_dialog_init(GnostrAppHandlerDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  self->handlers = NULL;
  self->selected_handler = NULL;

  /* Connect signals */
  g_signal_connect(self->handler_list, "row-selected",
                   G_CALLBACK(on_row_selected), self);
  g_signal_connect(self->open_button, "clicked",
                   G_CALLBACK(on_open_clicked), self);
  g_signal_connect(self->cancel_button, "clicked",
                   G_CALLBACK(on_cancel_clicked), self);

  /* Initially disable open button */
  gtk_widget_set_sensitive(GTK_WIDGET(self->open_button), FALSE);
}

GnostrAppHandlerDialog *
gnostr_app_handler_dialog_new(GtkWidget *parent)
{
  (void)parent;
  return g_object_new(GNOSTR_TYPE_APP_HANDLER_DIALOG, NULL);
}

void
gnostr_app_handler_dialog_set_event(GnostrAppHandlerDialog *self,
                                     const char *event_id_hex,
                                     guint event_kind,
                                     const char *event_pubkey_hex,
                                     const char *d_tag)
{
  g_return_if_fail(GNOSTR_IS_APP_HANDLER_DIALOG(self));

  g_free(self->event_id_hex);
  g_free(self->event_pubkey_hex);
  g_free(self->d_tag);

  self->event_id_hex = g_strdup(event_id_hex);
  self->event_kind = event_kind;
  self->event_pubkey_hex = g_strdup(event_pubkey_hex);
  self->d_tag = g_strdup(d_tag);

  /* Update kind label */
  const char *kind_desc = gnostr_nip89_get_kind_description(event_kind);
  char *label_text = g_strdup_printf("Open %s (kind %u) with:", kind_desc, event_kind);
  gtk_label_set_text(self->kind_label, label_text);
  g_free(label_text);
}

void
gnostr_app_handler_dialog_set_handlers(GnostrAppHandlerDialog *self,
                                        GPtrArray *handlers)
{
  g_return_if_fail(GNOSTR_IS_APP_HANDLER_DIALOG(self));

  g_clear_pointer(&self->handlers, g_ptr_array_unref);
  self->handlers = handlers ? g_ptr_array_ref(handlers) : NULL;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->handler_list)))) {
    gtk_list_box_remove(self->handler_list, child);
  }

  /* Show appropriate state */
  if (!handlers || handlers->len == 0) {
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
    return;
  }

  gtk_stack_set_visible_child_name(self->content_stack, "list");

  /* Add handler rows */
  for (guint i = 0; i < handlers->len; i++) {
    GnostrNip89HandlerInfo *handler = g_ptr_array_index(handlers, i);
    GnostrHandlerRow *row = gnostr_handler_row_new(handler);
    gtk_list_box_append(self->handler_list, GTK_WIDGET(row));
  }

  /* Select first row by default */
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(self->handler_list, 0);
  if (first) {
    gtk_list_box_select_row(self->handler_list, first);
  }
}

gpointer
gnostr_app_handler_dialog_get_selected_handler(GnostrAppHandlerDialog *self)
{
  g_return_val_if_fail(GNOSTR_IS_APP_HANDLER_DIALOG(self), NULL);
  return self->selected_handler;
}

gboolean
gnostr_app_handler_dialog_get_remember_choice(GnostrAppHandlerDialog *self)
{
  g_return_val_if_fail(GNOSTR_IS_APP_HANDLER_DIALOG(self), FALSE);
  return gtk_check_button_get_active(self->remember_check);
}
