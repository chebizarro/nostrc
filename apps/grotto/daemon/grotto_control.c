#include "grotto_control.h"
#include "grotto_clients.h"
#include "grotto_control_xml.h"
#include "signer_caller.h"
#include "event_history.h"
#include "settings_manager.h"
#include <nostr/nip55l/signer_ops.h>
#include <string.h>
#include <stdlib.h>
#include <secure_buf.h>
#include <keys.h>
#include <nostr-utils.h>
#include <nostr/nip55l/error.h>
#include <nostr/crypto/bip39.h>
#include <nostr/nip19/nip19.h>
#include "key_rotation.h"
#include <nostrc-profile-guard.h>
extern char *nostr_nip06_private_key_from_seed(const unsigned char *seed);

struct _GrottoControl {
  gint refs;
  gboolean closing, export_busy, reset_busy, trust_installed;
  GDBusConnection *connection;
  SignerApprovalService *approval;
  GrottoClients *clients;
  GnEventHistory *history;
  BunkerService *bunker;
  guint registration, subscription;
  gulong history_handler;
  guint64 requests_revision, clients_revision, status_revision, epoch_base, seen_permissions, seen_clients;
  gchar *epoch;
  gboolean desired_bunker, client_healthy;
  gpointer active_rotation; /* owner-thread pointer to the current worker job */
};
static void cancel_rotation_if_paused(GrottoControl *c);
static SignerPermissions *permissions(GrottoControl *c) { return signer_approval_service_permissions(c->approval); }
static void emit(GrottoControl *c, const gchar *signal, guint64 revision) {
  g_dbus_connection_emit_signal(c->connection, NULL, GROTTO_CONTROL_PATH,
      GROTTO_CONTROL_INTERFACE, signal, g_variant_new("(t)", revision), NULL);
}
static void status_changed(GrottoControl *c) { emit(c, "StatusChanged", ++c->status_revision); }
static void activity_changed(GnEventHistory *history, guint64 sequence, gpointer data) {
  (void)history; GrottoControl *c = data;
  emit(c, "ActivityChanged", sequence); status_changed(c);
}
static void refresh(GrottoControl *c) {
  guint64 p = signer_permissions_revision(permissions(c)), clients = grotto_clients_revision(c->clients);
  if (c->seen_permissions != p || c->seen_clients != clients) {
    c->seen_permissions = p; c->seen_clients = clients;
    emit(c, "ClientsChanged", ++c->clients_revision); status_changed(c);
  }
}
static void approval_table_changed(gpointer data) {
  GrottoControl *c = data;
  cancel_rotation_if_paused(c);
  emit(c, "RequestsChanged", ++c->requests_revision); refresh(c); status_changed(c);
}
static guint64 permission_revision(GrottoControl *c) {
  return c->epoch_base + signer_permissions_revision(permissions(c));
}
static const gchar *bunker_state(GrottoControl *c) {
  if (!c->bunker) return c->desired_bunker ? "ERROR" : "DISABLED";
  switch (bunker_service_get_state(c->bunker)) {
    case BUNKER_STATE_STARTING: return "STARTING";
    case BUNKER_STATE_RUNNING: return "LISTENING";
    case BUNKER_STATE_DEGRADED: return "DEGRADED";
    case BUNKER_STATE_OFFLINE: return "OFFLINE";
    case BUNKER_STATE_STOPPING: return "STOPPING";
    case BUNKER_STATE_ERROR: return "ERROR";
    default: return "DISABLED";
  }
}
static GVariant *requests(GrottoControl *c) {
  g_autoptr(GVariant) legacy = signer_approval_service_list_pending(c->approval);
  GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  for (gsize i = 0; i < g_variant_n_children(legacy); i++) {
    const gchar *who, *identity, *op, *preview, *id;
    g_variant_get_child(legacy, i, "(&s&s&s&s&s)", &who, &identity, &op, &preview, &id);
    g_autoptr(GVariant) info = signer_approval_service_info(c->approval, id);
    if (info) g_variant_builder_add_value(&rows, info);
  }
  return g_variant_ref_sink(g_variant_builder_end(&rows));
}
static GVariant *status(GrottoControl *c) {
  gboolean permission_healthy = signer_permissions_reload(permissions(c), NULL);
  refresh(c);
  GVariantBuilder row, caps; g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init(&caps, G_VARIANT_TYPE("a{sb}"));
  const gchar *enabled[] = {"clients", "activity", "scoped-permissions", "trust-tiers", "pause", "identity-self-test"};
  for (guint i = 0; i < G_N_ELEMENTS(enabled); i++) g_variant_builder_add(&caps, "{sb}", enabled[i], TRUE);
  const gchar *custody[] = {"rotate-identity", "mnemonic-export", "store-mnemonic", "identity-import"};
  for (guint i = 0; i < G_N_ELEMENTS(custody); i++)
    g_variant_builder_add(&caps, "{sb}", custody[i], nostr_nip55l_mnemonic_custody_supported() != 0);
  g_variant_builder_add(&caps, "{sb}", "raw-nsec-export", TRUE);
#ifdef GROTTO_HAVE_NIP49_EXPORT
  g_variant_builder_add(&caps, "{sb}", "export-identity", TRUE);
#else
  g_variant_builder_add(&caps, "{sb}", "export-identity", FALSE);
#endif
  g_variant_builder_add(&caps, "{sb}", "reset-bunker-endpoint", c->bunker != NULL);
  const gchar *disabled[] = {"bunker-configuration", "pairing", "key-lock"};
  for (guint i = 0; i < G_N_ELEMENTS(disabled); i++) g_variant_builder_add(&caps, "{sb}", disabled[i], FALSE);
  g_variant_builder_add(&row, "{sv}", "control_version", g_variant_new_uint32(1));
  g_variant_builder_add(&row, "{sv}", "control_epoch", g_variant_new_string(c->epoch));
  g_variant_builder_add(&row, "{sv}", "capabilities", g_variant_builder_end(&caps));
  g_variant_builder_add(&row, "{sv}", "paused", g_variant_new_boolean(signer_approval_service_is_paused(c->approval)));
  g_autoptr(GVariant) pending = requests(c);
  g_variant_builder_add(&row, "{sv}", "pending_count", g_variant_new_uint32(g_variant_n_children(pending)));
  g_variant_builder_add(&row, "{sv}", "bunker_enabled", g_variant_new_boolean(c->desired_bunker));
  g_variant_builder_add(&row, "{sv}", "bunker_state", g_variant_new_string(bunker_state(c)));
  const gchar *bunker_error = c->bunker ? bunker_service_get_error(c->bunker) : NULL;
  g_variant_builder_add(&row, "{sv}", "bunker_error", g_variant_new_string(bunker_error ? bunker_error : ""));
  g_variant_builder_add(&row, "{sv}", "bunker_replay_store_healthy", g_variant_new_boolean(c->bunker && bunker_service_replay_healthy(c->bunker)));
  g_variant_builder_add(&row, "{sv}", "key_state", g_variant_new_string("unknown"));
  g_variant_builder_add(&row, "{sv}", "key_lock_enforced", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&row, "{sv}", "activity_log_healthy", g_variant_new_boolean(gn_event_history_activity_healthy(c->history)));
  g_variant_builder_add(&row, "{sv}", "client_store_healthy", g_variant_new_boolean(c->client_healthy));
  g_variant_builder_add(&row, "{sv}", "permission_store_healthy", g_variant_new_boolean(permission_healthy));
#define REV(k,v) g_variant_builder_add(&row, "{sv}", k, g_variant_new_uint64(v))
  REV("requests_revision", c->requests_revision); REV("clients_revision", c->clients_revision);
  REV("status_revision", c->status_revision); REV("permissions_revision", permission_revision(c));
  REV("activity_sequence", gn_event_history_activity_sequence(c->history));
#undef REV
  return g_variant_ref_sink(g_variant_builder_end(&row));
}
static void approval_changed(gpointer data, GVariant *snapshot) {
  GrottoControl *c = data; const gchar *state = NULL;
  g_variant_lookup(snapshot, "state", "&s", &state);
  if (g_strcmp0(state, "PENDING")) {
    gboolean consented = FALSE, remember = FALSE;
    const gchar *principal = "", *transport = "", *claimed = "", *identity = "";
    g_variant_lookup(snapshot, "principal", "&s", &principal); g_variant_lookup(snapshot, "transport", "&s", &transport);
    g_variant_lookup(snapshot, "claimed_app_id", "&s", &claimed); g_variant_lookup(snapshot, "identity", "&s", &identity);
    g_variant_lookup(snapshot, "explicit_consent", "b", &consented); g_variant_lookup(snapshot, "remember", "b", &remember);
    gboolean known = grotto_clients_known(c->clients, principal);
    gboolean success = !g_strcmp0(state, "SUCCEEDED");
    /* No admission from denials, arbitrary failed calls, or cancelled work.
     * A remembered denial is still a permission record, but not Low admission.
     * A success refreshes any known record's observed transport, identity and
     * last use, including permission-created clients (whose automatic allows
     * are not consent decisions); that never sets consent or changes tier. */
    gboolean admission = consented && (success || !g_strcmp0(state, "FAILED"));
    if (strcmp(transport, "MANAGEMENT") && *principal && (admission || (success && known) || (!g_strcmp0(state, "DENIED") && remember))) {
      c->client_healthy = grotto_clients_record(c->clients, principal, transport, claimed,
          *identity ? identity : NULL, admission, success, NULL);
      const gchar *operation = NULL;
      g_variant_lookup(snapshot, "operation", "&s", &operation);
      if (success && admission && !g_strcmp0(operation, "connect") && g_str_has_prefix(principal, "nip46:"))
        c->client_healthy = grotto_clients_set_pairing(c->clients, principal, TRUE, NULL, NULL);
      if (admission && c->client_healthy) signer_permissions_policy_changed(permissions(c));
    }
    gn_event_history_record_activity(c->history, snapshot);
  }
  refresh(c); status_changed(c);
}
static void error_reply(GDBusMethodInvocation *inv, GError *error) {
  const gchar *name = GROTTO_CONTROL_ERROR "PersistenceFailed";
  if (error && error->domain == G_IO_ERROR) {
    switch (error->code) {
      case G_IO_ERROR_BUSY: name = GROTTO_CONTROL_ERROR "Conflict"; break;
      case G_IO_ERROR_INVALID_ARGUMENT: case G_IO_ERROR_INVALID_DATA: name = GROTTO_CONTROL_ERROR "InvalidInput"; break;
      case G_IO_ERROR_NOT_FOUND: name = GROTTO_CONTROL_ERROR "NotFound"; break;
      default: break;
    }
  }
  /* Do not forward file contents, keyring details or user input into errors. */
  g_dbus_method_invocation_return_dbus_error(inv, name, "Management operation failed; re-read the snapshot before retrying");
}
static void reply_rows(GDBusMethodInvocation *inv, GVariant *rows) {
  g_dbus_method_invocation_return_value(inv, g_variant_new("(@aa{sv})", rows));
}
static void bunker_unavailable(GrottoControl *c, GDBusMethodInvocation *inv) {
  gboolean ready = c->bunker && bunker_service_get_state(c->bunker) == BUNKER_STATE_RUNNING;
  g_dbus_method_invocation_return_dbus_error(inv, ready ? GROTTO_CONTROL_ERROR "NotSupported" : GROTTO_CONTROL_ERROR "NotReady",
      "Bunker management requires the G05/G06 request-aware transport and pairing implementation");
}
static void control_unref(GrottoControl *c) {
  if (!g_atomic_int_dec_and_test(&c->refs)) return;
  grotto_clients_free(c->clients); g_clear_object(&c->history);
  signer_approval_service_unref(c->approval); g_object_unref(c->connection); g_free(c->epoch); g_free(c);
}
typedef struct {
  GrottoControl *control;
  GDBusMethodInvocation *invocation;
  gchar *identity, *format, *passphrase, *backup;
  gchar *key, *mnemonic, *import_format, *migration, *old_npub;
  guint64 generation;
  gint cancelled, result;
} ExportJob;
static void cancel_rotation_if_paused(GrottoControl *c) {
  ExportJob *job = c->active_rotation;
  if (job && (c->closing || signer_approval_service_is_paused(c->approval) ||
      job->generation != signer_approval_service_generation(c->approval))) g_atomic_int_set(&job->cancelled, TRUE);
}
static void wipe_text(gchar *s) { if (s) { secure_wipe(s, strlen(s)); g_free(s); } }
static void export_job_free(gpointer data) {
  ExportJob *job = data;
  g_object_unref(job->invocation); g_free(job->identity); g_free(job->format);
  wipe_text(job->key); wipe_text(job->mnemonic); g_free(job->import_format); g_free(job->migration); g_free(job->old_npub);
  wipe_text(job->passphrase); wipe_text(job->backup); control_unref(job->control); g_free(job);
}
static void export_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
  (void)source; (void)cancel; ExportJob *job = data;
  if (!strcmp(job->format, "__store"))
    job->result = nostr_nip55l_store_key_with_mnemonic(job->key, job->identity, job->mnemonic, &job->backup);
  else if (!strcmp(job->format, "__import"))
    job->result = nostr_nip55l_import_identity(job->import_format, job->key, job->passphrase, job->identity, &job->backup);
  else if (!strcmp(job->format, "__rotate")) {
    char *old_npub = NULL, *new_hex = NULL, *old_hex = NULL, *event = NULL, *signed_event = NULL;
    unsigned char seed[64] = {0}, old_pk[32];
    job->result = g_atomic_int_get(&job->cancelled) ? NOSTR_SIGNER_ERROR_PERMISSION_DENIED :
        nostr_nip55l_resolve_npub(job->identity, &old_npub);
    if (!job->result) {
      job->mnemonic = nostr_bip39_generate(12);
      if (!job->mnemonic || !nostr_bip39_seed(job->mnemonic, "", seed)) job->result = NOSTR_SIGNER_ERROR_CRYPTO_FAILED;
      else job->key = nostr_nip06_private_key_from_seed(seed);
      secure_wipe(seed, sizeof(seed));
      if (!job->key) job->result = NOSTR_SIGNER_ERROR_CRYPTO_FAILED;
    }
    if (!job->result) {
      new_hex = nostr_key_get_public(job->key);
      if (!new_hex || nostr_nip19_decode_npub(old_npub, old_pk)) job->result = NOSTR_SIGNER_ERROR_INVALID_KEY;
      else {
        old_hex = g_malloc(65);
        for (guint i = 0; i < 32; i++) g_snprintf(old_hex + 2*i, 3, "%02x", old_pk[i]);
        event = key_rotation_build_migration_event(old_hex, new_hex, 0, NULL);
        job->result = g_atomic_int_get(&job->cancelled) ? NOSTR_SIGNER_ERROR_PERMISSION_DENIED :
            event ? nostr_nip55l_sign_event_json(event, old_npub, "", &signed_event) : NOSTR_SIGNER_ERROR_BACKEND;
      }
    }
    if (!job->result) job->result = g_atomic_int_get(&job->cancelled) ? NOSTR_SIGNER_ERROR_PERMISSION_DENIED :
        nostr_nip55l_store_key_with_mnemonic(job->key, "", job->mnemonic, &job->backup);
    if (!job->result) { job->migration = g_strdup(signed_event); job->old_npub = g_strdup(old_npub); }
    free(old_npub); free(new_hex); g_free(old_hex); g_free(event); free(signed_event);
    /* No clients, defaults or permissions are rewritten; neither key removed.
     * Migration is saved for explicit publication by a consuming application. */
  } else job->result = nostr_nip55l_export_identity(job->identity, job->format, job->passphrase, &job->backup);
  wipe_text(job->passphrase); job->passphrase = NULL;
  g_task_return_boolean(task, TRUE);
}
static void export_finished(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; (void)data; ExportJob *job = g_task_get_task_data(G_TASK(result));
  GrottoControl *c = job->control; c->export_busy = FALSE;
  if (job->backup && (!strcmp(job->format, "__store") || !strcmp(job->format, "__import") || !strcmp(job->format, "__rotate")))
    signer_notify_identities_changed(); /* store is durable even if its UI closed */
  if (c->active_rotation == job) c->active_rotation = NULL;
  if (!strcmp(job->format, "__rotate") && (g_atomic_int_get(&job->cancelled) ||
      job->generation != signer_approval_service_generation(c->approval))) {
    g_dbus_method_invocation_return_dbus_error(job->invocation, GROTTO_CONTROL_ERROR "Paused",
        "Rotation interrupted by signing pause; any completed key store is preserved and can be rediscovered"); return;
  }
  if (c->closing) {
    g_dbus_method_invocation_return_dbus_error(job->invocation, GROTTO_CONTROL_ERROR "NotReady", "Signer management is shutting down"); return;
  }
  /* Keyring prompts can outlive the original process. Re-attest immediately
   * before disclosing a secret; bus-name ownership is still not authority. */
  g_autoptr(SignerCaller) caller = signer_caller_identify_fresh(c->connection, g_dbus_method_invocation_get_sender(job->invocation));
  if (!signer_caller_is_approver(caller)) {
    g_dbus_method_invocation_return_dbus_error(job->invocation, GROTTO_CONTROL_ERROR "NotAuthorized", "Approver is no longer available"); return;
  }
  if (job->result) {
    const gchar *name = job->result == NOSTR_SIGNER_ERROR_NOT_SUPPORTED ? GROTTO_CONTROL_ERROR "NotSupported" :
        (job->result == NOSTR_SIGNER_ERROR_INVALID_ARG || job->result == NOSTR_SIGNER_ERROR_INVALID_KEY) ? GROTTO_CONTROL_ERROR "InvalidInput" :
        job->result == NOSTR_SIGNER_ERROR_NOT_FOUND ? GROTTO_CONTROL_ERROR "NotFound" : GROTTO_CONTROL_ERROR "BackendUnavailable";
    g_dbus_method_invocation_return_dbus_error(job->invocation, name, "Identity export could not be completed"); return;
  }
  if (job->migration) {
    g_autofree gchar *dir = g_build_filename(g_get_user_config_dir(), "grotto", "migrations", NULL);
    g_autofree gchar *path = g_build_filename(dir, job->old_npub, NULL);
    if (nostrc_profile_guard_refuses_path(path) || g_mkdir_with_parents(dir, 0700) ||
        !g_file_set_contents_full(path, job->migration, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, NULL)) {
      g_dbus_method_invocation_return_dbus_error(job->invocation, GROTTO_CONTROL_ERROR "PersistenceFailed",
          "Successor identity is stored, but the migration announcement could not be saved; re-read identities"); return;
    }
  }
  if (!strcmp(job->format, "__store") || !strcmp(job->format, "__import") || !strcmp(job->format, "__rotate")) status_changed(c);
  g_dbus_method_invocation_return_value(job->invocation, g_variant_new("(s)", job->backup));
  wipe_text(job->backup); job->backup = NULL;
}
static void start_export(GrottoControl *c, GDBusMethodInvocation *inv, const gchar *identity,
    const gchar *format, const gchar *passphrase) {
  ExportJob *job = g_new0(ExportJob, 1); job->control = c; g_atomic_int_inc(&c->refs);
  job->invocation = g_object_ref(inv); job->identity = g_strdup(identity);
  job->format = g_strdup(format); job->passphrase = g_strdup(passphrase); c->export_busy = TRUE;
  job->generation = signer_approval_service_generation(c->approval);
  if (!strcmp(format, "__rotate")) c->active_rotation = job;
  g_autoptr(GTask) task = g_task_new(NULL, NULL, export_finished, NULL);
  g_task_set_task_data(task, job, export_job_free); g_task_run_in_thread(task, export_worker);
}
static void start_store(GrottoControl *c, GDBusMethodInvocation *inv, const gchar *key,
    const gchar *label, const gchar *mnemonic, const gchar *format, const gchar *passphrase) {
  ExportJob *job = g_new0(ExportJob, 1); job->control = c; g_atomic_int_inc(&c->refs);
  job->invocation = g_object_ref(inv); job->identity = g_strdup(label); job->format = g_strdup(format ? "__import" : "__store");
  job->import_format = g_strdup(format); job->passphrase = g_strdup(passphrase);
  job->key = g_strdup(key); job->mnemonic = g_strdup(mnemonic); c->export_busy = TRUE;
  g_autoptr(GTask) task = g_task_new(NULL, NULL, export_finished, NULL);
  g_task_set_task_data(task, job, export_job_free); g_task_run_in_thread(task, export_worker);
}
static void self_test_testing(gpointer data) {
  GDBusMethodInvocation *inv = data;
  const gchar *identity; g_variant_get(g_dbus_method_invocation_get_parameters(inv), "(&s)", &identity);
  g_dbus_connection_emit_signal(g_dbus_method_invocation_get_connection(inv), g_dbus_method_invocation_get_sender(inv),
      GROTTO_CONTROL_PATH, GROTTO_CONTROL_INTERFACE, "IdentitySelfTestTesting", g_variant_new("(s)", identity), NULL);
}
static void self_test_reply(gpointer data, const gchar *error, const gchar *message, const gchar *result) {
  GDBusMethodInvocation *inv = data;
  if (error) g_dbus_method_invocation_return_dbus_error(inv, error, message);
  else g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", result));
  g_object_unref(inv);
}
typedef struct { GrottoControl *control; GDBusMethodInvocation *invocation; gint result; } ResetJob;
static void reset_free(gpointer data) {
  ResetJob *job = data; g_object_unref(job->invocation); control_unref(job->control); g_free(job);
}
static void reset_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
  (void)source; (void)cancel; ResetJob *job = data;
  char *secret = nostr_key_generate_private(); unsigned char key[32] = {0};
  job->result = secret && nostr_hex2bin(key, secret, sizeof(key)) ? nostr_nip55l_bunker_transport_store(key) : NOSTR_SIGNER_ERROR_BACKEND;
  if (secret) { secure_wipe(secret, strlen(secret)); free(secret); }
  secure_wipe(key, sizeof(key)); g_task_return_boolean(task, TRUE);
}
static void reset_finished(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; (void)data; ResetJob *job = g_task_get_task_data(G_TASK(result)); GrottoControl *c = job->control;
  c->reset_busy = FALSE;
  if (c->closing) { g_dbus_method_invocation_return_dbus_error(job->invocation, GROTTO_CONTROL_ERROR "NotReady", "Signer is shutting down"); return; }
  if (job->result) {
    bunker_service_fail_transport(c->bunker);
    g_dbus_method_invocation_return_dbus_error(job->invocation, GROTTO_CONTROL_ERROR "BackendUnavailable", "Transport key reset failed; remote signing remains disabled");
  } else if (!bunker_service_reset_replay(c->bunker)) {
    bunker_service_fail_transport(c->bunker);
    g_dbus_method_invocation_return_dbus_error(job->invocation, GROTTO_CONTROL_ERROR "PersistenceFailed", "New endpoint stored but replay protection could not be reset; remote signing remains disabled");
  } else {
    if (c->desired_bunker) bunker_service_restart(c->bunker);
    g_dbus_method_invocation_return_value(job->invocation, NULL);
  }
  status_changed(c);
}
static void reset_endpoint(GrottoControl *c, GDBusMethodInvocation *inv) {
  if (!c->bunker) { bunker_unavailable(c, inv); return; }
  if (c->reset_busy) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "Busy", "Endpoint reset is in progress"); return; }
  bunker_service_stop(c->bunker);
  /* Revoke before key replacement. A partial persistence failure leaves the
   * listener stopped; retry is idempotent and never preserves old pairings. */
  /* Include orphaned grants as well as inventory rows; an interrupted reset
   * must not leave scopes that a later re-pairing could resurrect. */
  g_autoptr(GError) policy_error = NULL;
  g_autoptr(GVariant) grants = signer_permissions_list(permissions(c), NULL, &policy_error);
  if (!grants) { bunker_service_fail_transport(c->bunker); error_reply(inv, policy_error); return; }
  for (gsize i = 0; i < g_variant_n_children(grants); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(grants, i); const gchar *principal = NULL;
    g_variant_lookup(row, "principal", "&s", &principal);
    if (!principal || !g_str_has_prefix(principal, "nip46:")) continue;
    signer_approval_service_cancel_principal(c->approval, principal);
    gboolean removed;
    if (!signer_permissions_clear(permissions(c), principal, NULL, NULL, &removed, &policy_error)) {
      bunker_service_fail_transport(c->bunker); error_reply(inv, policy_error); return;
    }
  }
  g_autoptr(GVariant) clients = grotto_clients_list(c->clients);
  for (gsize i = 0; i < g_variant_n_children(clients); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(clients, i); const gchar *principal = NULL;
    g_variant_lookup(row, "principal", "&s", &principal);
    if (!principal || !g_str_has_prefix(principal, "nip46:")) continue;
    signer_approval_service_cancel_principal(c->approval, principal);
    gboolean removed; g_autoptr(GError) error = NULL;
    if (!grotto_clients_remove(c->clients, principal, &error) ||
        !signer_permissions_clear(permissions(c), principal, NULL, NULL, &removed, &error)) {
      bunker_service_fail_transport(c->bunker); error_reply(inv, error); return;
    }
  }
  signer_permissions_policy_changed(permissions(c)); refresh(c);
  ResetJob *job = g_new0(ResetJob, 1); job->control = c; g_atomic_int_inc(&c->refs);
  job->invocation = g_object_ref(inv); c->reset_busy = TRUE;
  g_autoptr(GTask) task = g_task_new(NULL, NULL, reset_finished, NULL);
  g_task_set_task_data(task, job, reset_free); g_task_run_in_thread(task, reset_worker);
}
static void method_call(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *method, GVariant *args, GDBusMethodInvocation *inv, gpointer data) {
  (void)path; (void)interface; GrottoControl *c = data;
  g_autoptr(SignerCaller) who = signer_caller_identify_fresh(bus, sender);
  gboolean pause = FALSE;
  if (!strcmp(method, "SetPaused")) g_variant_get(args, "(b)", &pause);
  /* All methods are checked before dispatch, including unavailable methods.
   * Neither owning the GUI's bus name nor caller-supplied labels are authority. */
  gboolean approver = signer_caller_is_approver(who);
  if (!(approver || (who->same_uid && (!strcmp(method, "GetStatus") || (!strcmp(method, "SetPaused") && pause))))) {
    g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "NotAuthorized", "A trusted Grotto approver is required"); return;
  }
  g_autoptr(GError) error = NULL;
  if (!strcmp(method, "GetStatus")) {
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", status(c))); return;
  }
  if (!strcmp(method, "ListRequests")) { reply_rows(inv, requests(c)); return; }
  if (!strcmp(method, "ListClients")) {
    g_autoptr(GVariant) raw = grotto_clients_list(c->clients);
    GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
    for (gsize i = 0; i < g_variant_n_children(raw); i++) {
      g_autoptr(GVariant) row = g_variant_get_child_value(raw, i); const gchar *principal;
      g_variant_lookup(row, "principal", "&s", &principal);
      g_autoptr(GVariant) grants = signer_permissions_list(permissions(c), principal, &error);
      if (!grants) { g_variant_builder_clear(&rows); error_reply(inv, error); return; }
      GVariantDict enriched; g_variant_dict_init(&enriched, row);
      g_variant_dict_insert(&enriched, "permission-count", "u", (guint)g_variant_n_children(grants));
      g_variant_dict_insert(&enriched, "needs-review", "b", signer_permissions_needs_review(permissions(c), principal, &error));
      if (error) { g_variant_dict_clear(&enriched); g_variant_builder_clear(&rows); error_reply(inv, error); return; }
      g_variant_builder_add_value(&rows, g_variant_dict_end(&enriched));
    }
    refresh(c); reply_rows(inv, g_variant_builder_end(&rows)); return;
  }
  if (!strcmp(method, "ListPermissions")) {
    const gchar *p; g_variant_get(args, "(&s)", &p);
    GVariant *rows = signer_permissions_list(permissions(c), p, &error);
    refresh(c); if (!rows) error_reply(inv, error); else reply_rows(inv, rows); return;
  }
  if (!strcmp(method, "SetPermissions")) {
    const gchar *p, *identity; guint64 expected; g_autoptr(GVariant) rows = NULL;
    g_variant_get(args, "(&s&s@aa{sv}t)", &p, &identity, &rows, &expected);
    if (!grotto_clients_principal_valid(p) || strlen(identity) > 4096) {
      g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "InvalidInput", "Invalid client principal or identity"); return;
    }
    if (!signer_permissions_reload(permissions(c), &error)) { error_reply(inv, error); return; }
    refresh(c);
    if (expected != permission_revision(c)) {
      g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "Conflict", "Permission snapshot is stale; re-read it"); return;
    }
    if (!signer_permissions_replace(permissions(c), p, identity, rows, signer_permissions_revision(permissions(c)), &error)) {
      refresh(c); error_reply(inv, error); return;
    }
    if (g_variant_n_children(rows)) {
      c->client_healthy = grotto_clients_record(c->clients, p, NULL, NULL, NULL, FALSE, FALSE, &error);
      if (!c->client_healthy) { refresh(c); error_reply(inv, error); return; }
    }
    refresh(c);
  } else if (!strcmp(method, "SetClientLabel") || !strcmp(method, "SetClientTrust")) {
    const gchar *p, *value; g_variant_get(args, "(&s&s)", &p, &value);
    gboolean ok;
    if (!strcmp(method, "SetClientLabel")) ok = grotto_clients_set_label(c->clients, p, value, &error);
    else {
      SignerTrustTier tier = !strcmp(value, "low") ? SIGNER_TRUST_LOW : !strcmp(value, "medium") ? SIGNER_TRUST_MEDIUM : !strcmp(value, "full") ? SIGNER_TRUST_FULL : -1;
      ok = grotto_clients_set_tier(c->clients, p, tier, &error);
      if (ok) signer_permissions_policy_changed(permissions(c));
    }
    if (!ok) { error_reply(inv, error); return; } refresh(c);
  } else if (!strcmp(method, "RevokeClient")) {
    const gchar *p; gboolean removed; g_variant_get(args, "(&s)", &p);
    /* Cancel first, even when persistence fails. Removing the client invalidates
     * live tier admission before permissions are removed. Retrying is idempotent. */
    signer_approval_service_cancel_principal(c->approval, p);
    if (!grotto_clients_remove(c->clients, p, &error)) { error_reply(inv, error); return; }
    signer_permissions_policy_changed(permissions(c));
    if (!signer_permissions_clear(permissions(c), p, NULL, NULL, &removed, &error)) { refresh(c); error_reply(inv, error); return; }
    if (c->bunker && g_str_has_prefix(p, "nip46:")) bunker_service_disconnect_client(c->bunker, p + 6);
    refresh(c);
  } else if (!strcmp(method, "SetPaused")) {
    gboolean changed = signer_approval_service_is_paused(c->approval) != pause;
    signer_approval_service_set_paused(c->approval, pause); if (changed) status_changed(c);
  } else if (!strcmp(method, "ListActivity")) {
    guint64 before; guint limit; g_variant_get(args, "(tu)", &before, &limit);
    if (limit > 100) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "InvalidInput", "Activity limit must be at most 100"); return; }
    reply_rows(inv, gn_event_history_list_activity(c->history, before, limit)); return;
  } else if (!strcmp(method, "ClearActivity")) {
    if (!gn_event_history_clear_activity(c->history, &error)) { status_changed(c); error_reply(inv, error); return; }
    status_changed(c);
  } else if (!strcmp(method, "SetDefaultIdentityForLegacy")) {
    const gchar *identity; g_variant_get(args, "(&s)", &identity); char *selector = NULL, *npub = NULL;
    int rc = *identity ? nostr_nip55l_normalize_selector(identity, &selector, &npub) : 0;
    if (rc) { free(selector); free(npub); g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "InvalidInput", "Identity must name a stored signable identity"); return; }
    gboolean saved = g_settings_set_string(settings_manager_get_gsettings(settings_manager_get_default()),
        "default-identity", npub ? npub : "");
    free(selector); free(npub);
    if (!saved) { error_reply(inv, NULL); return; } status_changed(c);
  } else if (!strcmp(method, "ConfigureBunker")) {
    /* TODO G06: validate/persist bunker.ini v1 and apply desired config live. */
    bunker_unavailable(c, inv); return;
  } else if (!strcmp(method, "CreatePairing")) {
    /* TODO G05/G06: single-use secret, account/transport binding and consent. */
    bunker_unavailable(c, inv); return;
  } else if (!strcmp(method, "AcceptConnectURI")) {
    /* TODO G06: validated URI, chosen identity, shared review, encrypted ack. */
    bunker_unavailable(c, inv); return;
  } else if (!strcmp(method, "CancelPairing")) {
    /* TODO G06: invalidate token and cancel the shared pairing request. */
    bunker_unavailable(c, inv); return;
  } else if (!strcmp(method, "ResetBunkerEndpoint")) {
    reset_endpoint(c, inv); return;
  } else if (!strcmp(method, "StoreKeyWithMnemonic")) {
    const gchar *key, *label, *mnemonic;
    g_variant_get(args, "(&s&s&s)", &key, &label, &mnemonic);
    if (strlen(key) > 256 || strlen(label) > 160 || strlen(mnemonic) > 512) {
      g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "InvalidInput", "Invalid creation input"); return;
    }
    if (c->export_busy) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "Busy", "Key management is busy"); return; }
    start_store(c, inv, key, label, mnemonic, NULL, NULL); return;
  } else if (!strcmp(method, "ImportIdentity")) {
    const gchar *format, *input, *passphrase, *label;
    g_variant_get(args, "(&s&s&s&s)", &format, &input, &passphrase, &label);
    if (strlen(input) > 4096 || strlen(label) > 160 || strlen(passphrase) > 1024 ||
        (strcmp(format, "key") && strcmp(format, "mnemonic") && strcmp(format, "ncryptsec"))) {
      g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "InvalidInput", "Invalid import input"); return;
    }
    if (c->export_busy) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "Busy", "Key management is busy"); return; }
    start_store(c, inv, input, label, NULL, format, passphrase); return;
  } else if (!strcmp(method, "SelfTestIdentity")) {
    const gchar *identity; g_variant_get(args, "(&s)", &identity);
    SignerGateContext context = {.transport = SIGNER_TRANSPORT_MANAGEMENT,
      .reason = "Verify this stored identity without publishing or saving permissions",
      .management_testing = self_test_testing, .management_data = inv};
    SignerGateHandle *handle = signer_approval_service_submit(c->approval, who, sender,
        SIGNER_GATE_SELF_TEST, NULL, NULL, identity, &context, self_test_reply, g_object_ref(inv));
    signer_gate_handle_unref(handle); return;
  } else if (!strcmp(method, "CancelIdentitySelfTest")) {
    const gchar *identity; g_variant_get(args, "(&s)", &identity);
    signer_approval_service_cancel_self_test(c->approval, who->principal, identity);
  } else if (!strcmp(method, "GetIdentityMigration")) {
    const gchar *identity; g_variant_get(args, "(&s)", &identity);
    unsigned char pk[32];
    if (nostr_nip19_decode_npub(identity, pk)) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "InvalidInput", "Expected an npub"); return; }
    g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), "grotto", "migrations", identity, NULL);
    g_autofree gchar *event = NULL;
    if (!g_file_get_contents(path, &event, NULL, NULL)) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "NotFound", "No migration event"); return; }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", event)); return;
  } else if (!strcmp(method, "ExportIdentity") || !strcmp(method, "ExportRawIdentity")) {
    const gchar *identity, *format = "nsec", *passphrase = "";
    if (!strcmp(method, "ExportIdentity")) {
      g_variant_get(args, "(&s&s&s)", &identity, &format, &passphrase);
      if (!*format) format = "ncryptsec";
      /* A raw reveal can never be smuggled through the encrypted export API. */
      if (strcmp(format, "ncryptsec") && strcmp(format, "mnemonic")) {
        g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "NotSupported", "Use the separate raw-export action"); return;
      }
#ifndef GROTTO_HAVE_NIP49_EXPORT
      if (!strcmp(format, "ncryptsec")) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "NotSupported", "NIP-49 export is not built"); return; }
#endif
    } else g_variant_get(args, "(&s)", &identity);
    if (!*identity || strlen(identity) > 4096 || (!strcmp(format, "ncryptsec") && (!*passphrase || strlen(passphrase) > 1024))) {
      g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "InvalidInput", "Export requires an explicit identity and a nonempty bounded encryption passphrase"); return;
    }
    if (c->export_busy) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "Busy", "Another key export is in progress"); return; }
    start_export(c, inv, identity, format, passphrase); return;
  } else if (!strcmp(method, "RotateIdentity")) {
    const gchar *identity; g_variant_get(args, "(&s)", &identity);
    if (signer_approval_service_is_paused(c->approval)) {
      g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "Paused", "Resume signing before rotating an identity"); return;
    }
    if (!*identity || strlen(identity) > 4096) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "InvalidInput", "Choose an explicit identity"); return; }
    if (c->export_busy) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "Busy", "Key management is busy"); return; }
    start_export(c, inv, identity, "__rotate", ""); return;
  } else {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown Control1 method"); return;
  }
  g_dbus_method_invocation_return_value(inv, NULL);
}
static const GDBusInterfaceVTable vtable = {method_call, NULL, NULL, {0}};
GrottoControl *grotto_control_new(GDBusConnection *connection, SignerApprovalService *approval,
    const gchar *config_home, GError **error) {
  GrottoControl *c = g_new0(GrottoControl, 1); c->refs = 1;
  c->connection = g_object_ref(connection); c->approval = signer_approval_service_ref(approval);
  c->epoch = g_uuid_string_random();
  /* Random process epoch prevents a stale pre-restart expected_revision from
   * matching G03's process-local counter. GUI also discards snapshots on owner
   * change; this t is opaque, never an arithmetic revision supplied by clients. */
  g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, c->epoch, -1);
  digest[15] = '\0'; c->epoch_base = g_ascii_strtoull(digest, NULL, 16) << 3;
  c->requests_revision = c->clients_revision = c->status_revision = c->epoch_base;
  c->clients = grotto_clients_new(config_home, error);
  if (!c->clients) { grotto_control_free(c); return NULL; }
  c->client_healthy = TRUE; c->history = gn_event_history_new_activity(config_home);
  c->history_handler = g_signal_connect(c->history, "activity-changed", G_CALLBACK(activity_changed), c);
  c->seen_clients = grotto_clients_revision(c->clients); c->seen_permissions = signer_permissions_revision(permissions(c));
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(grotto_control_xml, error);
  if (node) c->registration = g_dbus_connection_register_object(connection, GROTTO_CONTROL_PATH,
      node->interfaces[0], &vtable, c, NULL, error);
  if (!c->registration) { grotto_control_free(c); return NULL; }
  signer_permissions_set_trust_lookup(permissions(c), grotto_clients_trust, c->clients);
  c->trust_installed = TRUE;
  c->subscription = signer_approval_service_subscribe(approval, approval_changed, c);
  signer_approval_service_set_changed(approval, approval_table_changed, c);
  return c;
}
static gboolean bunker_lookup(const gchar *client, gchar **identity, gpointer data) {
  GrottoControl *c = data;
  g_autofree gchar *principal = g_strdup_printf("nip46:%s", client);
  gchar *selected = grotto_clients_paired_identity(c->clients, principal);
  gboolean found = selected != NULL;
  if (identity) *identity = selected; else g_free(selected);
  return found;
}
static void bunker_changed(BunkerState state, const gchar *error, gpointer data) {
  (void)state; (void)error; status_changed(data);
}
void grotto_control_set_bunker(GrottoControl *c, BunkerService *bunker, gboolean desired) {
  if (!c) return;
  if (c->bunker) {
    bunker_service_set_client_lookup(c->bunker, NULL, NULL);
    bunker_service_set_state_callback(c->bunker, NULL, NULL);
  }
  c->bunker = bunker; c->desired_bunker = desired;
  if (bunker) {
    bunker_service_set_client_lookup(bunker, bunker_lookup, c);
    bunker_service_set_state_callback(bunker, bunker_changed, c);
  }
  status_changed(c);
}
void grotto_control_free(GrottoControl *c) {
  if (!c) return;
  c->closing = TRUE;
  cancel_rotation_if_paused(c);
  if (c->bunker) {
    bunker_service_set_client_lookup(c->bunker, NULL, NULL);
    bunker_service_set_state_callback(c->bunker, NULL, NULL);
  }
  if (c->subscription) signer_approval_service_unsubscribe(c->approval, c->subscription);
  if (c->registration) g_dbus_connection_unregister_object(c->connection, c->registration);
  if (c->trust_installed) {
    signer_approval_service_set_changed(c->approval, NULL, NULL);
    signer_permissions_set_trust_lookup(permissions(c), NULL, NULL);
  }
  c->subscription = c->registration = 0;
  if (c->history_handler) g_signal_handler_disconnect(c->history, c->history_handler);
  control_unref(c);
}
