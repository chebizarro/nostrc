/* Explicit identity-management UI. Client applications never receive keys;
 * this trusted UI handles recovery material transiently. */
#include "onboarding-assistant.h"
#include "page-identity-detail.h"
#include "../secret-variant.h"
#include "app-resources.h"
#include "../accounts_store.h"
#include "../backup-recovery.h"
#include "../secret_store.h"
#include "../secure-mem.h"
#include "../secret-clipboard.h"
#include "widgets/gn-secure-entry.h"
#include <gio/gio.h>
#include <string.h>
#define CONTROL_PATH "/org/nostr/signer/grotto"
#define CONTROL_IFACE "org.nostr.Grotto.Control1"
#define KEY_TIMEOUT (10 * 60 * 1000)

struct _OnboardingAssistant {
  AdwWindow parent_instance;
  GtkStack *steps;
  GtkButton *btn_back, *btn_next, *btn_skip, *btn_retry, *btn_copy_seed, *btn_backup_retry, *btn_backup_export;
  GtkCheckButton *radio_create, *radio_import, *radio_connect, *radio_import_seed, *seed_written_down;
  GtkEntry *entry_profile_name, *entry_import_label;
  GtkEntry *text_import_data;
  GtkBox *seed_words;
  GtkBox *box_import_passphrase_container;
  GnSecureEntry *import_passphrase;
  GtkLabel *import_passphrase_label, *lbl_status, *ready_summary;
  GtkSpinner *spinner_status;
  OnboardingState state;
  OnboardingTestResult test_result;
  GDBusConnection *test_bus;
  guint testing_signal;
  gboolean busy, closed, finished, identity_stored, self_test_passed, capabilities_ready;
  guint generation, accounts_changed;
  gboolean backup_presented;
  GCancellable *cancel;
  gchar *identity, *mnemonic, *nsec;
  OnboardingAssistantFinishedCb on_finished;
  gpointer on_finished_data;
};
G_DEFINE_TYPE(OnboardingAssistant, onboarding_assistant, ADW_TYPE_WINDOW)

static void wipe(gchar **text) {
  if (*text) { gnostr_secure_clear(*text, strlen(*text)); g_clear_pointer(text, g_free); }
}
static GSettings *settings(void) {
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  GSettingsSchema *schema = source ? g_settings_schema_source_lookup(source, "org.nostr.Grotto", TRUE) : NULL;
  if (!schema) return NULL;
  g_settings_schema_unref(schema); return g_settings_new("org.nostr.Grotto");
}
static void clear_input(OnboardingAssistant *self) {
  gtk_editable_set_text(GTK_EDITABLE(self->text_import_data), "");
  gn_secure_entry_clear(self->import_passphrase);
  wipe(&self->mnemonic); wipe(&self->nsec);
}
static void navigation(OnboardingAssistant *self) {
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_back), !self->busy && !self->identity_stored);
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back), self->state != ONBOARDING_WELCOME && !self->identity_stored);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_skip), !self->busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), !self->busy && self->capabilities_ready &&
      (self->state != ONBOARDING_BACKUP || (self->backup_presented && gtk_check_button_get_active(self->seed_written_down))));
  gtk_widget_set_visible(GTK_WIDGET(self->btn_retry), self->capabilities_ready && self->identity_stored && !self->self_test_passed && !self->busy);
  gtk_button_set_label(self->btn_next, self->state == ONBOARDING_READY ? "Open Grotto" :
      self->state == ONBOARDING_CREATE ? "Create identity" : self->state == ONBOARDING_IMPORT ? "Import identity" :
      self->state == ONBOARDING_TEST_FAILED ? "Enter management with warning" : "Next");
  gtk_widget_set_sensitive(GTK_WIDGET(self->seed_written_down), self->backup_presented && !self->busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_copy_seed), self->backup_presented && !self->busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_backup_retry), !self->busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_backup_export), !self->busy && self->identity_stored);
  gtk_spinner_set_spinning(self->spinner_status, self->busy);
}
static void status(OnboardingAssistant *self, const gchar *text) {
  gtk_label_set_text(self->lbl_status, text ? text : "");
  gtk_accessible_announce(GTK_ACCESSIBLE(self->lbl_status), text ? text : "", GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  navigation(self);
}
static void step(OnboardingAssistant *self, OnboardingState state) {
  if (self->state == ONBOARDING_BACKUP && state != ONBOARDING_BACKUP) {
    gn_secure_phrase_set_text(self->seed_words, ""); wipe(&self->mnemonic);
  }
  self->state = state;
  const gchar *page = state == ONBOARDING_WELCOME ? "welcome" : state == ONBOARDING_CHOICE ? "choice" :
    state == ONBOARDING_CREATE ? "create" : state == ONBOARDING_IMPORT ? "import" :
    state == ONBOARDING_CONNECT ? "connect" : state == ONBOARDING_BACKUP ? "backup" :
    state == ONBOARDING_READY ? "ready" : "test";
  gtk_stack_set_visible_child_name(self->steps, page); navigation(self);
}
static void finish(OnboardingAssistant *self, gboolean verified) {
  if (self->finished || self->busy) return;
  self->finished = TRUE;
  /* Dismissed setup is not verified setup; the callback carries the distinction. */
  onboarding_assistant_mark_completed();
  if (self->on_finished) self->on_finished(verified, self->on_finished_data);
  if (!verified && self->identity_stored) {
    GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(self));
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
    if (!parent && app) {
      for (GList *it = gtk_application_get_windows(app); it; it = it->next)
        if (!ONBOARDING_IS_ASSISTANT(it->data)) { parent = it->data; break; }
    }
    /* The warning survives closing setup, including callback-free retries.
     * Maintenance remains available on this exact identity, never a default. */
    if (parent) page_identity_detail_present_identity(GTK_WIDGET(parent), self->identity);
  }
  gtk_window_close(GTK_WINDOW(self));
}

typedef enum { CAPABILITIES, STORE, IMPORT, TEST, SEED } Operation;
typedef struct {
  OnboardingAssistant *self;
  GCancellable *cancel;
  guint generation;
  Operation operation;
  gchar *method, *interface, *path;
  GVariant *arguments;
} OperationCtx;
static void operation_free(OperationCtx *ctx) {
  g_object_unref(ctx->self); g_object_unref(ctx->cancel);
  g_free(ctx->method); g_free(ctx->interface); g_free(ctx->path);
  g_clear_pointer(&ctx->arguments, g_variant_unref); g_free(ctx);
}
static gboolean current(OperationCtx *ctx) {
  return !ctx->self->closed && ctx->generation == ctx->self->generation && !g_cancellable_is_cancelled(ctx->cancel);
}
static void start_call(OnboardingAssistant *self, Operation operation, gboolean management,
    const gchar *method, GVariant *arguments);
static void testing(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *interface,
    const gchar *signal, GVariant *parameters, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal;
  OnboardingAssistant *self = data; const gchar *identity; g_variant_get(parameters, "(&s)", &identity);
  if (!self->closed && self->state == ONBOARDING_TEST_WAITING && !g_strcmp0(self->identity, identity)) {
    step(self, ONBOARDING_TESTING); status(self, "Testing the approved identity locally. Nothing is published…");
  }
}
static void start_test(OnboardingAssistant *self) {
  if (!self->capabilities_ready || !self->identity_stored || self->busy || self->closed) return;
  self->test_result = ONBOARDING_TEST_NONE;
  step(self, ONBOARDING_TEST_WAITING);
  start_call(self, TEST, TRUE, "SelfTestIdentity", g_variant_new("(s)", self->identity));
  status(self, "Waiting for one-time consent: sign and verify an unpublished event; NIP-44 encrypt and decrypt a challenge. No permissions are saved.");
}
static void show_backup(OnboardingAssistant *self) {
  self->backup_presented = FALSE;
  gn_secure_phrase_set_text(self->seed_words, ""); wipe(&self->mnemonic);
  step(self, ONBOARDING_BACKUP);
  gtk_check_button_set_active(self->seed_written_down, FALSE);
  start_call(self, SEED, TRUE, "ExportIdentity", g_variant_new("(sss)", self->identity, "mnemonic", ""));
}
static void call_done(GObject *source, GAsyncResult *result, gpointer data) {
  OperationCtx *ctx = data;
  g_autoptr(GError) error = NULL;
  GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  OnboardingAssistant *self = ctx->self;
  if (!current(ctx)) { if (reply) g_variant_unref(reply); operation_free(ctx); return; }
  self->busy = FALSE;
  if (error) {
    if (ctx->operation == CAPABILITIES) {
      self->capabilities_ready = FALSE;
      status(self, "Signer unavailable or older than this UI. Update Grotto to create, import or verify identities. You can enter management without changing keys.");
    } else if (ctx->operation == TEST) {
      self->self_test_passed = FALSE; step(self, ONBOARDING_TEST_FAILED);
      g_autofree gchar *name = g_dbus_error_get_remote_error(error);
      self->test_result = name && g_str_has_suffix(name, "ApprovalTimedOut") ? ONBOARDING_TEST_EXPIRED :
          name && g_str_has_suffix(name, "ApprovalDenied") ? ONBOARDING_TEST_DENIED :
          name && g_str_has_suffix(name, "VerificationFailed") ? ONBOARDING_TEST_VERIFICATION_FAILED : ONBOARDING_TEST_KEYRING_UNAVAILABLE;
      status(self, self->test_result == ONBOARDING_TEST_EXPIRED ? "Self-test expired. Retry or enter management with a warning." :
          name && g_str_has_suffix(name, "ApprovalDenied") ? "Self-test denied. Retry or enter management with a warning." :
          "Self-test did not pass: keyring unavailable, signing paused, or verification failed. Retry or enter management with a warning.");
    } else if (ctx->operation == SEED) {
      g_autofree gchar *name = g_dbus_error_get_remote_error(error);
      self->backup_presented = FALSE;
      status(self, name && g_str_has_suffix(name, "NotFound") ?
          "No seed words are available for this imported identity. Use the encrypted export in Identity → Backup instead." :
          "Seed-word export failed. Retry from Identity → Backup when the keyring is available.");
      /* No recovery material was presented: acknowledgement cannot make Ready. */
    } else {
      clear_input(self); status(self, "Identity storage failed. Check that your keyring is available and the input is valid, then retry. A completed daemon store is preserved.");
    }
  } else if (ctx->operation == CAPABILITIES) {
    g_autoptr(GVariant) row = g_variant_get_child_value(reply, 0);
    guint32 version = 0; g_variant_lookup(row, "control_version", "u", &version);
    g_autoptr(GVariant) caps = g_variant_lookup_value(row, "capabilities", G_VARIANT_TYPE("a{sb}"));
    gboolean store = FALSE, test = FALSE;
    if (caps) { g_variant_lookup(caps, "store-mnemonic", "b", &store); g_variant_lookup(caps, "identity-self-test", "b", &test); }
    self->capabilities_ready = version >= 1 && store && test;
    status(self, self->capabilities_ready ? "" : "Update Grotto. Identity setup is read-only with this daemon.");
    if (self->capabilities_ready && self->identity_stored) start_test(self);
  } else if (ctx->operation == STORE || ctx->operation == IMPORT) {
    const gchar *identity = NULL;
    g_variant_get(reply, "(&s)", &identity);
    if (identity && *identity) {
      /* Copy all borrowed reply fields before releasing the variant. */
      g_free(self->identity); self->identity = g_strdup(identity); self->identity_stored = TRUE;
      clear_input(self);
      AccountsStore *as = accounts_store_get_default();
      if (!accounts_store_exists(as, self->identity) && !accounts_store_add(as, self->identity,
          gtk_editable_get_text(GTK_EDITABLE(ctx->operation == STORE ? self->entry_profile_name : self->entry_import_label)), NULL))
        status(self, "Identity is stored, but its local label could not be saved.");
      accounts_store_set_setup_state(as, self->identity, "verification-pending", NULL);
      accounts_store_set_selected(as, self->identity);
      accounts_store_sync_with_secrets_async(as, NULL, NULL);
      start_test(self);
    } else { clear_input(self); status(self, "Identity was not stored. Check the input and retry."); }
  } else if (ctx->operation == TEST) {
    const gchar *verdict; g_variant_get(reply, "(&s)", &verdict);
    self->self_test_passed = !strcmp(verdict, "passed");
    self->test_result = self->self_test_passed ? ONBOARDING_TEST_PASSED : ONBOARDING_TEST_VERIFICATION_FAILED;
    if (self->self_test_passed) {
      accounts_store_set_setup_state(accounts_store_get_default(), self->identity, "backup-pending", NULL);
      show_backup(self);
    }
    else { step(self, ONBOARDING_TEST_FAILED); status(self, "Self-test failed verification. Retry before relying on this identity."); }
  } else {
    const gchar *phrase; g_variant_get(reply, "(&s)", &phrase);
    wipe(&self->mnemonic); self->mnemonic = g_strdup(phrase);
    self->backup_presented = gn_secure_phrase_set_text(self->seed_words, phrase);
    status(self, "Write the seed words down privately. Anyone with them has full control of this identity.");
  }
  if (reply) g_variant_unref(reply);
  navigation(self); operation_free(ctx);
}
static void bus_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; OperationCtx *ctx = data; g_autoptr(GError) error = NULL;
  GDBusConnection *bus = g_bus_get_finish(result, &error);
  if (!current(ctx) || !bus) {
    if (current(ctx)) { ctx->self->busy = FALSE; clear_input(ctx->self); status(ctx->self, "Session signer is unavailable. Retry when it is running."); }
    g_clear_object(&bus); operation_free(ctx); return;
  }
  if (ctx->operation == TEST) {
    if (ctx->self->testing_signal) g_dbus_connection_signal_unsubscribe(ctx->self->test_bus, ctx->self->testing_signal);
    g_set_object(&ctx->self->test_bus, bus);
    ctx->self->testing_signal = g_dbus_connection_signal_subscribe(bus, "org.nostr.Signer", CONTROL_IFACE,
        "IdentitySelfTestTesting", CONTROL_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, testing, ctx->self, NULL);
  }
  g_dbus_connection_call(bus, "org.nostr.Signer", ctx->path, ctx->interface, ctx->method,
      ctx->arguments, ctx->operation == CAPABILITIES ? G_VARIANT_TYPE("(a{sv})") : G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE, KEY_TIMEOUT, ctx->cancel, call_done, ctx);
  g_clear_pointer(&ctx->arguments, g_variant_unref);
  g_object_unref(bus);
}
static void start_call(OnboardingAssistant *self, Operation operation, gboolean management,
    const gchar *method, GVariant *arguments) {
  if (self->busy || self->closed) { g_variant_ref_sink(arguments); g_variant_unref(arguments); return; }
  g_clear_object(&self->cancel); self->cancel = g_cancellable_new(); self->generation++; self->busy = TRUE;
  OperationCtx *ctx = g_new0(OperationCtx, 1);
  ctx->self = g_object_ref(self); ctx->cancel = g_object_ref(self->cancel); ctx->generation = self->generation;
  ctx->operation = operation; ctx->method = g_strdup(method);
  ctx->interface = g_strdup(management ? CONTROL_IFACE : "org.nostr.Signer");
  ctx->path = g_strdup(management ? CONTROL_PATH : "/org/nostr/signer");
  ctx->arguments = g_variant_ref_sink(arguments);
  navigation(self); g_bus_get(G_BUS_TYPE_SESSION, ctx->cancel, bus_ready, ctx);
}
static void create_identity(OnboardingAssistant *self) {
  g_autoptr(GError) error = NULL;
  clear_input(self);
  if (!gn_backup_generate_mnemonic(12, NULL, &self->mnemonic, &self->nsec, &error)) {
    status(self, "Could not generate an identity. Retry."); return;
  }
  const gchar *values[] = {self->nsec, gtk_editable_get_text(GTK_EDITABLE(self->entry_profile_name)), self->mnemonic};
  start_call(self, STORE, TRUE, "StoreKeyWithMnemonic", grotto_secret_tuple(values, G_N_ELEMENTS(values)));
  /* D-Bus owns the in-flight tuple; no seed material remains in labels/UI. */
  wipe(&self->mnemonic); wipe(&self->nsec); status(self, "Storing identity and seed words in the system keyring…");
}
static void import_identity(OnboardingAssistant *self) {
  gchar *input = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->text_import_data)));
  gsize input_size = strlen(input) + 1; g_strstrip(input);
  gchar *pass = gn_secure_entry_get_text(self->import_passphrase);
  if (!*input) { status(self, "Enter a private key or recovery phrase."); goto done; }
  gboolean mnemonic = gtk_check_button_get_active(self->radio_import_seed);
  gboolean encrypted = g_str_has_prefix(input, "ncryptsec1");
  if (encrypted && (!pass || !*pass)) { status(self, "Enter the NIP-49 decryption passphrase."); goto done; }
  /* The daemon validates mnemonic checksum/length, nsec checksum, scalar,
   * and authenticated NIP-49 before committing anything. */
  const gchar *values[] = {mnemonic ? "mnemonic" : encrypted ? "ncryptsec" : "key", input,
      mnemonic || encrypted ? (pass ? pass : "") : "", gtk_editable_get_text(GTK_EDITABLE(self->entry_import_label))};
  start_call(self, IMPORT, TRUE, "ImportIdentity", grotto_secret_tuple(values, G_N_ELEMENTS(values)));
  clear_input(self); status(self, "Validating and storing identity…");
done:
  gnostr_secure_clear(input, input_size); g_free(input); gn_secure_entry_free_text(pass);
}
static void next(GtkButton *button, gpointer data) {
  (void)button; OnboardingAssistant *self = data;
  if (self->busy || self->closed || !self->capabilities_ready) return;
  switch (self->state) {
    case ONBOARDING_WELCOME: step(self, ONBOARDING_CHOICE); break;
    case ONBOARDING_CHOICE:
      step(self, gtk_check_button_get_active(self->radio_create) ? ONBOARDING_CREATE :
          gtk_check_button_get_active(self->radio_import) ? ONBOARDING_IMPORT : ONBOARDING_CONNECT); break;
    case ONBOARDING_CREATE: create_identity(self); break;
    case ONBOARDING_IMPORT: import_identity(self); break;
    case ONBOARDING_CONNECT: finish(self, FALSE); break;
    case ONBOARDING_BACKUP:
      if (!self->backup_presented || !gtk_check_button_get_active(self->seed_written_down)) return;
      if (!accounts_store_set_setup_state(accounts_store_get_default(), self->identity, "ready", NULL)) {
        status(self, "Backup acknowledgement could not be saved. Retry or skip to management with backup pending."); return;
      }
      step(self, ONBOARDING_READY);
      gtk_label_set_text(self->ready_summary, "This identity passed signing and encryption checks. Apps ask for permission before using it. Nothing was published."); break;
    case ONBOARDING_READY: finish(self, self->self_test_passed); break;
    case ONBOARDING_TEST_FAILED: finish(self, FALSE); break;
    case ONBOARDING_TEST_WAITING: case ONBOARDING_TESTING: break;
  }
}
static void back(GtkButton *button, gpointer data) {
  (void)button; OnboardingAssistant *self = data;
  if (self->busy || self->closed || self->identity_stored) return;
  clear_input(self); status(self, "");
  step(self, self->state == ONBOARDING_CHOICE ? ONBOARDING_WELCOME : ONBOARDING_CHOICE);
}
static void skip(GtkButton *button, gpointer data) {
  (void)button; OnboardingAssistant *self = data;
  if (self->busy || self->closed) return;
  finish(self, FALSE);
}
static void backup_retry(GtkButton *button, gpointer data) { (void)button; OnboardingAssistant *self = data; if (!self->busy && self->self_test_passed) show_backup(self); }
static void backup_export(GtkButton *button, gpointer data) {
  (void)button; OnboardingAssistant *self = data;
  if (!self->busy && self->identity_stored) page_identity_detail_present_identity(GTK_WIDGET(self), self->identity);
}
static void setup_changed(AccountsStore *as, AccountsChangeType change, const gchar *id, gpointer data) {
  (void)change; (void)id; OnboardingAssistant *self = data;
  if (self->closed || self->state != ONBOARDING_BACKUP || !self->self_test_passed) return;
  AccountEntry *entry = accounts_store_find(as, self->identity);
  if (entry && !g_strcmp0(entry->setup_state, "ready")) {
    step(self, ONBOARDING_READY);
    gtk_label_set_text(self->ready_summary, "Identity verification and backup are complete. Apps still ask for permission; nothing was published.");
  }
  accounts_store_entry_free(entry);
}
static void retry(GtkButton *button, gpointer data) { (void)button; start_test(data); }
static void changed(GtkCheckButton *button, gpointer data) { (void)button; navigation(data); }
static void import_mode(GtkCheckButton *button, gpointer data) {
  OnboardingAssistant *self = data;
  gtk_label_set_text(self->import_passphrase_label, gtk_check_button_get_active(button) ?
      "Mnemonic passphrase (optional; not a keyring password)" : "NIP-49 decryption passphrase (encrypted imports only)");
  gn_secure_entry_clear(self->import_passphrase);
}
static void copy_seed(GtkButton *button, gpointer data) {
  (void)button; OnboardingAssistant *self = data;
  if (!self->mnemonic || self->closed) return;
  grotto_secret_clipboard_copy(gdk_display_get_clipboard(gtk_widget_get_display(GTK_WIDGET(self))), self->mnemonic);
  status(self, "Copied. Clipboard managers may keep seed words. Only Grotto's unchanged clipboard content is cleared after 60 seconds.");
}
static gboolean close_requested(GtkWindow *window, gpointer data) {
  (void)data; OnboardingAssistant *self = ONBOARDING_ASSISTANT(window);
  if (self->closed) return FALSE;
  self->closed = TRUE; self->generation++;
  if (self->test_bus && self->identity && (self->state == ONBOARDING_TEST_WAITING || self->state == ONBOARDING_TESTING))
    g_dbus_connection_call(self->test_bus, "org.nostr.Signer", CONTROL_PATH, CONTROL_IFACE, "CancelIdentitySelfTest",
        g_variant_new("(s)", self->identity), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
  if (self->testing_signal) { g_dbus_connection_signal_unsubscribe(self->test_bus, self->testing_signal); self->testing_signal = 0; }
  if (self->cancel) g_cancellable_cancel(self->cancel);
  clear_input(self); gn_secure_phrase_set_text(self->seed_words, "");
  /* Cancellation stops only this UI continuation; never ClearKey. */
  return FALSE;
}
static void dispose(GObject *object) {
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(object);
  if (!self->closed) close_requested(GTK_WINDOW(self), NULL);
  if (self->testing_signal) { g_dbus_connection_signal_unsubscribe(self->test_bus, self->testing_signal); self->testing_signal = 0; }
  g_clear_object(&self->test_bus);
  if (self->accounts_changed) { accounts_store_disconnect_changed(accounts_store_get_default(), self->accounts_changed); self->accounts_changed = 0; }
  g_clear_object(&self->cancel); wipe(&self->mnemonic); wipe(&self->nsec); g_clear_pointer(&self->identity, g_free);
  gtk_widget_dispose_template(GTK_WIDGET(self), TYPE_ONBOARDING_ASSISTANT);
  G_OBJECT_CLASS(onboarding_assistant_parent_class)->dispose(object);
}
static void onboarding_assistant_class_init(OnboardingAssistantClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/onboarding-assistant.ui");
#define BIND(name) gtk_widget_class_bind_template_child(wc, OnboardingAssistant, name)
  BIND(btn_backup_retry); BIND(btn_backup_export);
  BIND(steps); BIND(btn_back); BIND(btn_next); BIND(btn_skip); BIND(btn_retry); BIND(btn_copy_seed);
  BIND(radio_create); BIND(radio_import); BIND(radio_connect); BIND(radio_import_seed); BIND(seed_written_down);
  BIND(entry_profile_name); BIND(entry_import_label); BIND(text_import_data); BIND(box_import_passphrase_container);
  BIND(import_passphrase_label); BIND(lbl_status); BIND(seed_words); BIND(ready_summary); BIND(spinner_status);
#undef BIND
}
static void onboarding_assistant_init(OnboardingAssistant *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_autoptr(GtkEntryBuffer) input = gn_secure_entry_buffer_new();
  gtk_entry_set_buffer(self->text_import_data, input);
  gtk_entry_buffer_set_max_length(input, 4096);
  self->import_passphrase = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_show_strength_indicator(self->import_passphrase, FALSE);
  gtk_box_append(self->box_import_passphrase_container, GTK_WIDGET(self->import_passphrase));
  g_signal_connect(self->btn_next, "clicked", G_CALLBACK(next), self);
  g_signal_connect(self->btn_back, "clicked", G_CALLBACK(back), self);
  g_signal_connect(self->btn_skip, "clicked", G_CALLBACK(skip), self);
  g_signal_connect(self->btn_retry, "clicked", G_CALLBACK(retry), self);
  g_signal_connect(self->btn_copy_seed, "clicked", G_CALLBACK(copy_seed), self);
  g_signal_connect(self->btn_backup_retry, "clicked", G_CALLBACK(backup_retry), self);
  g_signal_connect(self->btn_backup_export, "clicked", G_CALLBACK(backup_export), self);
  self->accounts_changed = accounts_store_connect_changed(accounts_store_get_default(), setup_changed, self);
  g_signal_connect(self->seed_written_down, "toggled", G_CALLBACK(changed), self);
  g_signal_connect(self->radio_import_seed, "toggled", G_CALLBACK(import_mode), self);
  g_signal_connect(self, "close-request", G_CALLBACK(close_requested), NULL);
  step(self, ONBOARDING_WELCOME);
  start_call(self, CAPABILITIES, TRUE, "GetStatus", g_variant_new("()"));
}
OnboardingAssistant *onboarding_assistant_new(void) { return g_object_new(TYPE_ONBOARDING_ASSISTANT, NULL); }
void onboarding_assistant_verify_identity(GtkWindow *parent, const gchar *npub) {
  g_return_if_fail(npub && *npub);
  OnboardingAssistant *self = onboarding_assistant_new();
  self->identity = g_strdup(npub); self->identity_stored = TRUE;
  gtk_window_set_transient_for(GTK_WINDOW(self), parent);
  gtk_window_set_application(GTK_WINDOW(self), gtk_window_get_application(parent));
  gtk_window_present(GTK_WINDOW(self)); /* capability reply starts the exact-identity test */
}
static void presented_gone(gpointer data, GObject *object) {
  GtkWindow *parent = data;
  if (g_object_get_data(G_OBJECT(parent), "grotto-onboarding-assistant") == object)
    g_object_set_data(G_OBJECT(parent), "grotto-onboarding-assistant", NULL);
  g_object_unref(parent);
}
void onboarding_assistant_present(GtkWindow *parent) {
  OnboardingAssistant *self = g_object_get_data(G_OBJECT(parent), "grotto-onboarding-assistant");
  if (self && !self->closed) { gtk_window_present(GTK_WINDOW(self)); return; }
  self = onboarding_assistant_new();
  g_object_set_data(G_OBJECT(parent), "grotto-onboarding-assistant", self);
  g_object_weak_ref(G_OBJECT(self), presented_gone, g_object_ref(parent));
  gtk_window_set_transient_for(GTK_WINDOW(self), parent);
  gtk_window_set_application(GTK_WINDOW(self), gtk_window_get_application(parent));
  gtk_window_present(GTK_WINDOW(self));
}
void onboarding_assistant_set_on_finished(OnboardingAssistant *self, OnboardingAssistantFinishedCb cb, gpointer data) {
  g_return_if_fail(ONBOARDING_IS_ASSISTANT(self)); self->on_finished = cb; self->on_finished_data = data;
}
OnboardingTestResult onboarding_assistant_get_test_result(OnboardingAssistant *self) { return self->test_result; }
OnboardingState onboarding_assistant_get_state(OnboardingAssistant *self) { return self->state; }
gboolean onboarding_assistant_identity_stored(OnboardingAssistant *self) { return self->identity_stored; }
gboolean onboarding_assistant_self_test_passed(OnboardingAssistant *self) { return self->self_test_passed; }
const gchar *onboarding_assistant_get_identity(OnboardingAssistant *self) { return self->identity; }
gboolean onboarding_assistant_check_should_show(void) {
  g_autoptr(GSettings) s = settings();
  if (s && g_settings_get_boolean(s, "onboarding-completed")) return FALSE;
  /* ListIdentities is authoritative; watch-only metadata never suppresses setup. */
  GPtrArray *identities = secret_store_list();
  gboolean has_keys = identities && identities->len;
  if (identities) g_ptr_array_unref(identities);
  return !has_keys;
}
void onboarding_assistant_mark_completed(void) { g_autoptr(GSettings) s = settings(); if (s) g_settings_set_boolean(s, "onboarding-completed", TRUE); }
void onboarding_assistant_reset(void) { g_autoptr(GSettings) s = settings(); if (s) g_settings_set_boolean(s, "onboarding-completed", FALSE); }
