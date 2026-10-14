#include "page-identity-detail.h"
#include "app-resources.h"
#include "onboarding-assistant.h"
#include "widgets/gn-secure-entry.h"
#include "sheets/sheet-qr-display.h"
#include "../accounts_store.h"
#include "../secure-mem.h"
#include "../secret-variant.h"
#include "../secret-clipboard.h"
#include <string.h>
#define CONTROL_PATH "/org/nostr/signer/grotto"
#define CONTROL_IFACE "org.nostr.Grotto.Control1"
typedef enum { INVENTORY, CAPABILITIES, SEED, ENCRYPTED_FILE, ENCRYPTED_QR, RAW, ROTATE, DEFAULT, REMOVE, MIGRATION } DetailOperation;
struct _GrottoIdentityDetail {
  AdwNavigationPage parent_instance;
  GtkDropDown *identities;
  GtkLabel *public_key, *status_label, *setup_warning;
  GtkBox *seed_words;
  GtkCheckButton *backup_written_down;
  GtkButton *btn_backup_done;
  GtkEntry *label_entry;
  GtkBox *export_passphrase_box;
  GnSecureEntry *passphrase;
  GtkButton *btn_test, *btn_label, *btn_seed, *btn_export, *btn_qr, *btn_raw, *btn_default, *btn_rotate, *btn_remove, *btn_migration;
  gchar *identity;
  gboolean busy, ready, disposed, pinned, backup_available, removal_supported;
  guint accounts_changed;
  guint generation;
  GCancellable *cancel;
  GDBusConnection *bus;
  GtkStringList *npubs;
  GFile *destination;
};
G_DEFINE_TYPE(GrottoIdentityDetail, grotto_identity_detail, ADW_TYPE_NAVIGATION_PAGE)
static void wipe(gchar *text) { if (text) { gnostr_secure_clear(text, strlen(text)); g_free(text); } }
static void setup_warning(GrottoIdentityDetail *self) {
  AccountEntry *entry = self->identity ? accounts_store_find(accounts_store_get_default(), self->identity) : NULL;
  const gchar *warning = !self->identity ? "" : entry && !g_strcmp0(entry->setup_state, "ready") ? "" :
      entry && !g_strcmp0(entry->setup_state, "backup-pending") ? "Backup incomplete. Keep a usable backup before relying on this identity. Management remains available." :
      "Identity not verified. Retry this identity's self-test before relying on it. Management remains available.";
  gtk_label_set_text(self->setup_warning, warning);
  gtk_widget_set_visible(GTK_WIDGET(self->setup_warning), *warning != '\0');
  gboolean verified = entry && !g_strcmp0(entry->setup_state, "backup-pending");
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_backup_done), self->ready && verified && self->backup_available && !self->busy && gtk_check_button_get_active(self->backup_written_down));
  accounts_store_entry_free(entry);
}
static void update(GrottoIdentityDetail *self, const gchar *message) {
  if (self->disposed) return;
  if (message) gtk_label_set_text(self->status_label, message);
  gboolean enabled = self->ready && self->identity && !self->busy;
  GtkWidget *buttons[] = {GTK_WIDGET(self->btn_test), GTK_WIDGET(self->btn_label), GTK_WIDGET(self->btn_seed), GTK_WIDGET(self->btn_export),
    GTK_WIDGET(self->btn_qr), GTK_WIDGET(self->btn_raw), GTK_WIDGET(self->btn_default), GTK_WIDGET(self->btn_rotate),
    GTK_WIDGET(self->btn_remove), GTK_WIDGET(self->btn_migration)};
  for (guint i = 0; i < G_N_ELEMENTS(buttons); i++) gtk_widget_set_sensitive(buttons[i], enabled);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_remove), enabled && self->removal_supported);
  gtk_widget_set_sensitive(GTK_WIDGET(self->identities), !self->busy);
  setup_warning(self);
}
typedef struct { GrottoIdentityDetail *self; guint generation; DetailOperation operation; } DetailCtx;
static DetailCtx *context(GrottoIdentityDetail *self, DetailOperation operation) {
  DetailCtx *ctx = g_new0(DetailCtx, 1); ctx->self = g_object_ref(self); ctx->generation = self->generation; ctx->operation = operation; return ctx;
}
static gboolean current(DetailCtx *ctx) { return !ctx->self->disposed && ctx->generation == ctx->self->generation; }
static void context_free(DetailCtx *ctx) { g_object_unref(ctx->self); g_free(ctx); }
static void call(GrottoIdentityDetail *self, DetailOperation operation, const gchar *method, GVariant *args);
static void select_identity(GrottoIdentityDetail *self, const gchar *npub) {
  self->generation++; g_free(self->identity); self->identity = g_strdup(npub);
  gtk_label_set_text(self->public_key, npub ? npub : "No signable identity");
  gn_secure_phrase_set_text(self->seed_words, ""); gn_secure_entry_clear(self->passphrase);
  self->backup_available = FALSE; gtk_check_button_set_active(self->backup_written_down, FALSE);
  accounts_store_set_selected(accounts_store_get_default(), npub);
  AccountEntry *entry = npub ? accounts_store_find(accounts_store_get_default(), npub) : NULL;
  gtk_editable_set_text(GTK_EDITABLE(self->label_entry), entry && entry->label ? entry->label : "");
  accounts_store_entry_free(entry);
  update(self, "Management selection does not switch another application's identity.");
}
static void selected(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)object; (void)pspec; GrottoIdentityDetail *self = data;
  if (self->busy || !self->npubs) return;
  guint index = gtk_drop_down_get_selected(self->identities);
  select_identity(self, gtk_string_list_get_string(self->npubs, index));
}
typedef struct { DetailCtx *ctx; GBytes *bytes; gchar *text; gsize size; } FileJob;
static void file_done(GObject *source, GAsyncResult *result, gpointer data) {
  FileJob *job = data; g_autoptr(GError) error = NULL;
  gboolean ok = g_file_replace_contents_finish(G_FILE(source), result, NULL, &error);
  if (current(job->ctx)) {
    job->ctx->self->busy = FALSE;
    if (ok && job->ctx->operation == ENCRYPTED_FILE) job->ctx->self->backup_available = TRUE;
    update(job->ctx->self, ok ? "Export saved. Keep it and its passphrase safe, then confirm your backup." : "Export was not saved. Backup is incomplete; check the destination or retry.");
  }
  gnostr_secure_clear(job->text, job->size); g_bytes_unref(job->bytes); context_free(job->ctx); g_free(job);
}
static void write_destination(GrottoIdentityDetail *self, DetailOperation operation, const gchar *text) {
  FileJob *job = g_new0(FileJob, 1); job->ctx = context(self, operation);
  job->text = g_strdup(text); job->size = strlen(text); job->bytes = g_bytes_new_take(job->text, job->size);
  g_file_replace_contents_bytes_async(self->destination, job->bytes, NULL, FALSE,
      G_FILE_CREATE_PRIVATE, self->cancel, file_done, job);
}
static void reply(GObject *source, GAsyncResult *result, gpointer data) {
  DetailCtx *ctx = data; GrottoIdentityDetail *self = ctx->self; g_autoptr(GError) error = NULL;
  GVariant *value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!current(ctx)) { if (value) g_variant_unref(value); context_free(ctx); return; }
  self->busy = FALSE;
  if (error) {
    g_autofree gchar *name = g_dbus_error_get_remote_error(error);
    update(self, ctx->operation == SEED && name && g_str_has_suffix(name, "NotFound") ?
        "No seed words for this identity. Use the encrypted export instead." :
        "Operation could not be completed. Check keyring availability and update Grotto if management capabilities are missing.");
  } else if (ctx->operation == CAPABILITIES) {
    GVariant *row; g_variant_get(value, "(@a{sv})", &row);
    guint32 version = 0; g_variant_lookup(row, "control_version", "u", &version);
    g_autoptr(GVariant) caps = g_variant_lookup_value(row, "capabilities", G_VARIANT_TYPE("a{sb}"));
    gboolean seed = FALSE; self->removal_supported = FALSE;
    if (caps) {
      g_variant_lookup(caps, "mnemonic-export", "b", &seed);
      g_variant_lookup(caps, "typed-identity-removal", "b", &self->removal_supported);
    }
    self->ready = version >= 1 && seed; g_variant_unref(row);
    update(self, self->ready ? "Choose an identity to manage." : "Update Grotto. Identity management is read-only with this daemon.");
    call(self, INVENTORY, "ListIdentities", NULL);
  } else if (ctx->operation == INVENTORY) {
    gchar **npubs; g_variant_get(value, "(^as)", &npubs);
    g_autofree gchar *wanted = g_strdup(self->identity ? self->identity : accounts_store_get_selected(accounts_store_get_default()));
    self->busy = TRUE; /* model replacement must not change a pinned identity */
    g_clear_object(&self->npubs); self->npubs = gtk_string_list_new((const gchar *const *)npubs);
    gtk_drop_down_set_model(self->identities, G_LIST_MODEL(self->npubs));
    guint index = 0;
    gboolean found = !self->pinned;
    for (guint i = 0; npubs[i]; i++) if (wanted && !strcmp(npubs[i], wanted)) { index = i; found = TRUE; break; }
    if (found && npubs[0]) {
      AccountsStore *as = accounts_store_get_default();
      if (!accounts_store_exists(as, npubs[index])) accounts_store_add(as, npubs[index], "", NULL);
      gtk_drop_down_set_selected(self->identities, index); select_identity(self, npubs[index]);
    } else { self->ready = FALSE; update(self, "This identity is watch-only or no longer held by the daemon."); }
    self->busy = FALSE;
    update(self, NULL);
    gtk_widget_set_visible(GTK_WIDGET(self->identities), !self->pinned);
    g_strfreev(npubs);
  } else if (ctx->operation == REMOVE) {
    gboolean removed; g_variant_get(value, "(b)", &removed);
    if (removed) {
      accounts_store_remove(accounts_store_get_default(), self->identity, NULL);
      select_identity(self, NULL); self->pinned = FALSE; call(self, INVENTORY, "ListIdentities", NULL);
    } else update(self, "Identity was not removed.");
  } else if (ctx->operation == DEFAULT) update(self, "Default updated for older apps only. Explicit consumer identities are unchanged.");
  else {
    const gchar *text; g_variant_get(value, "(&s)", &text);
    if (ctx->operation == SEED) {
      self->backup_available = gn_secure_phrase_set_text(self->seed_words, text); update(self, "Write these words down privately. They disappear when this page is closed or another identity is selected.");
    } else if (ctx->operation == RAW) {
      grotto_secret_clipboard_copy(gdk_display_get_clipboard(gtk_widget_get_display(GTK_WIDGET(self))), text);
      update(self, "Secret copied. Clipboard managers can retain it. Grotto clears only its own unchanged clipboard content after 60 seconds.");
    } else if (ctx->operation == ENCRYPTED_QR) {
      SheetQrDisplay *qr = sheet_qr_display_new(); sheet_qr_display_set_ncryptsec(qr, text);
      adw_dialog_present(ADW_DIALOG(qr), GTK_WIDGET(self));
    } else if (ctx->operation == ENCRYPTED_FILE || ctx->operation == MIGRATION) {
      self->busy = TRUE; write_destination(self, ctx->operation, text);
    } else if (ctx->operation == ROTATE) {
      accounts_store_add(accounts_store_get_default(), text, "Successor identity", NULL);
      update(self, "Successor identity stored. The old key, app identities and permissions are unchanged. Export the saved migration announcement here; it has not been published. Remove the old key only after confirming migration and backup.");
    }
  }
  if (value) g_variant_unref(value); update(self, NULL); context_free(ctx);
}
static void call(GrottoIdentityDetail *self, DetailOperation operation, const gchar *method, GVariant *args) {
  if (!self->bus || self->busy || self->disposed) { if (args) { g_variant_ref_sink(args); g_variant_unref(args); } return; }
  if (operation == SEED || operation == ENCRYPTED_FILE) {
    self->backup_available = FALSE; gtk_check_button_set_active(self->backup_written_down, FALSE);
    gn_secure_phrase_set_text(self->seed_words, "");
  }
  self->busy = TRUE; update(self, "Working…");
  gboolean legacy = operation == INVENTORY;
  g_dbus_connection_call(self->bus, "org.nostr.Signer", legacy ? "/org/nostr/signer" : CONTROL_PATH,
      legacy ? "org.nostr.Signer" : CONTROL_IFACE, method, args, NULL, G_DBUS_CALL_FLAGS_NONE,
      10 * 60 * 1000, self->cancel, reply, context(self, operation));
}
static void export_call(GrottoIdentityDetail *self, DetailOperation operation) {
  gchar *pass = gn_secure_entry_get_text(self->passphrase);
  if (!pass || !*pass) { update(self, "Enter an encrypted-export passphrase first."); gn_secure_entry_free_text(pass); return; }
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
    } else update(ctx->self, "Export cancelled; no file was written.");
  }
  g_clear_object(&file); context_free(ctx);
}
static void choose_destination(GrottoIdentityDetail *self, DetailOperation operation) {
  if (!self->ready || !self->identity || self->busy) return;
  self->busy = TRUE; update(self, "Choose the export destination.");
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_initial_name(dialog, operation == MIGRATION ? "migration.json" : "identity.ncryptsec");
  gtk_file_dialog_save(dialog, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))), self->cancel, save_chosen, context(self, operation));
  g_object_unref(dialog);
}
static void backup_done(GtkButton *b, gpointer data) {
  (void)b; GrottoIdentityDetail *self = data;
  AccountEntry *entry = self->identity ? accounts_store_find(accounts_store_get_default(), self->identity) : NULL;
  gboolean verified = entry && !g_strcmp0(entry->setup_state, "backup-pending");
  accounts_store_entry_free(entry);
  if (!self->ready || self->busy || !verified || !self->backup_available || !gtk_check_button_get_active(self->backup_written_down)) return;
  update(self, accounts_store_set_setup_state(accounts_store_get_default(), self->identity, "ready", NULL) ?
      "Verified identity and backup acknowledged." : "Backup acknowledgement could not be saved; setup remains incomplete.");
}
static void backup_checked(GtkCheckButton *b, gpointer data) { (void)b; update(data, NULL); }
static void metadata_changed(AccountsStore *as, AccountsChangeType change, const gchar *id, gpointer data) {
  (void)as; (void)change; (void)id; GrottoIdentityDetail *self = data;
  if (!self->disposed) update(self, NULL);
}
static void test_clicked(GtkButton *b, gpointer data) {
  (void)b; GrottoIdentityDetail *s = data;
  if (s->ready && !s->busy && s->identity)
    onboarding_assistant_verify_identity(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(s))), s->identity);
}
static void seed_clicked(GtkButton *b, gpointer data) { (void)b; GrottoIdentityDetail *s = data; call(s, SEED, "ExportIdentity", g_variant_new("(sss)", s->identity, "mnemonic", "")); }
static void export_clicked(GtkButton *b, gpointer data) { (void)b; choose_destination(data, ENCRYPTED_FILE); }
static void migration_clicked(GtkButton *b, gpointer data) { (void)b; choose_destination(data, MIGRATION); }
static void qr_clicked(GtkButton *b, gpointer data) { (void)b; export_call(data, ENCRYPTED_QR); }
static void default_clicked(GtkButton *b, gpointer data) { (void)b; GrottoIdentityDetail *s = data; call(s, DEFAULT, "SetDefaultIdentityForLegacy", g_variant_new("(s)", s->identity)); }
static void label_clicked(GtkButton *b, gpointer data) {
  (void)b; GrottoIdentityDetail *s = data;
  update(s, accounts_store_set_label(accounts_store_get_default(), s->identity, gtk_editable_get_text(GTK_EDITABLE(s->label_entry)), NULL) ? "Label saved." : "Label could not be saved.");
}
static void confirmed(AdwAlertDialog *dialog, const gchar *response, gpointer data) {
  DetailCtx *ctx = data; GrottoIdentityDetail *s = ctx->self;
  if (!current(ctx)) return;
  s->busy = FALSE;
  if (!strcmp(response, "proceed") && s->ready) {
    if (ctx->operation == REMOVE) {
      GtkWidget *entry = adw_alert_dialog_get_extra_child(dialog);
      call(s, REMOVE, "RemoveIdentity", g_variant_new("(ss)", s->identity, gtk_editable_get_text(GTK_EDITABLE(entry))));
    } else call(s, ctx->operation, ctx->operation == RAW ? "ExportRawIdentity" : "RotateIdentity", g_variant_new("(s)", s->identity));
  } else update(s, "Cancelled; identity preserved.");
}
static void confirmation_free(gpointer data, GClosure *closure) { (void)closure; context_free(data); }
static void typed_changed(GtkEditable *entry, gpointer data) {
  AdwAlertDialog *dialog = data;
  const gchar *identity = g_object_get_data(G_OBJECT(dialog), "confirmation-identity");
  adw_alert_dialog_set_response_enabled(dialog, "proceed", !g_strcmp0(gtk_editable_get_text(entry), identity));
}
static void confirm(GrottoIdentityDetail *self, const gchar *title, const gchar *body, DetailOperation operation) {
  if (!self->ready || self->busy || !self->identity || (operation == REMOVE && !self->removal_supported)) return;
  g_autofree gchar *message = g_strdup_printf("%s\n\nIdentity: %s", body, self->identity);
  AdwDialog *dialog = adw_alert_dialog_new(title, message);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "proceed", "Continue", NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "proceed", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  if (operation == REMOVE) {
    GtkWidget *entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Type the full npub to confirm removal");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
    g_object_set_data_full(G_OBJECT(dialog), "confirmation-identity", g_strdup(self->identity), g_free);
    adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), "proceed", FALSE);
    g_signal_connect(entry, "changed", G_CALLBACK(typed_changed), dialog);
  }
  self->busy = TRUE; update(self, NULL); /* pin the displayed identity until decision */
  g_signal_connect_data(dialog, "response", G_CALLBACK(confirmed), context(self, operation), confirmation_free, 0);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}
static void raw_clicked(GtkButton *b, gpointer data) { (void)b; confirm(data, "Reveal secret key?", "Anyone with this key can control your identity. Clipboard managers may retain it. Never paste it into software you do not trust.", RAW); }
static void rotate_clicked(GtkButton *b, gpointer data) { (void)b; confirm(data, "Create a successor identity?", "Both identities remain stored. Applications will NOT be redirected and permissions will NOT be copied. A signed migration announcement is saved, not published.", ROTATE); }
static void remove_clicked(GtkButton *b, gpointer data) { (void)b; confirm(data, "Remove this identity?", "Confirm you have a usable backup and no application still needs this key. This cannot be undone by Grotto.", REMOVE); }
static void bus_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; GrottoIdentityDetail *self = data; g_autoptr(GError) error = NULL;
  GDBusConnection *bus = g_bus_get_finish(result, &error);
  if (!self->disposed && bus) { g_set_object(&self->bus, bus); call(self, CAPABILITIES, "GetStatus", NULL); }
  else if (!self->disposed) update(self, "Signer unavailable. Identity management is read-only.");
  g_clear_object(&bus); g_object_unref(self);
}
static void unmap(GtkWidget *widget) {
  GrottoIdentityDetail *self = GROTTO_IDENTITY_DETAIL(widget);
  if (!self->disposed) {
    self->generation++; if (self->cancel) g_cancellable_cancel(self->cancel);
    self->busy = FALSE; gn_secure_phrase_set_text(self->seed_words, ""); gn_secure_entry_clear(self->passphrase);
  }
  GTK_WIDGET_CLASS(grotto_identity_detail_parent_class)->unmap(widget);
}
static void map(GtkWidget *widget) {
  GrottoIdentityDetail *self = GROTTO_IDENTITY_DETAIL(widget);
  GTK_WIDGET_CLASS(grotto_identity_detail_parent_class)->map(widget);
  if (g_cancellable_is_cancelled(self->cancel)) { g_clear_object(&self->cancel); self->cancel = g_cancellable_new(); }
  if (self->bus) call(self, CAPABILITIES, "GetStatus", NULL);
}
static void dispose(GObject *object) {
  GrottoIdentityDetail *self = GROTTO_IDENTITY_DETAIL(object);
  if (self->accounts_changed) { accounts_store_disconnect_changed(accounts_store_get_default(), self->accounts_changed); self->accounts_changed = 0; }
  if (!self->disposed) { gn_secure_phrase_set_text(self->seed_words, ""); gn_secure_entry_clear(self->passphrase); }
  self->disposed = TRUE; self->generation++; if (self->cancel) g_cancellable_cancel(self->cancel);
  g_clear_object(&self->cancel); g_clear_object(&self->bus); g_clear_object(&self->npubs); g_clear_object(&self->destination);
  g_clear_pointer(&self->identity, g_free);
  gtk_widget_dispose_template(GTK_WIDGET(self), GROTTO_TYPE_IDENTITY_DETAIL);
  G_OBJECT_CLASS(grotto_identity_detail_parent_class)->dispose(object);
}
static void grotto_identity_detail_class_init(GrottoIdentityDetailClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass); wc->unmap = unmap; wc->map = map;
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-identity-detail.ui");
#define BIND(name) gtk_widget_class_bind_template_child(wc, GrottoIdentityDetail, name)
  BIND(setup_warning); BIND(backup_written_down); BIND(btn_backup_done);
  BIND(identities); BIND(public_key); BIND(seed_words); BIND(status_label); BIND(label_entry); BIND(export_passphrase_box);
  BIND(btn_test); BIND(btn_label); BIND(btn_seed); BIND(btn_export); BIND(btn_qr); BIND(btn_raw); BIND(btn_default); BIND(btn_rotate); BIND(btn_remove); BIND(btn_migration);
#undef BIND
}
static void grotto_identity_detail_init(GrottoIdentityDetail *self) {
  gtk_widget_init_template(GTK_WIDGET(self)); self->cancel = g_cancellable_new();
  self->passphrase = GN_SECURE_ENTRY(gn_secure_entry_new()); gn_secure_entry_set_show_strength_indicator(self->passphrase, FALSE);
  gtk_box_append(self->export_passphrase_box, GTK_WIDGET(self->passphrase));
#define CLICK(name, cb) g_signal_connect(self->name, "clicked", G_CALLBACK(cb), self)
  CLICK(btn_backup_done, backup_done);
  CLICK(btn_test, test_clicked); CLICK(btn_seed, seed_clicked); CLICK(btn_export, export_clicked); CLICK(btn_qr, qr_clicked); CLICK(btn_raw, raw_clicked);
  CLICK(btn_rotate, rotate_clicked); CLICK(btn_default, default_clicked); CLICK(btn_remove, remove_clicked);
  CLICK(btn_label, label_clicked); CLICK(btn_migration, migration_clicked);
#undef CLICK
  g_signal_connect(self->identities, "notify::selected", G_CALLBACK(selected), self);
  g_signal_connect(self->backup_written_down, "toggled", G_CALLBACK(backup_checked), self);
  self->accounts_changed = accounts_store_connect_changed(accounts_store_get_default(), metadata_changed, self);
  update(self, "Checking daemon capabilities…");
  g_bus_get(G_BUS_TYPE_SESSION, self->cancel, bus_ready, g_object_ref(self));
}
GtkWidget *page_identity_detail_new(const gchar *npub) {
  GrottoIdentityDetail *self = g_object_new(GROTTO_TYPE_IDENTITY_DETAIL, NULL);
  self->identity = g_strdup(npub); self->pinned = npub != NULL; return GTK_WIDGET(self);
}
void page_identity_detail_present_identity(GtkWidget *parent, const gchar *npub) {
  AdwDialog *dialog = adw_dialog_new(); adw_dialog_set_title(dialog, "Identity and backup");
  adw_dialog_set_content_width(dialog, 560); adw_dialog_set_content_height(dialog, 640);
  GtkWidget *navigation = adw_navigation_view_new();
  adw_navigation_view_add(ADW_NAVIGATION_VIEW(navigation), ADW_NAVIGATION_PAGE(page_identity_detail_new(npub)));
  adw_dialog_set_child(dialog, navigation); adw_dialog_present(dialog, parent);
}
void page_identity_detail_present(GtkWidget *parent) { page_identity_detail_present_identity(parent, NULL); }
