#include "i18n.h"
#include "page-identity-detail.h"
#include "signer-window.h"
#include "app-resources.h"
#include "dialog-actions.h"
#include "onboarding-assistant.h"
#include "widgets/gn-secure-entry.h"
#include "sheets/sheet-qr-display.h"
#include "../accounts_store.h"
#include "../secure-mem.h"
#include "../secret-variant.h"
#include "../secret-clipboard.h"
#include "session-manager.h"
#include <string.h>
#define CONTROL_PATH "/org/nostr/signer/grotto"
#define CONTROL_IFACE "org.nostr.Grotto.Control1"
typedef enum { CAPABILITIES, SEED, ENCRYPTED_FILE, ENCRYPTED_QR, RAW, ROTATE, MIGRATION } DetailOperation;
struct _GrottoIdentityDetail {
  AdwNavigationPage parent_instance;
  AdwHeaderBar *header;
  AdwActionRow *public_key, *phrase_row, *save_row, *qr_row, *raw_row, *test_row, *replace_row, *announcement_row;
  AdwPreferencesGroup *encrypted_group, *advanced_group;
  AdwBanner *error_banner, *setup_banner;
  AdwDialog *dialog, *warning;
  GtkLabel *dialog_error;
  AdwActionRow *dialog_retry;
  GtkWidget *backup_export;
  GtkBox *seed_words;
  GtkCheckButton *backup_written_down;
  GtkButton *btn_backup_done;
  GnSecureEntry *passphrase;
  gchar *identity, *phrase;
  GrottoSecretClipboardLease *clipboard_lease;
  GnSessionManager *session;
  gulong locked_handler, unlocked_handler;
  gboolean busy, ready, disposed, backup_available;
  gboolean seed_supported, export_supported, raw_supported;
  guint generation;
  DetailOperation retry_operation;
  GCancellable *cancel;
  GDBusConnection *bus;
  gchar *owner;
  guint owner_watch;
  GFile *destination;
};
G_DEFINE_TYPE(GrottoIdentityDetail, grotto_identity_detail, ADW_TYPE_NAVIGATION_PAGE)
static void wipe(gchar *text) { if (text) { gnostr_secure_clear(text, strlen(text)); g_free(text); } }
static void update(GrottoIdentityDetail *self, const gchar *error) {
  if (self->disposed) return;
  if (error) {
    adw_banner_set_title(self->error_banner, error); adw_banner_set_revealed(self->error_banner, TRUE);
    if (self->dialog_error) { gtk_label_set_text(self->dialog_error, error); gtk_widget_set_visible(GTK_WIDGET(self->dialog_retry), TRUE); }
  }
  GtkWidget *actions[] = {GTK_WIDGET(self->phrase_row), GTK_WIDGET(self->save_row), GTK_WIDGET(self->qr_row),
    GTK_WIDGET(self->raw_row), GTK_WIDGET(self->test_row), GTK_WIDGET(self->replace_row), GTK_WIDGET(self->announcement_row)};
  for (guint i = 0; i < G_N_ELEMENTS(actions); i++) gtk_widget_set_sensitive(actions[i], self->ready && !self->busy && self->identity);
  if (self->backup_export) {
    gchar *pass = self->passphrase ? gn_secure_entry_get_text(self->passphrase) : NULL;
    gtk_widget_set_sensitive(self->backup_export, pass && *pass && self->ready && !self->busy);
    gn_secure_entry_free_text(pass);
  }
  if (self->btn_backup_done) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_backup_done), self->backup_available && !self->busy && self->ready && gtk_check_button_get_active(self->backup_written_down));
}
typedef struct { GrottoIdentityDetail *self; guint generation; DetailOperation operation; } DetailCtx;
static DetailCtx *context(GrottoIdentityDetail *self, DetailOperation operation) {
  DetailCtx *ctx = g_new0(DetailCtx, 1); ctx->self = g_object_ref(self); ctx->generation = self->generation; ctx->operation = operation; return ctx;
}
static gboolean current(DetailCtx *ctx) { return !ctx->self->disposed && ctx->generation == ctx->self->generation && !gn_session_manager_is_locked(ctx->self->session); }
static void context_free(DetailCtx *ctx) { g_object_unref(ctx->self); g_free(ctx); }
static void call(GrottoIdentityDetail *self, DetailOperation operation, const gchar *method, GVariant *args);
static void choose_destination(GrottoIdentityDetail *self, DetailOperation operation);
static void action(GrottoIdentityDetail *self, DetailOperation operation);
static void clear_dialog(GrottoIdentityDetail *self) {
  g_clear_pointer(&self->clipboard_lease, grotto_secret_clipboard_lease_free);
  if (self->seed_words) gn_secure_phrase_set_text(self->seed_words, "");
  if (self->passphrase) gn_secure_entry_clear(self->passphrase);
  g_clear_pointer(&self->phrase, wipe);
  self->seed_words = NULL; self->passphrase = NULL; self->backup_written_down = NULL; self->btn_backup_done = NULL;
  self->dialog_error = NULL; self->dialog_retry = NULL; self->backup_export = NULL;
  self->backup_available = FALSE;
}
static void dialog_closed(AdwDialog *dialog, gpointer data) {
  GrottoIdentityDetail *self = data;
  if (self->dialog != dialog) return;
  self->generation++; g_cancellable_cancel(self->cancel);
  g_clear_object(&self->cancel); self->cancel = g_cancellable_new();
  clear_dialog(self); self->busy = FALSE;
  adw_dialog_set_child(dialog, NULL); g_clear_object(&self->dialog); update(self, NULL);
}
static gboolean finish_early_close(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)clock; guint *frames = data;
  /* libadwaita 1.5 opens a newly mapped sheet on its second frame. A close
   * before that is otherwise overwritten by the opening tick (adw-dialog.c,
   * map_tick_cb). Content is already detached; finish closing after that tick. */
  if (++*frames < 3) return G_SOURCE_CONTINUE;
  adw_dialog_force_close(ADW_DIALOG(widget)); return G_SOURCE_REMOVE;
}
static void retire_dialog(AdwDialog *dialog) {
  adw_dialog_set_child(dialog, NULL); /* no secret or stale action survives even an early close */
  adw_dialog_force_close(dialog);
  if (gtk_widget_get_parent(GTK_WIDGET(dialog)))
    gtk_widget_add_tick_callback(GTK_WIDGET(dialog), finish_early_close, g_new0(guint, 1), g_free);
  g_object_unref(dialog);
}
static void close_dialog(GrottoIdentityDetail *self) {
  if (self->dialog || self->warning) {
    self->generation++; g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel); self->cancel = g_cancellable_new(); self->busy = FALSE;
  }
  clear_dialog(self);
  if (self->warning) retire_dialog(g_steal_pointer(&self->warning));
  if (self->dialog) retire_dialog(g_steal_pointer(&self->dialog));
}
static void show_toast(GrottoIdentityDetail *self, const char *text) {
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  if (SIGNER_IS_WINDOW(root)) signer_window_toast(SIGNER_WINDOW(root), text);
}
typedef struct { DetailCtx *ctx; GBytes *bytes; gchar *text; gsize size; } FileJob;
static void file_done(GObject *source, GAsyncResult *result, gpointer data) {
  FileJob *job = data; g_autoptr(GError) error = NULL;
  gboolean ok = g_file_replace_contents_finish(G_FILE(source), result, NULL, &error);
  if (current(job->ctx)) {
    GrottoIdentityDetail *self = job->ctx->self; self->busy = FALSE;
    if (ok && job->ctx->operation == ENCRYPTED_FILE && self->backup_written_down) {
      self->backup_available = TRUE;
      gtk_widget_set_visible(GTK_WIDGET(self->backup_written_down), TRUE);
      gtk_widget_set_visible(GTK_WIDGET(self->btn_backup_done), TRUE);
      gtk_widget_set_visible(self->backup_export, FALSE);
      adw_dialog_set_default_widget(self->dialog, GTK_WIDGET(self->btn_backup_done));
      gtk_label_set_text(self->dialog_error, _("Backup saved. Keep it and its passphrase safe"));
      update(self, NULL);
    } else if (ok) { close_dialog(self); show_toast(self, _("Migration announcement saved. Nothing was published")); }
    else update(self, _("The file wasn't saved. Check the destination and retry"));
  }
  gnostr_secure_clear(job->text, job->size); g_bytes_unref(job->bytes); context_free(job->ctx); g_free(job);
}
static void write_destination(GrottoIdentityDetail *self, DetailOperation operation, const gchar *text) {
  FileJob *job = g_new0(FileJob, 1); job->ctx = context(self, operation);
  job->text = g_strdup(text); job->size = strlen(text); job->bytes = g_bytes_new_take(job->text, job->size);
  g_file_replace_contents_bytes_async(self->destination, job->bytes, NULL, FALSE,
      G_FILE_CREATE_PRIVATE, self->cancel, file_done, job);
}
static void migration_available(GObject *source, GAsyncResult *result, gpointer data) {
  DetailCtx *ctx = data;
  g_autoptr(GVariant) value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (current(ctx) && value) {
    const char *announcement; g_variant_get(value, "(&s)", &announcement);
    if (*announcement) gtk_widget_set_visible(GTK_WIDGET(ctx->self->announcement_row), TRUE);
  }
  context_free(ctx);
}
static void reply(GObject *source, GAsyncResult *result, gpointer data) {
  DetailCtx *ctx = data; GrottoIdentityDetail *self = ctx->self; g_autoptr(GError) error = NULL;
  GVariant *value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!current(ctx)) { if (value) g_variant_unref(value); context_free(ctx); return; }
  self->busy = FALSE;
  if (error) {
    g_autofree gchar *name = g_dbus_error_get_remote_error(error);
    if (ctx->operation == SEED && name && g_str_has_suffix(name, "NotFound")) {
      adw_action_row_set_subtitle(self->phrase_row, _("No recovery phrase is stored for this identity. Use an encrypted backup"));
      close_dialog(self);
    }
    update(self, ctx->operation == SEED ? _("Recovery phrase unavailable. Use an encrypted backup or retry") : _("The action couldn't be completed. Check the signer and retry"));
  } else if (ctx->operation == CAPABILITIES) {
    GVariant *row; g_variant_get(value, "(@a{sv})", &row);
    guint32 version = 0; g_variant_lookup(row, "control_version", "u", &version);
    g_autoptr(GVariant) caps = g_variant_lookup_value(row, "capabilities", G_VARIANT_TYPE("a{sb}"));
    self->seed_supported = self->export_supported = self->raw_supported = FALSE;
    if (caps) {
      g_variant_lookup(caps, "mnemonic-export", "b", &self->seed_supported);
      g_variant_lookup(caps, "export-identity", "b", &self->export_supported);
      g_variant_lookup(caps, "raw-nsec-export", "b", &self->raw_supported);
    }
    self->ready = version >= 1; g_variant_unref(row);
    AccountEntry *entry = accounts_store_find(accounts_store_get_default(), self->identity);
    if (entry && entry->watch_only) self->ready = FALSE;
    adw_banner_set_title(self->setup_banner, entry && !g_strcmp0(entry->setup_state, "backup-pending") ? _("Backup incomplete") : _("Identity not verified. Test this identity before relying on it"));
    adw_banner_set_revealed(self->setup_banner, entry && g_strcmp0(entry->setup_state, "ready"));
    accounts_store_entry_free(entry);
    gtk_widget_set_visible(GTK_WIDGET(self->phrase_row), self->seed_supported);
    gtk_widget_set_visible(GTK_WIDGET(self->encrypted_group), self->export_supported);
    gtk_widget_set_visible(GTK_WIDGET(self->raw_row), self->raw_supported);
    gboolean test = FALSE, rotate = FALSE;
    if (caps) { g_variant_lookup(caps, "identity-self-test", "b", &test); g_variant_lookup(caps, "rotate-identity", "b", &rotate); }
    gtk_widget_set_visible(GTK_WIDGET(self->test_row), test);
    gtk_widget_set_visible(GTK_WIDGET(self->replace_row), rotate);
    if (!self->ready) update(self, _("Identity actions are unavailable with this signer"));
    else g_dbus_connection_call(self->bus, self->owner, CONTROL_PATH, CONTROL_IFACE, "GetIdentityMigration",
      g_variant_new("(s)", self->identity), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
      10000, self->cancel, migration_available, context(self, MIGRATION));
  } else {
    const gchar *text; g_variant_get(value, "(&s)", &text);
    if (ctx->operation == SEED && self->seed_words) {
      self->phrase = g_strdup(text);
      self->backup_available = gn_secure_phrase_set_text(self->seed_words, text);
    } else if (ctx->operation == RAW) {
      g_clear_pointer(&self->clipboard_lease, grotto_secret_clipboard_lease_free);
      self->clipboard_lease = grotto_secret_clipboard_copy(gtk_widget_get_clipboard(GTK_WIDGET(self)), text);
      show_toast(self, _("Secret key copied. Clipboard managers may keep it"));
    } else if (ctx->operation == ENCRYPTED_QR) {
      close_dialog(self);
      SheetQrDisplay *qr = sheet_qr_display_new(); sheet_qr_display_set_ncryptsec(qr, text);
      self->dialog = g_object_ref(ADW_DIALOG(qr));
      g_signal_connect_object(qr, "closed", G_CALLBACK(dialog_closed), self, 0);
      adw_dialog_present(ADW_DIALOG(qr), GTK_WIDGET(self));
    } else if (ctx->operation == ENCRYPTED_FILE || ctx->operation == MIGRATION) {
      self->busy = TRUE; write_destination(self, ctx->operation, text);
    } else if (ctx->operation == ROTATE) {
      accounts_store_add(accounts_store_get_default(), text, _("Replacement identity"), NULL);
      gtk_widget_set_visible(GTK_WIDGET(self->announcement_row), TRUE);
      show_toast(self, _("Replacement created. Apps and permissions are unchanged"));
    }
  }
  if (value) g_variant_unref(value);
  update(self, NULL); context_free(ctx);
}
static void call(GrottoIdentityDetail *self, DetailOperation operation, const gchar *method, GVariant *args) {
  if (!self->bus || !self->owner || self->busy || self->disposed || gn_session_manager_is_locked(self->session)) { if (args) { g_variant_ref_sink(args); g_variant_unref(args); } return; }
  self->busy = TRUE; self->retry_operation = operation;
  adw_banner_set_revealed(self->error_banner, FALSE); update(self, NULL);
  g_dbus_connection_call(self->bus, self->owner, CONTROL_PATH, CONTROL_IFACE, method, args, NULL,
    G_DBUS_CALL_FLAGS_NO_AUTO_START, 10 * 60 * 1000, self->cancel, reply, context(self, operation));
}
static void export_call(GrottoIdentityDetail *self, DetailOperation operation) {
  if (!self->passphrase) return;
  gchar *pass = gn_secure_entry_get_text(self->passphrase);
  if (!pass || !*pass) { update(self, _("Choose a passphrase first")); gn_secure_entry_free_text(pass); return; }
  const gchar *values[] = {self->identity, "ncryptsec", pass};
  call(self, operation, "ExportIdentity", grotto_secret_tuple(values, G_N_ELEMENTS(values)));
  gn_secure_entry_free_text(pass); gn_secure_entry_clear(self->passphrase);
}
static void save_chosen(GObject *source, GAsyncResult *result, gpointer data) {
  DetailCtx *ctx = data; g_autoptr(GError) error = NULL;
  GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
  if (current(ctx)) {
    ctx->self->busy = FALSE;
    if (file) {
      g_set_object(&ctx->self->destination, file);
      if (ctx->operation == MIGRATION) call(ctx->self, MIGRATION, "GetIdentityMigration", g_variant_new("(s)", ctx->self->identity));
      else export_call(ctx->self, ENCRYPTED_FILE);
    } else {
      if (ctx->self->passphrase) gn_secure_entry_clear(ctx->self->passphrase);
      if (ctx->self->dialog_error) gtk_label_set_text(ctx->self->dialog_error, _("Export cancelled; no file was written"));
      update(ctx->self, NULL);
    }
  }
  g_clear_object(&file); context_free(ctx);
}
static void choose_destination(GrottoIdentityDetail *self, DetailOperation operation) {
  if (!self->ready || !self->identity || self->busy) return;
  if (operation == ENCRYPTED_FILE && self->backup_written_down) {
    self->backup_available = FALSE; gtk_check_button_set_active(self->backup_written_down, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->backup_written_down), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->btn_backup_done), FALSE);
  }
  self->busy = TRUE; update(self, NULL);
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_initial_name(dialog, operation == MIGRATION ? "migration.json" : "identity.ncryptsec");
  gtk_file_dialog_save(dialog, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))), self->cancel, save_chosen, context(self, operation));
  g_object_unref(dialog);
}
static void backup_done(GtkButton *button, gpointer data) {
  (void)button; GrottoIdentityDetail *self = data;
  if (!self->ready || self->busy || !self->backup_available || !gtk_check_button_get_active(self->backup_written_down)) return;
  AccountEntry *entry = accounts_store_find(accounts_store_get_default(), self->identity);
  gboolean verified = entry && (!g_strcmp0(entry->setup_state, "backup-pending") || !g_strcmp0(entry->setup_state, "ready"));
  accounts_store_entry_free(entry);
  if (verified && accounts_store_set_setup_state(accounts_store_get_default(), self->identity, "ready", NULL)) {
    close_dialog(self); show_toast(self, _("Backup acknowledged. This isn't proof of recovery"));
  } else update(self, _("Backup acknowledgement wasn't saved. Test this identity first, then retry"));
}
static void checked(GtkCheckButton *button, gpointer data) { (void)button; update(data, NULL); }
static void copy_phrase_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)dialog; GrottoIdentityDetail *self = data;
  if (!strcmp(response, "copy") && self->phrase && self->ready) {
    g_clear_pointer(&self->clipboard_lease, grotto_secret_clipboard_lease_free);
    self->clipboard_lease = grotto_secret_clipboard_copy(gtk_widget_get_clipboard(GTK_WIDGET(self)), self->phrase);
  }
}
static void copy_phrase(AdwActionRow *button, gpointer data) {
  (void)button; GrottoIdentityDetail *self = data;
  if (self->warning) { adw_dialog_force_close(self->warning); g_clear_object(&self->warning); }
  AdwDialog *dialog = adw_alert_dialog_new(_("Copy Recovery Phrase?"), _("Anyone with these words controls your identity. Clipboard managers may keep a copy"));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("_Cancel"), "copy", _("Cop_y Phrase"), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "copy", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  self->warning = g_object_ref(dialog);
  g_signal_connect_object(dialog, "response", G_CALLBACK(copy_phrase_response), self, 0);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}
static void export_commit(GtkButton *button, gpointer data) {
  GrottoIdentityDetail *self = data;
  if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "qr"))) export_call(self, ENCRYPTED_QR);
  else choose_destination(self, ENCRYPTED_FILE);
}
static void dialog_retry(AdwActionRow *button, gpointer data) {
  (void)button; GrottoIdentityDetail *self = data; DetailOperation operation = self->retry_operation;
  self->busy = FALSE; action(self, operation);
}
static void passphrase_changed(GnSecureEntry *entry, gpointer data) {
  (void)entry; update(data, NULL);
}
static void secret_dialog(GrottoIdentityDetail *self, DetailOperation operation) {
  close_dialog(self);
  AdwDialog *dialog = adw_dialog_new(); self->dialog = g_object_ref(dialog);
  adw_dialog_set_title(dialog, operation == SEED ? _("Recovery Phrase") : _("Encrypted Backup"));
  adw_dialog_set_content_width(dialog, 400);
  adw_dialog_set_follows_content_size(dialog, TRUE);
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *affirmative = gtk_button_new_with_mnemonic(operation == SEED ? _("Confirm _Backup") :
      operation == ENCRYPTED_QR ? _("Show _QR Code") : _("_Save Backup…"));
  gtk_widget_set_sensitive(affirmative, FALSE);
  GtkWidget *header = grotto_dialog_header(dialog, affirmative);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  GtkWidget *page = adw_preferences_page_new();
  adw_preferences_page_set_description(ADW_PREFERENCES_PAGE(page), operation == SEED ? _("Write these words down in order. Keep them private and offline") : _("Choose a passphrase. It's needed to restore this backup"));
  GtkWidget *group = adw_preferences_group_new(); adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
  if (operation == SEED) {
    self->seed_words = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6)); gtk_widget_set_name(GTK_WIDGET(self->seed_words), "backup-words");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->seed_words));
    self->backup_written_down = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("I wrote these down")));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->backup_written_down));
    self->btn_backup_done = GTK_BUTTON(affirmative);
    GtkWidget *copy = adw_action_row_new(); gtk_widget_set_name(copy, "copy-recovery-phrase");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(copy), _("Cop_y Recovery Phrase…"));
    adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(copy), TRUE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(copy), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), copy);
    g_signal_connect(copy, "activated", G_CALLBACK(copy_phrase), self);
    g_signal_connect(self->backup_written_down, "toggled", G_CALLBACK(checked), self);
    g_signal_connect(self->btn_backup_done, "clicked", G_CALLBACK(backup_done), self);
  } else {
    self->passphrase = GN_SECURE_ENTRY(gn_secure_entry_new()); gn_secure_entry_set_show_strength_indicator(self->passphrase, FALSE);
    gtk_widget_set_name(GTK_WIDGET(self->passphrase), "backup-passphrase");
    gn_secure_entry_set_accessible_label(self->passphrase, _("Backup Passphrase"));
    GtkWidget *passphrase_row = adw_preferences_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(passphrase_row), _("Backup Passphrase"));
    GtkWidget *passphrase_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(passphrase_box, 12); gtk_widget_set_margin_end(passphrase_box, 12);
    gtk_widget_set_margin_top(passphrase_box, 12); gtk_widget_set_margin_bottom(passphrase_box, 12);
    GtkWidget *passphrase_label = gtk_label_new(_("Backup Passphrase"));
    gtk_label_set_xalign(GTK_LABEL(passphrase_label), 0); gtk_label_set_wrap(GTK_LABEL(passphrase_label), TRUE);
    gtk_box_append(GTK_BOX(passphrase_box), passphrase_label); gtk_box_append(GTK_BOX(passphrase_box), GTK_WIDGET(self->passphrase));
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(passphrase_row), passphrase_box);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), passphrase_row);
    GtkWidget *commit = affirmative; self->backup_export = commit;
    g_signal_connect(self->passphrase, "changed", G_CALLBACK(passphrase_changed), self);
    g_signal_connect_swapped(self->passphrase, "activate", G_CALLBACK(gtk_widget_activate_default), self->passphrase);
    g_object_set_data(G_OBJECT(commit), "qr", GINT_TO_POINTER(operation == ENCRYPTED_QR));
    g_signal_connect(commit, "clicked", G_CALLBACK(export_commit), self);
    if (operation == ENCRYPTED_FILE) {
      self->backup_written_down = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("I saved this backup and its passphrase")));
      self->btn_backup_done = GTK_BUTTON(gtk_button_new_with_mnemonic(_("Confirm _Backup")));
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_backup_done), "suggested-action");
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->backup_written_down));
      adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(self->btn_backup_done));
      gtk_widget_set_visible(GTK_WIDGET(self->backup_written_down), FALSE); gtk_widget_set_visible(GTK_WIDGET(self->btn_backup_done), FALSE);
      g_signal_connect(self->backup_written_down, "toggled", G_CALLBACK(checked), self);
      g_signal_connect(self->btn_backup_done, "clicked", G_CALLBACK(backup_done), self);
    }
  }
  if (self->backup_written_down) gtk_widget_set_name(GTK_WIDGET(self->backup_written_down), "backup-acknowledgement");
  if (self->btn_backup_done) gtk_widget_set_name(GTK_WIDGET(self->btn_backup_done), "record-backup");
  self->dialog_error = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(self->dialog_error, TRUE); gtk_widget_set_name(GTK_WIDGET(self->dialog_error), "backup-feedback");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->dialog_error));
  self->dialog_retry = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->dialog_retry), _("_Retry"));
  adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(self->dialog_retry), TRUE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->dialog_retry), TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->dialog_retry), FALSE);
  g_signal_connect(self->dialog_retry, "activated", G_CALLBACK(dialog_retry), self);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->dialog_retry));
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page); adw_dialog_set_child(dialog, toolbar);
  g_signal_connect_object(dialog, "closed", G_CALLBACK(dialog_closed), self, 0);
  adw_dialog_present(dialog, GTK_WIDGET(self));
  if (self->passphrase) gn_secure_entry_grab_focus_entry(self->passphrase);
  if (operation == SEED) call(self, SEED, "ExportIdentity", g_variant_new("(sss)", self->identity, "mnemonic", ""));
}
static void confirmed(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)dialog; DetailCtx *ctx = data; GrottoIdentityDetail *self = ctx->self;
  if (!current(ctx)) return;
  self->busy = FALSE;
  if (!strcmp(response, "proceed") && self->ready)
    call(self, ctx->operation, ctx->operation == RAW ? "ExportRawIdentity" : "RotateIdentity", g_variant_new("(s)", self->identity));
  update(self, NULL);
}
static void confirmation_free(gpointer data, GClosure *closure) { (void)closure; context_free(data); }
static void confirm(GrottoIdentityDetail *self, DetailOperation operation) {
  AdwDialog *dialog = adw_alert_dialog_new(operation == RAW ? _("Copy Secret Key?") : _("Create Replacement Identity?"),
    operation == RAW ? _("Anyone with this key controls your identity. Clipboard managers may keep a copy") : _("Both identities stay on this device. Apps keep using this one until changed"));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("_Cancel"), "proceed", operation == RAW ? _("Copy _Key") : _("Create _Identity"), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "proceed", operation == RAW ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel"); adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  self->busy = TRUE; update(self, NULL);
  self->dialog = g_object_ref(dialog);
  g_signal_connect_data(dialog, "response", G_CALLBACK(confirmed), context(self, operation), confirmation_free, 0);
  /* Response starts the RPC; don't invalidate that RPC on ordinary close. */
  adw_dialog_present(dialog, GTK_WIDGET(self));
}
static void action(GrottoIdentityDetail *self, DetailOperation operation) {
  if (!self->ready || self->busy || !self->identity) return;
  self->retry_operation = operation;
  if (operation == SEED || operation == ENCRYPTED_FILE || operation == ENCRYPTED_QR) secret_dialog(self, operation);
  else if (operation == MIGRATION) choose_destination(self, operation);
  else { close_dialog(self); confirm(self, operation); }
}
static void row_activated(AdwActionRow *row, gpointer data) {
  GrottoIdentityDetail *self = data;
  if (row == self->test_row) {
    if (self->ready && !self->busy) onboarding_assistant_verify_identity(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))), self->identity);
  } else action(self, row == self->phrase_row ? SEED : row == self->save_row ? ENCRYPTED_FILE : row == self->qr_row ? ENCRYPTED_QR : row == self->raw_row ? RAW : row == self->replace_row ? ROTATE : MIGRATION);
}
static void retry(AdwBanner *banner, gpointer data) {
  (void)banner; GrottoIdentityDetail *self = data; adw_banner_set_revealed(self->error_banner, FALSE);
  if (self->retry_operation == CAPABILITIES) call(self, CAPABILITIES, "GetStatus", NULL);
  else action(self, self->retry_operation);
}
static void session_locked(GnSessionManager *session, GnLockReason reason, gpointer data) {
  (void)session; (void)reason; GrottoIdentityDetail *self = data;
  if (self->disposed) return;
  self->generation++; self->ready = self->busy = FALSE;
  g_cancellable_cancel(self->cancel); close_dialog(self);
  g_clear_object(&self->cancel); self->cancel = g_cancellable_new();
  update(self, _("Management is locked"));
}
static void session_unlocked(GnSessionManager *session, gpointer data) {
  (void)session; GrottoIdentityDetail *self = data;
  if (!self->disposed && gtk_widget_get_mapped(GTK_WIDGET(self))) call(self, CAPABILITIES, "GetStatus", NULL);
}
static void owner_appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data) {
  (void)name; GrottoIdentityDetail *self = data; g_set_object(&self->bus, bus);
  g_free(self->owner); self->owner = g_strdup(owner); call(self, CAPABILITIES, "GetStatus", NULL);
}
static void owner_vanished(GDBusConnection *bus, const char *name, gpointer data) {
  (void)bus; (void)name; GrottoIdentityDetail *self = data;
  self->generation++; g_cancellable_cancel(self->cancel); close_dialog(self);
  g_clear_object(&self->cancel); self->cancel = g_cancellable_new();
  self->busy = self->ready = FALSE; g_clear_pointer(&self->owner, g_free);
  gtk_widget_set_visible(GTK_WIDGET(self->announcement_row), FALSE);
  update(self, _("Signer unavailable. Open Grotto and retry"));
}
static void unmap(GtkWidget *widget) {
  GrottoIdentityDetail *self = GROTTO_IDENTITY_DETAIL(widget);
  self->generation++; if (self->cancel) g_cancellable_cancel(self->cancel); close_dialog(self);
  if (self->owner_watch) { g_bus_unwatch_name(self->owner_watch); self->owner_watch = 0; }
  self->busy = self->ready = FALSE; g_clear_object(&self->bus); g_clear_pointer(&self->owner, g_free);
  GTK_WIDGET_CLASS(grotto_identity_detail_parent_class)->unmap(widget);
}
static void map(GtkWidget *widget) {
  GrottoIdentityDetail *self = GROTTO_IDENTITY_DETAIL(widget);
  gboolean controls = !SIGNER_IS_WINDOW(gtk_widget_get_root(widget)) &&
      !gtk_widget_get_ancestor(widget, ADW_TYPE_DIALOG);
  adw_header_bar_set_show_start_title_buttons(self->header, controls);
  adw_header_bar_set_show_end_title_buttons(self->header, controls);
  GTK_WIDGET_CLASS(grotto_identity_detail_parent_class)->map(widget);
  if (g_cancellable_is_cancelled(self->cancel)) { g_clear_object(&self->cancel); self->cancel = g_cancellable_new(); }
  self->owner_watch = g_bus_watch_name(G_BUS_TYPE_SESSION, "org.nostr.Signer", G_BUS_NAME_WATCHER_FLAGS_AUTO_START, owner_appeared, owner_vanished, self, NULL);
}
static void dispose(GObject *object) {
  GrottoIdentityDetail *self = GROTTO_IDENTITY_DETAIL(object);
  self->disposed = TRUE; self->generation++; if (self->cancel) g_cancellable_cancel(self->cancel); close_dialog(self);
  if (self->session) {
    g_signal_handler_disconnect(self->session, self->locked_handler);
    g_signal_handler_disconnect(self->session, self->unlocked_handler);
    g_clear_object(&self->session);
  }
  if (self->owner_watch) { g_bus_unwatch_name(self->owner_watch); self->owner_watch = 0; }
  g_clear_object(&self->cancel); g_clear_object(&self->bus); g_clear_object(&self->destination);
  g_clear_pointer(&self->identity, g_free); g_clear_pointer(&self->owner, g_free);
  gtk_widget_dispose_template(GTK_WIDGET(self), GROTTO_TYPE_IDENTITY_DETAIL);
  G_OBJECT_CLASS(grotto_identity_detail_parent_class)->dispose(object);
}
static void grotto_identity_detail_class_init(GrottoIdentityDetailClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass); wc->unmap = unmap; wc->map = map;
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-identity-detail.ui");
#define BIND(name) gtk_widget_class_bind_template_child(wc, GrottoIdentityDetail, name)
  BIND(header); BIND(public_key); BIND(phrase_row); BIND(save_row); BIND(qr_row); BIND(raw_row); BIND(test_row); BIND(replace_row); BIND(announcement_row);
  BIND(encrypted_group); BIND(advanced_group); BIND(error_banner); BIND(setup_banner);
#undef BIND
}
static void grotto_identity_detail_init(GrottoIdentityDetail *self) {
  gtk_widget_init_template(GTK_WIDGET(self)); self->cancel = g_cancellable_new();
  self->session = g_object_ref(gn_session_manager_get_default());
  self->locked_handler = g_signal_connect(self->session, "session-locked", G_CALLBACK(session_locked), self);
  self->unlocked_handler = g_signal_connect(self->session, "session-unlocked", G_CALLBACK(session_unlocked), self);
  AdwActionRow *rows[] = {self->phrase_row, self->save_row, self->qr_row, self->raw_row, self->test_row, self->replace_row, self->announcement_row};
  for (guint i = 0; i < G_N_ELEMENTS(rows); i++) g_signal_connect(rows[i], "activated", G_CALLBACK(row_activated), self);
  g_signal_connect(self->error_banner, "button-clicked", G_CALLBACK(retry), self);
  update(self, NULL);
}
GtkWidget *page_identity_detail_new(const gchar *npub) {
  GrottoIdentityDetail *self = g_object_new(GROTTO_TYPE_IDENTITY_DETAIL, NULL);
  self->identity = g_strdup(npub); adw_action_row_set_subtitle(self->public_key, npub);
  return GTK_WIDGET(self);
}
static void backup_shell_closed(AdwDialog *dialog, gpointer data) { (void)data; adw_dialog_set_child(dialog, NULL); }
void page_identity_detail_present_identity(GtkWidget *parent, const gchar *npub) {
  GtkRoot *root = gtk_widget_get_root(parent);
  if (SIGNER_IS_WINDOW(root)) { signer_window_show_identity(SIGNER_WINDOW(root), npub); return; }
  /* Onboarding also offers backup before the management window exists. This
   * trusted shell contains only the pinned Backup page, not another editor. */
  AdwDialog *dialog = adw_dialog_new(); adw_dialog_set_title(dialog, _("Back Up Identity"));
  adw_dialog_set_content_width(dialog, 400); adw_dialog_set_content_height(dialog, 640);
  AdwNavigationView *navigation = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
  GrottoIdentityDetail *page = GROTTO_IDENTITY_DETAIL(page_identity_detail_new(npub));
  adw_header_bar_set_show_start_title_buttons(page->header, FALSE);
  adw_header_bar_set_show_end_title_buttons(page->header, FALSE);
  GtkWidget *close = gtk_button_new_with_mnemonic(_("_Close"));
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(adw_dialog_close), dialog);
  adw_header_bar_pack_start(page->header, close);
  adw_navigation_view_add(navigation, ADW_NAVIGATION_PAGE(page));
  adw_dialog_set_child(dialog, GTK_WIDGET(navigation));
  g_signal_connect(dialog, "closed", G_CALLBACK(backup_shell_closed), NULL);
  adw_dialog_present(dialog, parent);
}
void page_identity_detail_present(GtkWidget *parent) { page_identity_detail_present_identity(parent, NULL); }
