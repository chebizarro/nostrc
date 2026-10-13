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
#include <nostr/nip55l/error.h>

struct _GrottoControl {
  gint refs;
  gboolean closing, export_busy, trust_installed;
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
};
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
  emit(c, "RequestsChanged", ++c->requests_revision); refresh(c); status_changed(c);
}
static guint64 permission_revision(GrottoControl *c) {
  return c->epoch_base + signer_permissions_revision(permissions(c));
}
static const gchar *bunker_state(GrottoControl *c) {
  if (!c->bunker) return c->desired_bunker ? "ERROR" : "DISABLED";
  switch (bunker_service_get_state(c->bunker)) {
    case BUNKER_STATE_STARTING: return "STARTING";
    case BUNKER_STATE_RUNNING: return "RUNNING"; /* G05 replaces with protocol-proven LISTENING. */
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
  const gchar *enabled[] = {"clients", "activity", "scoped-permissions", "trust-tiers", "pause"};
  for (guint i = 0; i < G_N_ELEMENTS(enabled); i++) g_variant_builder_add(&caps, "{sb}", enabled[i], TRUE);
  g_variant_builder_add(&caps, "{sb}", "raw-nsec-export", TRUE);
#ifdef GROTTO_HAVE_NIP49_EXPORT
  g_variant_builder_add(&caps, "{sb}", "export-identity", TRUE);
#else
  g_variant_builder_add(&caps, "{sb}", "export-identity", FALSE);
#endif
  const gchar *disabled[] = {"bunker-configuration", "pairing", "reset-bunker-endpoint", "rotate-identity", "mnemonic-export", "key-lock"};
  for (guint i = 0; i < G_N_ELEMENTS(disabled); i++) g_variant_builder_add(&caps, "{sb}", disabled[i], FALSE);
  g_variant_builder_add(&row, "{sv}", "control_version", g_variant_new_uint32(1));
  g_variant_builder_add(&row, "{sv}", "control_epoch", g_variant_new_string(c->epoch));
  g_variant_builder_add(&row, "{sv}", "capabilities", g_variant_builder_end(&caps));
  g_variant_builder_add(&row, "{sv}", "paused", g_variant_new_boolean(signer_approval_service_is_paused(c->approval)));
  g_autoptr(GVariant) pending = requests(c);
  g_variant_builder_add(&row, "{sv}", "pending_count", g_variant_new_uint32(g_variant_n_children(pending)));
  g_variant_builder_add(&row, "{sv}", "bunker_enabled", g_variant_new_boolean(c->desired_bunker));
  g_variant_builder_add(&row, "{sv}", "bunker_state", g_variant_new_string(bunker_state(c)));
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
    if (*principal && (admission || (success && known) || (!g_strcmp0(state, "DENIED") && remember))) {
      c->client_healthy = grotto_clients_record(c->clients, principal, transport, claimed, identity,
          admission, success, NULL);
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
  gint result;
} ExportJob;
static void wipe_text(gchar *s) { if (s) { secure_wipe(s, strlen(s)); g_free(s); } }
static void export_job_free(gpointer data) {
  ExportJob *job = data;
  g_object_unref(job->invocation); g_free(job->identity); g_free(job->format);
  wipe_text(job->passphrase); wipe_text(job->backup); control_unref(job->control); g_free(job);
}
static void export_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
  (void)source; (void)cancel; ExportJob *job = data;
  job->result = nostr_nip55l_export_identity(job->identity, job->format, job->passphrase, &job->backup);
  wipe_text(job->passphrase); job->passphrase = NULL;
  g_task_return_boolean(task, TRUE);
}
static void export_finished(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; (void)data; ExportJob *job = g_task_get_task_data(G_TASK(result));
  GrottoControl *c = job->control; c->export_busy = FALSE;
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
        job->result == NOSTR_SIGNER_ERROR_INVALID_ARG ? GROTTO_CONTROL_ERROR "InvalidInput" :
        job->result == NOSTR_SIGNER_ERROR_NOT_FOUND ? GROTTO_CONTROL_ERROR "NotFound" : GROTTO_CONTROL_ERROR "BackendUnavailable";
    g_dbus_method_invocation_return_dbus_error(job->invocation, name, "Identity export could not be completed"); return;
  }
  g_dbus_method_invocation_return_value(job->invocation, g_variant_new("(s)", job->backup));
  wipe_text(job->backup); job->backup = NULL;
}
static void start_export(GrottoControl *c, GDBusMethodInvocation *inv, const gchar *identity,
    const gchar *format, const gchar *passphrase) {
  ExportJob *job = g_new0(ExportJob, 1); job->control = c; g_atomic_int_inc(&c->refs);
  job->invocation = g_object_ref(inv); job->identity = g_strdup(identity);
  job->format = g_strdup(format); job->passphrase = g_strdup(passphrase); c->export_busy = TRUE;
  g_autoptr(GTask) task = g_task_new(NULL, NULL, export_finished, NULL);
  g_task_set_task_data(task, job, export_job_free); g_task_run_in_thread(task, export_worker);
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
    /* TODO G05/G06: stop, revoke all pairing, rotate transport-only custody. */
    bunker_unavailable(c, inv); return;
  } else if (!strcmp(method, "ExportIdentity") || !strcmp(method, "ExportRawIdentity")) {
    const gchar *identity, *format = "nsec", *passphrase = "";
    if (!strcmp(method, "ExportIdentity")) {
      g_variant_get(args, "(&s&s&s)", &identity, &format, &passphrase);
      if (!*format) format = "ncryptsec";
      /* A raw reveal can never be smuggled through the encrypted export API. */
      if (strcmp(format, "ncryptsec")) {
        g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "NotSupported", "Use the separate raw-export action; mnemonic custody is G09"); return;
      }
#ifndef GROTTO_HAVE_NIP49_EXPORT
      g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "NotSupported", "NIP-49 export is not built"); return;
#endif
    } else g_variant_get(args, "(&s)", &identity);
    if (!*identity || strlen(identity) > 4096 || (!strcmp(format, "ncryptsec") && (!*passphrase || strlen(passphrase) > 1024))) {
      g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "InvalidInput", "Export requires an explicit identity and a nonempty bounded encryption passphrase"); return;
    }
    if (c->export_busy) { g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "Busy", "Another key export is in progress"); return; }
    start_export(c, inv, identity, format, passphrase); return;
  } else if (!strcmp(method, "RotateIdentity")) {
    /* TODO G09 (owner-confirmed): no rotation backend/policy exists at G03. */
    g_dbus_method_invocation_return_dbus_error(inv, GROTTO_CONTROL_ERROR "NotSupported", "Identity rotation requires the G09 custody implementation"); return;
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
void grotto_control_set_bunker(GrottoControl *c, BunkerService *bunker, gboolean desired) {
  if (!c) return;
  c->bunker = bunker; c->desired_bunker = desired; status_changed(c);
}
void grotto_control_free(GrottoControl *c) {
  if (!c) return;
  c->closing = TRUE;
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
