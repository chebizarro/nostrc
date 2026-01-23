/* sheet-key-rotation.c - Key rotation dialog implementation */
#include "sheet-key-rotation.h"
#include "../app-resources.h"
#include "../../key_rotation.h"
#include "../../secret_store.h"
#include "../../accounts_store.h"
#include <string.h>

struct _SheetKeyRotation {
  AdwDialog parent_instance;

  /* Template children - Info page */
  GtkWidget *stack_main;
  GtkLabel *lbl_current_npub;
  GtkLabel *lbl_current_label;
  AdwEntryRow *entry_new_label;
  GtkCheckButton *chk_publish;
  GtkCheckButton *chk_keep_old;
  GtkButton *btn_start_rotation;
  GtkButton *btn_cancel;

  /* Template children - Progress page */
  GtkSpinner *spinner_progress;
  GtkLabel *lbl_progress_status;
  GtkProgressBar *progress_bar;

  /* Template children - Result page */
  AdwStatusPage *status_result;
  GtkLabel *lbl_new_npub;
  GtkLabel *lbl_migration_event;
  GtkButton *btn_copy_event;
  GtkButton *btn_close;

  /* Internal state */
  gchar *npub;
  KeyRotation *rotation;

  /* Callback */
  SheetKeyRotationCompleteCb on_complete;
  gpointer on_complete_ud;
};

G_DEFINE_TYPE(SheetKeyRotation, sheet_key_rotation, ADW_TYPE_DIALOG)

/* Forward declarations */
static void on_start_rotation(GtkButton *btn, gpointer user_data);
static void on_cancel(GtkButton *btn, gpointer user_data);
static void on_copy_event(GtkButton *btn, gpointer user_data);
static void on_close(GtkButton *btn, gpointer user_data);

static void sheet_key_rotation_dispose(GObject *object) {
  SheetKeyRotation *self = SHEET_KEY_ROTATION(object);

  if (self->rotation) {
    key_rotation_cancel(self->rotation);
    key_rotation_free(self->rotation);
    self->rotation = NULL;
  }

  g_clear_pointer(&self->npub, g_free);

  G_OBJECT_CLASS(sheet_key_rotation_parent_class)->dispose(object);
}

static void sheet_key_rotation_class_init(SheetKeyRotationClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = sheet_key_rotation_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
      APP_RESOURCE_PATH "/ui/sheets/sheet-key-rotation.ui");

  /* Bind template children - Info page */
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, stack_main);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, lbl_current_npub);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, lbl_current_label);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, entry_new_label);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, chk_publish);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, chk_keep_old);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, btn_start_rotation);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, btn_cancel);

  /* Bind template children - Progress page */
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, spinner_progress);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, lbl_progress_status);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, progress_bar);

  /* Bind template children - Result page */
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, status_result);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, lbl_new_npub);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, lbl_migration_event);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, btn_copy_event);
  gtk_widget_class_bind_template_child(widget_class, SheetKeyRotation, btn_close);
}

static void sheet_key_rotation_init(SheetKeyRotation *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect signals */
  g_signal_connect(self->btn_start_rotation, "clicked",
                   G_CALLBACK(on_start_rotation), self);
  g_signal_connect(self->btn_cancel, "clicked",
                   G_CALLBACK(on_cancel), self);
  g_signal_connect(self->btn_copy_event, "clicked",
                   G_CALLBACK(on_copy_event), self);
  g_signal_connect(self->btn_close, "clicked",
                   G_CALLBACK(on_close), self);

  /* Default values */
  gtk_check_button_set_active(self->chk_publish, TRUE);
  gtk_check_button_set_active(self->chk_keep_old, TRUE);
}

SheetKeyRotation *sheet_key_rotation_new(void) {
  return g_object_new(TYPE_SHEET_KEY_ROTATION, NULL);
}

void sheet_key_rotation_set_account(SheetKeyRotation *self, const gchar *npub) {
  g_return_if_fail(SHEET_IS_KEY_ROTATION(self));

  g_free(self->npub);
  self->npub = g_strdup(npub);

  if (!npub || !*npub) {
    gtk_label_set_text(self->lbl_current_npub, "No account selected");
    gtk_label_set_text(self->lbl_current_label, "");
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_start_rotation), FALSE);
    return;
  }

  /* Display current npub (truncated) */
  gchar *display_npub = NULL;
  gsize len = strlen(npub);
  if (len > 20) {
    display_npub = g_strdup_printf("%.12s...%.8s", npub, npub + len - 8);
  } else {
    display_npub = g_strdup(npub);
  }
  gtk_label_set_text(self->lbl_current_npub, display_npub);
  g_free(display_npub);

  /* Get and display label */
  AccountsStore *as = accounts_store_get_default();
  gchar *label = accounts_store_get_display_name(as, npub);
  if (label && *label && !g_str_has_prefix(label, "npub1")) {
    gtk_label_set_text(self->lbl_current_label, label);
  } else {
    gtk_label_set_text(self->lbl_current_label, "(no label)");
  }
  g_free(label);

  /* Enable start button */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_start_rotation), TRUE);
}

void sheet_key_rotation_set_on_complete(SheetKeyRotation *self,
                                         SheetKeyRotationCompleteCb callback,
                                         gpointer user_data) {
  g_return_if_fail(SHEET_IS_KEY_ROTATION(self));

  self->on_complete = callback;
  self->on_complete_ud = user_data;
}

/* Progress callback */
static void rotation_progress_cb(KeyRotation *kr,
                                  KeyRotationState state,
                                  const gchar *message,
                                  gpointer user_data) {
  SheetKeyRotation *self = SHEET_KEY_ROTATION(user_data);
  (void)kr;

  /* Update status label */
  gtk_label_set_text(self->lbl_progress_status, message ? message : "Processing...");

  /* Update progress bar based on state */
  gdouble progress = 0.0;
  switch (state) {
    case KEY_ROTATION_STATE_GENERATING:
      progress = 0.1;
      break;
    case KEY_ROTATION_STATE_CREATING_EVENT:
      progress = 0.3;
      break;
    case KEY_ROTATION_STATE_SIGNING_OLD:
      progress = 0.5;
      break;
    case KEY_ROTATION_STATE_SIGNING_NEW:
      progress = 0.6;
      break;
    case KEY_ROTATION_STATE_STORING:
      progress = 0.8;
      break;
    case KEY_ROTATION_STATE_PUBLISHING:
      progress = 0.9;
      break;
    default:
      progress = 0.0;
      break;
  }
  gtk_progress_bar_set_fraction(self->progress_bar, progress);
}

/* Completion callback */
static void rotation_complete_cb(KeyRotation *kr,
                                  KeyRotationResult result,
                                  const gchar *new_npub,
                                  const gchar *error_message,
                                  gpointer user_data) {
  SheetKeyRotation *self = SHEET_KEY_ROTATION(user_data);

  /* Stop spinner */
  gtk_spinner_stop(self->spinner_progress);

  if (result == KEY_ROTATION_OK) {
    /* Success - show result page */
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "result");

    adw_status_page_set_icon_name(self->status_result, "emblem-ok-symbolic");
    adw_status_page_set_title(self->status_result, "Key Rotation Complete");
    adw_status_page_set_description(self->status_result,
        "Your identity has been migrated to a new key.");

    /* Display new npub */
    if (new_npub && *new_npub) {
      gchar *display = NULL;
      gsize len = strlen(new_npub);
      if (len > 20) {
        display = g_strdup_printf("%.16s...%.8s", new_npub, new_npub + len - 8);
      } else {
        display = g_strdup(new_npub);
      }
      gtk_label_set_text(self->lbl_new_npub, display);
      g_free(display);
    }

    /* Display migration event for manual publishing */
    const gchar *event = key_rotation_get_migration_event(kr);
    if (event && *event) {
      gtk_label_set_text(self->lbl_migration_event, event);
      gtk_widget_set_visible(GTK_WIDGET(self->btn_copy_event), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->btn_copy_event), FALSE);
    }

    /* Invoke completion callback */
    if (self->on_complete) {
      self->on_complete(self->npub, new_npub, self->on_complete_ud);
    }

  } else {
    /* Error - show error on result page */
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "result");

    adw_status_page_set_icon_name(self->status_result, "dialog-error-symbolic");
    adw_status_page_set_title(self->status_result, "Key Rotation Failed");

    gchar *desc = g_strdup_printf("%s\n\n%s",
        key_rotation_result_to_string(result),
        error_message ? error_message : "");
    adw_status_page_set_description(self->status_result, desc);
    g_free(desc);

    gtk_label_set_text(self->lbl_new_npub, "");
    gtk_label_set_text(self->lbl_migration_event, "");
    gtk_widget_set_visible(GTK_WIDGET(self->btn_copy_event), FALSE);
  }
}

static void on_start_rotation(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetKeyRotation *self = SHEET_KEY_ROTATION(user_data);

  if (!self->npub || !*self->npub) {
    return;
  }

  /* Create rotation context */
  self->rotation = key_rotation_new(self->npub);
  if (!self->rotation) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to initialize key rotation");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    return;
  }

  /* Configure rotation */
  const gchar *new_label = gtk_editable_get_text(GTK_EDITABLE(self->entry_new_label));
  if (new_label && *new_label) {
    key_rotation_set_new_label(self->rotation, new_label);
  }

  key_rotation_set_publish(self->rotation,
      gtk_check_button_get_active(self->chk_publish));
  key_rotation_set_keep_old(self->rotation,
      gtk_check_button_get_active(self->chk_keep_old));

  /* Set callbacks */
  key_rotation_set_progress_callback(self->rotation, rotation_progress_cb, self);
  key_rotation_set_complete_callback(self->rotation, rotation_complete_cb, self);

  /* Switch to progress page */
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "progress");
  gtk_spinner_start(self->spinner_progress);
  gtk_progress_bar_set_fraction(self->progress_bar, 0.0);
  gtk_label_set_text(self->lbl_progress_status, "Starting key rotation...");

  /* Execute rotation */
  if (!key_rotation_execute(self->rotation)) {
    gtk_spinner_stop(self->spinner_progress);
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "info");

    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to start key rotation. "
        "Make sure the source key is accessible.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);

    key_rotation_free(self->rotation);
    self->rotation = NULL;
  }
}

static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetKeyRotation *self = SHEET_KEY_ROTATION(user_data);

  if (self->rotation) {
    key_rotation_cancel(self->rotation);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static void on_copy_event(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetKeyRotation *self = SHEET_KEY_ROTATION(user_data);

  if (!self->rotation) return;

  const gchar *event = key_rotation_get_migration_event(self->rotation);
  if (!event || !*event) return;

  GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(self));
  if (display) {
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    if (clipboard) {
      gdk_clipboard_set_text(clipboard, event);

      /* Show toast */
      AdwToast *toast = adw_toast_new("Migration event copied to clipboard");
      adw_toast_set_timeout(toast, 2);

      /* Find a ToastOverlay to show the toast */
      GtkWidget *window = gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_WINDOW);
      if (window) {
        /* Just use an alert dialog as fallback since we don't have toast overlay */
        GtkAlertDialog *ad = gtk_alert_dialog_new("Migration event copied to clipboard!\n\n"
            "You can publish this event to your relays to announce your key migration.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(window));
        g_object_unref(ad);
      }
    }
  }
}

static void on_close(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetKeyRotation *self = SHEET_KEY_ROTATION(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}
