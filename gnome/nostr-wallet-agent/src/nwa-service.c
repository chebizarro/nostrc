/* nwa-service.c - org.nostr.Wallet1 implementation
 *
 * SPDX-License-Identifier: MIT
 *
 * Every method call follows the same pipeline:
 *
 *   validate arguments (sync)  ->  identify caller from bus credentials
 *   ->  nwa_policy_decide()    ->  ALLOW: execute
 *                                  PROMPT: libadwaita dialog -> execute | Denied
 *                                  DENY: org.nostr.Wallet1.Error.*
 *
 * Scheme-handler links (nwa_service_open_uri) run the same pipeline with the
 * agent itself as caller (NWA_CALLER_SELF), which the policy always prompts
 * for, and report the outcome in a window instead of a D-Bus reply.
 *
 * GetInfoFor / GetBalanceFor / MakeInvoiceFor / PayInvoiceFor take a web
 * origin as first argument and otherwise run the plain method; once the
 * caller is identified as the trusted browser bridge
 * (nwa_caller_may_assert_origin) the origin becomes the principal for
 * policy, budget, prompt and ledger. Anyone else gets Denied.
 */
#include "nwa-service.h"
#include "nwa-bolt11.h"
#include "nwa-budget.h"
#include "nwa-caller.h"
#include "nwa-error.h"
#include "nwa-nwc.h"
#include "nwa-policy.h"
#include "nwa-ui.h"
#include "nwa-uri.h"
#include "nwa-dbus-xml.h"

#include <json-glib/json-glib.h>
#include <nostr/nip47/nwc.h>
#include <string.h>
#include <time.h>

#ifdef NWA_HAVE_LIBSECRET
#include "seahorse/secret_store.h"
#endif

#define NWA_SETTINGS_SCHEMA   "org.nostr.Wallet"
#define MAX_PROMPTS_PER_APP   2
#define MAX_PROMPTS_TOTAL     6

struct _NwaService {
  GDBusConnection *bus;
  GDBusNodeInfo   *node;
  guint            reg_id;

  NwaNwcClient    *client;
  gulong           notify_handler;
  NwaBudgetStore  *budgets;
  GSettings       *settings;
  GHashTable      *prompts;   /* prompt key -> count */
  guint            prompts_total;
  gboolean         ephemeral; /* NOSTR_WALLET_AGENT_EPHEMERAL: no keyring */
  GCancellable    *cancel;
  GStrv            origin_bridges; /* executables that may assert a web origin */
};

/* ---------------------------------------------------------------------- */
/* settings                                                                */

static guint
setting_uint(NwaService *s, const gchar *key, guint def)
{
  return s->settings ? g_settings_get_uint(s->settings, key) : def;
}

static gboolean
setting_bool(NwaService *s, const gchar *key, gboolean def)
{
  return s->settings ? g_settings_get_boolean(s->settings, key) : def;
}

static guint64
setting_u64(NwaService *s, const gchar *key, guint64 def)
{
  return s->settings ? g_settings_get_uint64(s->settings, key) : def;
}

/* Trusted (settings) apps skip budget confirmations, so the id must be one
 * the caller cannot choose: only sandbox-attested (Flatpak) identities. */
static gboolean
is_trusted(NwaService *s, const NwaCaller *c)
{
  if (!c->app_id || c->kind == NWA_CALLER_SELF || !c->attested) return FALSE;
  g_auto(GStrv) trusted = s->settings ? g_settings_get_strv(s->settings, "trusted-apps")
                                      : g_strdupv((gchar *[]){ "org.nostr.Settings", NULL });
  return g_strv_contains((const gchar *const *)trusted, c->app_id);
}

/* ---------------------------------------------------------------------- */
/* JSON <-> GVariant                                                       */

static GVariant *
json_node_to_variant_value(JsonNode *n)
{
  if (JSON_NODE_HOLDS_VALUE(n)) {
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_STRING)  return g_variant_new_string(json_node_get_string(n));
    if (t == G_TYPE_INT64)   return g_variant_new_int64(json_node_get_int(n));
    if (t == G_TYPE_BOOLEAN) return g_variant_new_boolean(json_node_get_boolean(n));
    if (t == G_TYPE_DOUBLE)  return g_variant_new_double(json_node_get_double(n));
    return NULL;
  }
  if (JSON_NODE_HOLDS_NULL(n)) return NULL;
  g_autofree gchar *j = json_to_string(n, FALSE);
  return g_variant_new_string(j);
}

/* Preimages never leave the agent except as PayInvoice's return value. */
static GVariant *
json_object_to_vardict(JsonObject *o)
{
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  if (o) {
    GList *members = json_object_get_members(o);
    for (GList *l = members; l; l = l->next) {
      const gchar *k = l->data;
      if (g_str_equal(k, "preimage")) continue;
      GVariant *v = json_node_to_variant_value(json_object_get_member(o, k));
      if (v) g_variant_builder_add(&b, "{sv}", k, v);
    }
    g_list_free(members);
  }
  return g_variant_builder_end(&b);
}

static gint64
json_int_member(JsonObject *o, const gchar *k)
{
  if (!o || !json_object_has_member(o, k)) return 0;
  JsonNode *n = json_object_get_member(o, k);
  if (!JSON_NODE_HOLDS_VALUE(n)) return 0;
  if (json_node_get_value_type(n) == G_TYPE_INT64) return json_node_get_int(n);
  if (json_node_get_value_type(n) == G_TYPE_DOUBLE) return (gint64)json_node_get_double(n);
  return 0;
}

/* ---------------------------------------------------------------------- */
/* call context                                                            */

typedef struct {
  NwaService            *svc;
  GDBusMethodInvocation *inv;      /* NULL for scheme-handler flows */
  gchar                 *method;
  GVariant              *params;
  NwaCaller             *caller;
  NwaOp                  op;
  gchar                 *prompt_key;

  gboolean               via_link; /* OpenUri / --open: always prompt */
  gchar                 *origin;   /* *For methods: web origin to act for */

  /* PayInvoice */
  gchar     *bolt11;
  NwaBolt11  inv11;
  guint64    amount_msat;
  guint64    charge_msat;   /* amount + routing-fee reserve */
  gboolean   amountless;
  gboolean   user_approved;
  guint      reservation;

  /* Pair */
  gchar     *nwc_uri;
  gchar     *pair_label;

  /* Budget */
  gchar     *target_app;
  guint64    new_limit;
} Call;

static void
call_free(Call *c)
{
  if (c->prompt_key) {
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(c->svc->prompts, c->prompt_key));
    if (n <= 1) g_hash_table_remove(c->svc->prompts, c->prompt_key);
    else g_hash_table_insert(c->svc->prompts, g_strdup(c->prompt_key), GUINT_TO_POINTER(n - 1));
    c->svc->prompts_total--;
    g_free(c->prompt_key);
  }
  g_clear_object(&c->inv);
  g_free(c->method);
  if (c->params) g_variant_unref(c->params);
  nwa_caller_free(c->caller);
  g_free(c->bolt11);
  nwa_bolt11_clear(&c->inv11);
  if (c->nwc_uri) {
    memset(c->nwc_uri, 0, strlen(c->nwc_uri));
    g_free(c->nwc_uri);
  }
  g_free(c->pair_label);
  g_free(c->target_app);
  g_free(c->origin);
  g_free(c);
}

static void
call_return(Call *c, GVariant *value, const gchar *self_success_msg)
{
  if (c->inv) {
    g_dbus_method_invocation_return_value(c->inv, value);
    c->inv = NULL; /* consumed */
  } else {
    if (value) g_variant_unref(g_variant_ref_sink(value));
    if (self_success_msg)
      nwa_ui_show_message("Nostr Wallet", self_success_msg, self_success_msg);
  }
  call_free(c);
}

static void
call_error(Call *c, GError *err)
{
  if (c->caller)
    g_message("nostr-wallet-agent: %s from %s (%s) failed: %s", c->method,
              c->caller->display_name, c->caller->app_id ? c->caller->app_id : "unidentified",
              err->message);
  else
    g_message("nostr-wallet-agent: %s rejected: %s", c->method, err->message);
  if (c->inv) {
    g_dbus_method_invocation_return_gerror(c->inv, err);
    c->inv = NULL;
  } else if (!g_error_matches(err, NWA_ERROR, NWA_ERROR_DENIED)) {
    nwa_ui_show_message("Nostr Wallet", err->message, "The request failed");
  }
  g_error_free(err);
  call_free(c);
}

#define CALL_FAIL(c, code, ...) call_error((c), g_error_new(NWA_ERROR, (code), __VA_ARGS__))

/* ---------------------------------------------------------------------- */
/* pairing / client                                                        */

static void
emit_properties_changed(NwaService *s)
{
  GVariantBuilder changed;
  g_variant_builder_init(&changed, G_VARIANT_TYPE_VARDICT);
  const gchar *wpk = s->client ? nwa_nwc_client_get_wallet_pubkey(s->client) : "";
  const gchar *lud = s->client ? nwa_nwc_client_get_lud16(s->client) : NULL;
  const gchar *const *relays = s->client ? nwa_nwc_client_get_relays(s->client) : NULL;
  const gchar *const empty[] = { NULL };
  g_variant_builder_add(&changed, "{sv}", "Paired", g_variant_new_boolean(s->client != NULL));
  g_variant_builder_add(&changed, "{sv}", "WalletPubkey", g_variant_new_string(wpk));
  g_variant_builder_add(&changed, "{sv}", "Lud16", g_variant_new_string(lud ? lud : ""));
  g_variant_builder_add(&changed, "{sv}", "Relays", g_variant_new_strv(relays ? relays : empty, -1));
  g_dbus_connection_emit_signal(s->bus, NULL, NWA_OBJECT_PATH,
                                "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                g_variant_new("(sa{sv}as)", NWA_INTERFACE, &changed, NULL), NULL);
}

static void
on_wallet_notification(NwaNwcClient *client, const gchar *type, const gchar *json, gpointer data)
{
  (void)client;
  NwaService *s = data;
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, json, -1, NULL)) return;
  JsonNode *root = json_parser_get_root(p);
  if (!JSON_NODE_HOLDS_OBJECT(root)) return;
  JsonObject *o = json_node_get_object(root);

  /* Signals reach every session-bus client: carry only what a notifier
   * needs. Private memos (description), payment hashes and preimages stay
   * behind the policy-gated LookupInvoice/ListTransactions. */
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  static const gchar *const keep[] = { "type", "amount", "fees_paid", "created_at", "settled_at", NULL };
  for (guint i = 0; keep[i]; i++) {
    if (!json_object_has_member(o, keep[i])) continue;
    GVariant *v = json_node_to_variant_value(json_object_get_member(o, keep[i]));
    if (v) g_variant_builder_add(&b, "{sv}", keep[i], v);
  }

  /* NIP-57 hook: a zap invoice's description is the kind-9734 zap request,
   * which is public anyway (it is embedded in the kind-9735 receipt). */
  const gchar *desc = json_object_get_string_member_with_default(o, "description", NULL);
  gboolean is_zap = FALSE;
  if (desc && *desc == '{') {
    g_autoptr(JsonParser) zp = json_parser_new();
    if (json_parser_load_from_data(zp, desc, -1, NULL) &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(zp)) &&
        json_int_member(json_node_get_object(json_parser_get_root(zp)), "kind") == 9734) {
      is_zap = TRUE;
      g_variant_builder_add(&b, "{sv}", "zap_request", g_variant_new_string(desc));
    }
  }
  g_variant_builder_add(&b, "{sv}", "is_zap", g_variant_new_boolean(is_zap));

  const gchar *signal = g_strcmp0(type, "payment_received") == 0 ? "PaymentReceived"
                      : g_strcmp0(type, "payment_sent") == 0     ? "PaymentSent" : NULL;
  if (!signal) {
    g_variant_builder_clear(&b);
    return;
  }
  g_dbus_connection_emit_signal(s->bus, NULL, NWA_OBJECT_PATH, NWA_INTERFACE, signal,
                                g_variant_new("(a{sv})", &b), NULL);
}

static void
install_client(NwaService *s, NwaNwcClient *client)
{
  if (s->client) {
    g_signal_handler_disconnect(s->client, s->notify_handler);
    nwa_nwc_client_stop(s->client);
    g_clear_object(&s->client);
  }
  if (client) {
    s->client = client;
    nwa_nwc_client_set_timeouts(client, setting_uint(s, "request-timeout", 60), 5000);
    s->notify_handler = g_signal_connect(client, "notification",
                                         G_CALLBACK(on_wallet_notification), s);
    nwa_nwc_client_start(client);
  }
  emit_properties_changed(s);
}

typedef enum { KR_LOOKUP, KR_SAVE, KR_DELETE } KeyringOp;

typedef struct {
  KeyringOp op;
  gchar    *uri;
  gchar    *wallet_pk;
  gchar    *client_pk;
  gchar    *relay;
  gchar    *lud16;
} KeyringJob;

static void
keyring_job_free(KeyringJob *j)
{
  if (j->uri) {
    memset(j->uri, 0, strlen(j->uri));
    g_free(j->uri);
  }
  g_free(j->wallet_pk);
  g_free(j->client_pk);
  g_free(j->relay);
  g_free(j->lud16);
  g_free(j);
}

static void
keyring_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  (void)src; (void)cancel;
  KeyringJob *j = data;
#ifdef NWA_HAVE_LIBSECRET
  GError *err = NULL;
  switch (j->op) {
    case KR_LOOKUP: {
      gchar *uri = gnostr_secret_wallet_lookup(&err);
      if (err) { g_task_return_error(task, err); return; }
      gchar *copy = g_strdup(uri);
      if (uri) secret_password_free(uri);
      g_task_return_pointer(task, copy, g_free);
      return;
    }
    case KR_SAVE: {
      g_autoptr(GDateTime) now = g_date_time_new_now_utc();
      g_autofree gchar *ts = g_date_time_format_iso8601(now);
      GnostrSecretWallet w = { j->wallet_pk, j->client_pk, j->relay, j->lud16, ts };
      if (!gnostr_secret_wallet_save(&w, j->uri, &err)) { g_task_return_error(task, err); return; }
      g_task_return_pointer(task, NULL, NULL);
      return;
    }
    case KR_DELETE:
      if (!gnostr_secret_wallet_delete_all(&err)) { g_task_return_error(task, err); return; }
      g_task_return_pointer(task, NULL, NULL);
      return;
  }
#else
  (void)j;
  g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_KEYRING,
                          "built without libsecret; pairing cannot be stored");
#endif
}

static void
keyring_run(NwaService *s, KeyringJob *j, GAsyncReadyCallback cb, gpointer data)
{
  GTask *t = g_task_new(NULL, s->cancel, cb, data);
  if (s->ephemeral) {
    /* test/lab mode: the pairing lives in memory only */
    g_task_return_pointer(t, NULL, NULL);
    keyring_job_free(j);
  } else {
    g_task_set_task_data(t, j, (GDestroyNotify)keyring_job_free);
    g_task_run_in_thread(t, keyring_thread);
  }
  g_object_unref(t);
}

static void
on_keyring_loaded(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  NwaService *s = data;
  GError *err = NULL;
  gchar *uri = g_task_propagate_pointer(G_TASK(res), &err);
  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_message("nostr-wallet-agent: could not read the wallet pairing from the keyring: %s",
                err->message);
    g_error_free(err);
    return;
  }
  if (!uri) {
    g_message("nostr-wallet-agent: no wallet paired");
    return;
  }
  NwaNwcClient *client = nwa_nwc_client_new(uri, NULL, NULL, &err);
  memset(uri, 0, strlen(uri));
  g_free(uri);
  if (!client) {
    g_warning("nostr-wallet-agent: stored pairing is invalid: %s", err->message);
    g_error_free(err);
    return;
  }
  g_message("nostr-wallet-agent: loaded pairing for wallet %.16s…",
            nwa_nwc_client_get_wallet_pubkey(client));
  install_client(s, client);
}

/* ---------------------------------------------------------------------- */
/* execution                                                               */

static void
on_get_info_reply(GObject *src, GAsyncResult *res, gpointer data)
{
  Call *c = data;
  GError *err = NULL;
  g_autoptr(JsonNode) r = res ? nwa_nwc_client_request_finish(NWA_NWC_CLIENT(src), res, &err) : NULL;
  NwaNwcClient *cl = c->svc->client;

  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  if (r && JSON_NODE_HOLDS_OBJECT(r)) {
    GVariant *w = json_object_to_vardict(json_node_get_object(r));
    GVariantIter it;
    const gchar *k;
    GVariant *v;
    g_variant_iter_init(&it, w);
    while (g_variant_iter_next(&it, "{&sv}", &k, &v)) {
      if (!g_str_equal(k, "methods")) g_variant_builder_add(&b, "{sv}", k, v);
      g_variant_unref(v);
    }
    g_variant_unref(g_variant_ref_sink(w));
  } else if (err) {
    g_variant_builder_add(&b, "{sv}", "wallet_error", g_variant_new_string(err->message));
  }
  g_clear_error(&err);

  g_variant_builder_add(&b, "{sv}", "paired", g_variant_new_boolean(cl != NULL));
  if (cl) {
    const gchar *const empty[] = { NULL };
    const gchar *const *methods = nwa_nwc_client_get_methods(cl);
    g_variant_builder_add(&b, "{sv}", "wallet_pubkey",
                          g_variant_new_string(nwa_nwc_client_get_wallet_pubkey(cl)));
    g_variant_builder_add(&b, "{sv}", "client_pubkey",
                          g_variant_new_string(nwa_nwc_client_get_client_pubkey(cl)));
    const gchar *lud = nwa_nwc_client_get_lud16(cl);
    g_variant_builder_add(&b, "{sv}", "lud16", g_variant_new_string(lud ? lud : ""));
    g_variant_builder_add(&b, "{sv}", "relays",
                          g_variant_new_strv(nwa_nwc_client_get_relays(cl), -1));
    g_variant_builder_add(&b, "{sv}", "encryption",
                          g_variant_new_string(nwa_nwc_client_get_encryption(cl)));
    g_variant_builder_add(&b, "{sv}", "methods",
                          g_variant_new_strv(methods ? methods : empty, -1));
  }
  call_return(c, g_variant_new("(a{sv})", &b), NULL);
}

static void
on_balance_reply(GObject *src, GAsyncResult *res, gpointer data)
{
  Call *c = data;
  GError *err = NULL;
  g_autoptr(JsonNode) r = nwa_nwc_client_request_finish(NWA_NWC_CLIENT(src), res, &err);
  if (!r) { call_error(c, err); return; }
  gint64 bal = JSON_NODE_HOLDS_OBJECT(r) ? json_int_member(json_node_get_object(r), "balance") : 0;
  call_return(c, g_variant_new("(t)", (guint64)MAX(bal, 0)), NULL);
}

static void
on_make_invoice_reply(GObject *src, GAsyncResult *res, gpointer data)
{
  Call *c = data;
  GError *err = NULL;
  g_autoptr(JsonNode) r = nwa_nwc_client_request_finish(NWA_NWC_CLIENT(src), res, &err);
  if (!r) { call_error(c, err); return; }
  JsonObject *o = JSON_NODE_HOLDS_OBJECT(r) ? json_node_get_object(r) : NULL;
  const gchar *inv = o ? json_object_get_string_member_with_default(o, "invoice", NULL) : NULL;
  const gchar *hash = o ? json_object_get_string_member_with_default(o, "payment_hash", "") : "";
  if (!inv) {
    CALL_FAIL(c, NWA_ERROR_WALLET, "wallet returned no invoice");
    return;
  }
  call_return(c, g_variant_new("(ss)", inv, hash ? hash : ""), NULL);
}

static void
on_lookup_reply(GObject *src, GAsyncResult *res, gpointer data)
{
  Call *c = data;
  GError *err = NULL;
  g_autoptr(JsonNode) r = nwa_nwc_client_request_finish(NWA_NWC_CLIENT(src), res, &err);
  if (!r) { call_error(c, err); return; }
  JsonObject *o = JSON_NODE_HOLDS_OBJECT(r) ? json_node_get_object(r) : NULL;
  GVariant *d = json_object_to_vardict(o);
  call_return(c, g_variant_new_tuple(&d, 1), NULL);
}

static void
on_list_reply(GObject *src, GAsyncResult *res, gpointer data)
{
  Call *c = data;
  GError *err = NULL;
  g_autoptr(JsonNode) r = nwa_nwc_client_request_finish(NWA_NWC_CLIENT(src), res, &err);
  if (!r) { call_error(c, err); return; }
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));
  JsonObject *o = JSON_NODE_HOLDS_OBJECT(r) ? json_node_get_object(r) : NULL;
  JsonArray *txs = (o && json_object_has_member(o, "transactions") &&
                    JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, "transactions")))
                   ? json_object_get_array_member(o, "transactions") : NULL;
  for (guint i = 0; txs && i < json_array_get_length(txs); i++) {
    JsonNode *n = json_array_get_element(txs, i);
    if (JSON_NODE_HOLDS_OBJECT(n))
      g_variant_builder_add_value(&b, json_object_to_vardict(json_node_get_object(n)));
  }
  call_return(c, g_variant_new("(aa{sv})", &b), NULL);
}

static void
on_pay_reply(GObject *src, GAsyncResult *res, gpointer data)
{
  Call *c = data;
  NwaService *s = c->svc;
  GError *err = NULL;
  g_autoptr(JsonNode) r = nwa_nwc_client_request_finish(NWA_NWC_CLIENT(src), res, &err);
  const gchar *app = c->caller->app_id;
  if (!r) {
    if (nwa_nwc_error_is_definite(err))
      nwa_budget_store_release(s->budgets, c->reservation);
    else
      nwa_budget_store_commit(s->budgets, c->reservation, c->charge_msat); /* unknown: count it */
    c->reservation = 0;
    call_error(c, err);
    return;
  }
  JsonObject *o = JSON_NODE_HOLDS_OBJECT(r) ? json_node_get_object(r) : NULL;
  const gchar *preimage = o ? json_object_get_string_member_with_default(o, "preimage", "") : "";
  gint64 fees = MAX(json_int_member(o, "fees_paid"), 0);
  nwa_budget_store_commit(s->budgets, c->reservation, c->amount_msat + (guint64)fees);
  c->reservation = 0;

  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("outgoing"));
  g_variant_builder_add(&b, "{sv}", "amount", g_variant_new_int64((gint64)c->amount_msat));
  g_variant_builder_add(&b, "{sv}", "fees_paid", g_variant_new_int64(fees));
  g_variant_builder_add(&b, "{sv}", "app_id", g_variant_new_string(app ? app : ""));
  g_dbus_connection_emit_signal(s->bus, NULL, NWA_OBJECT_PATH, NWA_INTERFACE, "PaymentSent",
                                g_variant_new("(a{sv})", &b), NULL);

  g_autofree gchar *amt = nwa_ui_format_msat(c->amount_msat);
  g_autofree gchar *msg = g_strdup_printf("Paid %s", amt);
  call_return(c, g_variant_new("(st)", preimage ? preimage : "", (guint64)fees), msg);
}

static void run_policy(Call *c);

static void
on_keyring_saved(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  Call *c = data;
  GError *err = NULL;
  g_task_propagate_pointer(G_TASK(res), &err);
  if (err) {
    CALL_FAIL(c, NWA_ERROR_KEYRING, "could not store the pairing in the keyring: %s", err->message);
    g_error_free(err);
    return;
  }
  GError *cerr = NULL;
  NwaNwcClient *client = nwa_nwc_client_new(c->nwc_uri, NULL, NULL, &cerr);
  if (!client) { call_error(c, cerr); return; }
  install_client(c->svc, client);
  g_autofree gchar *msg = g_strdup_printf("Connected to %s", c->pair_label);
  call_return(c, NULL, msg);
}

static void
on_keyring_deleted(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  Call *c = data;
  GError *err = NULL;
  g_task_propagate_pointer(G_TASK(res), &err);
  if (err) {
    CALL_FAIL(c, NWA_ERROR_KEYRING, "could not delete the pairing from the keyring: %s", err->message);
    g_error_free(err);
    return;
  }
  install_client(c->svc, NULL);
  call_return(c, NULL, "Wallet disconnected");
}

static void
execute(Call *c)
{
  NwaService *s = c->svc;
  const gchar *m = c->method;

  if (g_str_equal(m, "GetInfo")) {
    if (!s->client || !nwa_nwc_client_supports(s->client, "get_info")) {
      on_get_info_reply(NULL, NULL, c);
      return;
    }
    nwa_nwc_client_request_async(s->client, "get_info", NULL, NULL, on_get_info_reply, c);
  } else if (g_str_equal(m, "GetBalance")) {
    nwa_nwc_client_request_async(s->client, "get_balance", NULL, NULL, on_balance_reply, c);
  } else if (g_str_equal(m, "MakeInvoice")) {
    guint32 amount, expiry;
    const gchar *desc;
    g_variant_get(c->params, "(u&su)", &amount, &desc, &expiry);
    g_autoptr(JsonObject) p = json_object_new();
    json_object_set_int_member(p, "amount", amount);
    if (*desc) json_object_set_string_member(p, "description", desc);
    if (expiry) json_object_set_int_member(p, "expiry", expiry);
    nwa_nwc_client_request_async(s->client, "make_invoice", p, NULL, on_make_invoice_reply, c);
  } else if (g_str_equal(m, "LookupInvoice")) {
    const gchar *q;
    g_variant_get(c->params, "(&s)", &q);
    g_autoptr(JsonObject) p = json_object_new();
    json_object_set_string_member(p, g_ascii_strncasecmp(q, "ln", 2) == 0 ? "invoice" : "payment_hash", q);
    nwa_nwc_client_request_async(s->client, "lookup_invoice", p, NULL, on_lookup_reply, c);
  } else if (g_str_equal(m, "ListTransactions")) {
    guint64 from, until;
    guint32 limit;
    const gchar *type;
    g_variant_get(c->params, "(ttu&s)", &from, &until, &limit, &type);
    g_autoptr(JsonObject) p = json_object_new();
    if (from) json_object_set_int_member(p, "from", (gint64)MIN(from, (guint64)G_MAXINT64));
    if (until) json_object_set_int_member(p, "until", (gint64)MIN(until, (guint64)G_MAXINT64));
    if (limit) json_object_set_int_member(p, "limit", limit);
    if (*type) json_object_set_string_member(p, "type", type);
    nwa_nwc_client_request_async(s->client, "list_transactions", p, NULL, on_list_reply, c);
  } else if (g_str_equal(m, "PayInvoice")) {
    /* Link payments are ledgered under the agent's own id, never against
     * the opener's automatic budget. */
    const gchar *app = c->via_link ? "org.nostr.Wallet" : c->caller->app_id;
    /* Auto-approved payments must still fit at reservation time (a
     * concurrent call may have used the budget since the policy ran). */
    c->reservation = nwa_budget_store_reserve(s->budgets, app ? app : "(unidentified)",
                                              c->charge_msat, c->user_approved);
    if (!c->reservation) {
      if (!c->user_approved) {
        /* lost a race for the remaining budget since the policy ran */
        NwaBudgetInfo bi;
        nwa_budget_store_get(s->budgets, app, &bi);
        g_dbus_connection_emit_signal(s->bus, NULL, NWA_OBJECT_PATH, NWA_INTERFACE, "BudgetExceeded",
                                      g_variant_new("(stt)", app ? app : "", c->charge_msat,
                                                    bi.remaining_msat), NULL);
        CALL_FAIL(c, NWA_ERROR_BUDGET_EXCEEDED, "payment no longer fits the daily budget; retry to be asked");
        return;
      }
      CALL_FAIL(c, NWA_ERROR_FAILED, "could not record the payment");
      return;
    }
    g_autoptr(JsonObject) p = json_object_new();
    json_object_set_string_member(p, "invoice", c->bolt11);
    if (c->amountless)
      json_object_set_int_member(p, "amount", (gint64)c->amount_msat);
    nwa_nwc_client_request_async(s->client, "pay_invoice", p, NULL, on_pay_reply, c);
  } else if (g_str_equal(m, "Pair")) {
    NostrNwcConnection conn = { 0 };
    if (nostr_nwc_uri_parse(c->nwc_uri, &conn) != 0) {
      CALL_FAIL(c, NWA_ERROR_INVALID_ARGS, "invalid nostr+walletconnect URI");
      return;
    }
    KeyringJob *j = g_new0(KeyringJob, 1);
    j->op = KR_SAVE;
    j->uri = g_strdup(c->nwc_uri);
    j->wallet_pk = g_strdup(conn.wallet_pubkey_hex);
    j->relay = g_strdup(conn.relays ? conn.relays[0] : NULL);
    j->lud16 = g_strdup(conn.lud16);
    GError *e = NULL;
    NwaNwcClient *probe = nwa_nwc_client_new(c->nwc_uri, NULL, NULL, &e);
    if (probe) {
      j->client_pk = g_strdup(nwa_nwc_client_get_client_pubkey(probe));
      g_object_unref(probe);
    }
    g_clear_error(&e);
    nostr_nwc_connection_clear(&conn);
    keyring_run(s, j, on_keyring_saved, c);
  } else if (g_str_equal(m, "Unpair")) {
    if (!s->client) {
      call_return(c, NULL, NULL);
      return;
    }
    KeyringJob *j = g_new0(KeyringJob, 1);
    j->op = KR_DELETE;
    keyring_run(s, j, on_keyring_deleted, c);
  } else if (g_str_equal(m, "GetBudget")) {
    NwaBudgetInfo bi;
    nwa_budget_store_get(s->budgets, c->target_app, &bi);
    call_return(c, g_variant_new("(ut)", (guint32)MIN(bi.limit_msat_per_day, (guint64)G_MAXUINT32),
                                 bi.spent_today_msat), NULL);
  } else if (g_str_equal(m, "SetBudget")) {
    /* A spending limit is not a read grant (balance/history are asked for
     * separately). */
    nwa_budget_store_set_limit(s->budgets, c->target_app, c->new_limit);
    call_return(c, NULL, NULL);
  } else {
    CALL_FAIL(c, NWA_ERROR_FAILED, "unknown method %s", m);
  }
}

/* ---------------------------------------------------------------------- */
/* prompts                                                                 */

static gboolean
prompt_admit(Call *c)
{
  NwaService *s = c->svc;
  const gchar *key = c->caller->app_id ? c->caller->app_id : c->caller->sender;
  guint n = GPOINTER_TO_UINT(g_hash_table_lookup(s->prompts, key));
  if (n >= MAX_PROMPTS_PER_APP || s->prompts_total >= MAX_PROMPTS_TOTAL)
    return FALSE;
  g_hash_table_insert(s->prompts, g_strdup(key), GUINT_TO_POINTER(n + 1));
  s->prompts_total++;
  c->prompt_key = g_strdup(key);
  return TRUE;
}

static void
on_payment_answer(gboolean approved, gboolean remember, guint64 limit_msat, gpointer data)
{
  Call *c = data;
  if (!approved) {
    CALL_FAIL(c, NWA_ERROR_DENIED, "payment declined");
    return;
  }
  /* "Always allow up to N/day" persists exactly that limit, nothing else. */
  if (remember && c->caller->app_id && c->caller->kind != NWA_CALLER_SELF && !c->via_link)
    nwa_budget_store_set_limit(c->svc->budgets, c->caller->app_id, limit_msat);
  c->user_approved = TRUE;
  execute(c);
}

static void
on_confirm_answer(gboolean accepted, gboolean remember, gpointer data)
{
  Call *c = data;
  if (!accepted) {
    CALL_FAIL(c, NWA_ERROR_DENIED, "request declined");
    return;
  }
  if (remember && (c->op == NWA_OP_READ || c->op == NWA_OP_RECEIVE) &&
      c->caller->app_id && c->caller->kind != NWA_CALLER_SELF && !c->via_link)
    nwa_budget_store_set_allow_read(c->svc->budgets, c->caller->app_id, TRUE);
  c->user_approved = TRUE;
  execute(c);
}

static gchar *
caller_phrase(const NwaCaller *c, gboolean via_link)
{
  if (c->kind == NWA_CALLER_SELF) return g_strdup("A link opened on this computer");
  if (c->kind == NWA_CALLER_WEB_ORIGIN)
    return c->via ? g_strdup_printf("The website %s (in %s, via the Nostr browser extension)", c->app_id, c->via)
                  : g_strdup_printf("The website %s (via the Nostr browser extension)", c->app_id);
  if (via_link)
    return c->app_id ? g_strdup_printf("A link opened by %s (%s%s)", c->display_name, c->app_id,
                                       c->attested ? "" : ", unverified")
                     : g_strdup("A link opened by an unidentified application");
  if (!c->app_id) return g_strdup("An unidentified application");
  return g_strdup_printf("%s (%s%s)", c->display_name, c->app_id,
                         c->attested ? "" : ", unverified");
}

static void
show_prompt(Call *c, gboolean over_budget)
{
  NwaService *s = c->svc;
  guint timeout = setting_uint(s, "approval-timeout", 120);
  g_autofree gchar *who = caller_phrase(c->caller, c->via_link);

  if (c->op == NWA_OP_PAY) {
    NwaBudgetInfo bi;
    nwa_budget_store_get(s->budgets, c->caller->app_id, &bi);
    NwaPaymentPrompt p = {
      .app_name = c->caller->display_name,
      .app_id = c->caller->app_id,
      .app_kind = nwa_caller_kind_to_string(c->caller->kind),
      .app_attested = c->caller->attested,
      .can_remember = c->caller->app_id != NULL && c->caller->kind != NWA_CALLER_SELF && !c->via_link,
      .is_site = c->caller->kind == NWA_CALLER_WEB_ORIGIN,
      .via = c->caller->via,
      .via_link = c->via_link,
      .amount_msat = c->amount_msat,
      .description = c->inv11.description,
      .payee = c->inv11.payee,
      .network = c->inv11.network,
      .limit_msat = bi.limit_msat_per_day,
      .remaining_msat = bi.remaining_msat,
      .over_budget = over_budget,
    };
    nwa_ui_prompt_payment(&p, timeout, on_payment_answer, c);
    return;
  }

  g_autofree gchar *body = NULL;
  const gchar *title = "Wallet Access";
  const gchar *accept = "_Allow";
  const gchar *remember = NULL;
  gboolean destructive = FALSE;
  switch (c->op) {
    case NWA_OP_READ:
    case NWA_OP_RECEIVE:
      body = g_strdup_printf("%s wants to use your Lightning wallet.\n\nIt will be able to see "
                             "your balance and transaction history and create invoices. It "
                             "cannot send payments without your approval.", who);
      if (c->caller->app_id && !c->via_link)
        remember = c->caller->kind == NWA_CALLER_WEB_ORIGIN ? "Always allow this site"
                                                            : "Always allow this app";
      break;
    case NWA_OP_PAIR: {
      g_autofree gchar *cur = NULL;
      if (s->client) {
        const gchar *lud = nwa_nwc_client_get_lud16(s->client);
        cur = g_strdup_printf("\n\nThis replaces the wallet currently connected (%s).",
                              lud ? lud : nwa_nwc_client_get_wallet_pubkey(s->client));
      }
      title = "Connect Wallet";
      accept = "_Connect";
      body = g_strdup_printf("%s wants to connect this computer to the wallet %s.\n\nEvery "
                             "application will pay from, and create invoices into, this "
                             "wallet. Whoever created this connection link also holds its "
                             "secret and can use the wallet directly — only accept links "
                             "from your own wallet.%s", who, c->pair_label, cur ? cur : "");
      break;
    }
    case NWA_OP_UNPAIR:
      title = "Disconnect Wallet";
      accept = "_Disconnect";
      destructive = TRUE;
      body = g_strdup_printf("%s wants to disconnect your Lightning wallet. Applications "
                             "will no longer be able to pay or receive.", who);
      break;
    case NWA_OP_BUDGET_CHANGE: {
      g_autofree gchar *amt = nwa_ui_format_msat(c->new_limit);
      title = "Payment Budget";
      if (c->caller->app_id && g_strcmp0(c->caller->app_id, c->target_app) == 0)
        body = g_strdup_printf("%s wants to pay up to %s per day without asking you.", who, amt);
      else
        body = g_strdup_printf("%s wants to let %s pay up to %s per day without asking you.",
                               who, c->target_app, amt);
      break;
    }
    default:
      CALL_FAIL(c, NWA_ERROR_DENIED, "request not allowed");
      return;
  }
  nwa_ui_confirm(title, body, accept, destructive, remember, timeout, on_confirm_answer, c);
}

static void
run_policy(Call *c)
{
  NwaService *s = c->svc;
  NwaBudgetInfo bi;
  nwa_budget_store_get(s->budgets, c->caller->app_id, &bi);

  NwaPolicyInput in = {
    .op = c->op,
    .same_uid = c->caller->same_uid,
    .caller_identified = c->caller->app_id != NULL,
    .caller_trusted = is_trusted(s, c->caller),
    .is_self = c->caller->kind == NWA_CALLER_SELF || c->via_link,
    .paired = s->client != NULL,
    .ui_available = nwa_ui_available(),
    .always_confirm = setting_bool(s, "always-confirm-payments", FALSE),
    .allow_read = bi.allow_read,
    .amount_msat = c->charge_msat,
    .limit_msat = bi.limit_msat_per_day,
    .remaining_msat = bi.remaining_msat,
    .max_auto_pay_msat = setting_u64(s, "max-auto-pay-msat", 0),
  };
  NwaDecision d = nwa_policy_decide(&in);

  if (c->op == NWA_OP_PAY && d.over_budget) {
    g_dbus_connection_emit_signal(s->bus, NULL, NWA_OBJECT_PATH, NWA_INTERFACE, "BudgetExceeded",
                                  g_variant_new("(stt)", c->caller->app_id ? c->caller->app_id : "",
                                                c->charge_msat, bi.remaining_msat), NULL);
  }

  switch (d.kind) {
    case NWA_DECISION_ALLOW:
      execute(c);
      return;
    case NWA_DECISION_PROMPT:
      if (!prompt_admit(c)) {
        CALL_FAIL(c, NWA_ERROR_RATE_LIMITED, "too many pending approval requests");
        return;
      }
      show_prompt(c, d.over_budget);
      return;
    case NWA_DECISION_DENY: {
      NwaError code = NWA_ERROR_DENIED;
      if (d.reason == NWA_DENY_NOT_PAIRED) code = NWA_ERROR_NOT_PAIRED;
      else if (d.reason == NWA_DENY_INVALID) code = NWA_ERROR_INVALID_ARGS;
      else if (d.reason == NWA_DENY_NO_UI && d.over_budget) code = NWA_ERROR_BUDGET_EXCEEDED;
      CALL_FAIL(c, code, "%s", nwa_policy_deny_reason_to_string(d.reason));
      return;
    }
  }
}

/* ---------------------------------------------------------------------- */
/* argument validation (before identifying the caller)                     */

static gboolean
prepare_pay(Call *c, const gchar *bolt11, guint64 amount_arg, GError **error)
{
  c->bolt11 = g_ascii_strdown(bolt11, -1);
  if (!nwa_bolt11_decode(c->bolt11, &c->inv11, error))
    return FALSE;
  if (nwa_bolt11_is_expired(&c->inv11, (gint64)time(NULL))) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invoice has expired");
    return FALSE;
  }
  if (c->inv11.amount_msat == 0) {
    if (amount_arg == 0) {
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                          "amount-less invoice: pass amount_msat");
      return FALSE;
    }
    c->amountless = TRUE;
    c->amount_msat = amount_arg;
  } else {
    if (amount_arg != 0 && amount_arg != c->inv11.amount_msat) {
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                          "amount_msat does not match the invoice amount");
      return FALSE;
    }
    c->amount_msat = c->inv11.amount_msat;
  }
  guint64 fee = nwa_budget_fee_reserve(c->amount_msat);
  c->charge_msat = c->amount_msat > G_MAXUINT64 - fee ? G_MAXUINT64 : c->amount_msat + fee;
  c->op = NWA_OP_PAY;
  return TRUE;
}

static gboolean
prepare_pair(Call *c, const gchar *uri, GError **error)
{
  c->nwc_uri = nwa_uri_normalize_nwc(uri, error);
  if (!c->nwc_uri) return FALSE;
  NostrNwcConnection conn = { 0 };
  if (nostr_nwc_uri_parse(c->nwc_uri, &conn) == 0) {
    c->pair_label = conn.lud16 && *conn.lud16
      ? g_strdup_printf("%s via %s", conn.lud16, conn.relays[0])
      : g_strdup_printf("%.16s… via %s", conn.wallet_pubkey_hex, conn.relays[0]);
    nostr_nwc_connection_clear(&conn);
  }
  c->op = NWA_OP_PAIR;
  return TRUE;
}

/* Budget ops need the caller's identity; finalize the op there. */
static gboolean
resolve_budget_op(Call *c, GError **error)
{
  const gchar *own = c->caller->app_id;
  if (!c->target_app || !*c->target_app) {
    if (!own) {
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_DENIED,
                          "the calling application could not be identified");
      return FALSE;
    }
    g_free(c->target_app);
    c->target_app = g_strdup(own);
  }
  gboolean is_own = own && g_str_equal(own, c->target_app);
  if (g_str_equal(c->method, "GetBudget")) {
    c->op = is_own ? NWA_OP_BUDGET_LOWER_OWN /* read own: always allowed */ : NWA_OP_BUDGET_QUERY_OTHER;
  } else {
    NwaBudgetInfo bi;
    nwa_budget_store_get(c->svc->budgets, c->target_app, &bi);
    c->op = (is_own && c->new_limit <= bi.limit_msat_per_day) ? NWA_OP_BUDGET_LOWER_OWN
                                                              : NWA_OP_BUDGET_CHANGE;
  }
  return TRUE;
}

static void
on_caller(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  Call *c = data;
  GError *err = NULL;
  NwaCaller *caller = nwa_caller_identify_finish(res, &err);
  if (!caller) { call_error(c, err); return; }
  c->caller = caller;
  g_debug("nostr-wallet-agent: %s from %s app=%s kind=%s pid=%u exe=%s", c->method, caller->sender,
          caller->app_id ? caller->app_id : "(none)", nwa_caller_kind_to_string(caller->kind),
          caller->pid, caller->exe ? caller->exe : "(none)");
  if (c->origin) {
    if (!nwa_caller_may_assert_origin(caller, (const gchar *const *)c->svc->origin_bridges)) {
      CALL_FAIL(c, NWA_ERROR_DENIED, "only the browser bridge may act for a web origin");
      return;
    }
    c->caller = nwa_caller_for_origin(caller, c->origin);
    nwa_caller_free(caller);
    if (g_str_equal(c->method, "GetInfo") && !c->svc->client) {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&b, "{sv}", "paired", g_variant_new_boolean(FALSE));
      call_return(c, g_variant_new("(a{sv})", &b), NULL);
      return;
    }
  }
  if (c->target_app || g_str_equal(c->method, "GetBudget") || g_str_equal(c->method, "SetBudget")) {
    if (!resolve_budget_op(c, &err)) { call_error(c, err); return; }
  }
  run_policy(c);
}

/* "<Method>For(s origin, <Method args>…)" -> method "<Method>" with the
 * origin stripped off. D-Bus has already checked the signature. */
static gboolean
unwrap_origin_call(Call *c, GError **error)
{
  static const gchar *const wrapped[] = { "GetInfoFor", "GetBalanceFor", "MakeInvoiceFor",
                                          "PayInvoiceFor", NULL };
  if (!g_strv_contains(wrapped, c->method)) return TRUE;
  g_autoptr(GVariant) first = g_variant_get_child_value(c->params, 0);
  const gchar *origin = g_variant_get_string(first, NULL);
  if (!nwa_caller_is_web_origin(origin)) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                        "origin must be a serialized secure web origin (https://host[:port])");
    return FALSE;
  }
  gsize n = g_variant_n_children(c->params);
  GVariant **rest = g_new0(GVariant *, n);
  for (gsize i = 1; i < n; i++) rest[i - 1] = g_variant_get_child_value(c->params, i);
  GVariant *inner = g_variant_ref_sink(g_variant_new_tuple(rest, n - 1));
  for (gsize i = 0; i + 1 < n; i++) g_variant_unref(rest[i]);
  g_free(rest);
  g_variant_unref(c->params);
  c->params = inner;
  c->origin = g_strdup(origin);
  c->method[strlen(c->method) - strlen("For")] = '\0';
  return TRUE;
}

typedef struct {
  NwaService *svc;
  gchar      *uri;
} OpenUriJob;

static void on_open_uri_caller(GObject *src, GAsyncResult *res, gpointer data);

static void
handle_method_call(GDBusConnection *bus, const gchar *sender, const gchar *path,
                   const gchar *iface, const gchar *method, GVariant *params,
                   GDBusMethodInvocation *inv, gpointer data)
{
  (void)bus; (void)path; (void)iface;
  NwaService *s = data;
  Call *c = g_new0(Call, 1);
  c->svc = s;
  c->inv = g_object_ref(inv);
  c->method = g_strdup(method);
  c->params = g_variant_ref(params);
  GError *err = NULL;
  gboolean ok = TRUE;

  if (!unwrap_origin_call(c, &err)) {
    call_error(c, err);
    return;
  }
  method = c->method;
  params = c->params;

  if (g_str_equal(method, "GetInfo")) {
    if (!s->client && !c->origin) { /* *For: the caller is checked first */
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&b, "{sv}", "paired", g_variant_new_boolean(FALSE));
      call_return(c, g_variant_new("(a{sv})", &b), NULL);
      return;
    }
    c->op = NWA_OP_READ;
  } else if (g_str_equal(method, "GetBalance") || g_str_equal(method, "LookupInvoice") ||
             g_str_equal(method, "ListTransactions")) {
    c->op = NWA_OP_READ;
    if (g_str_equal(method, "ListTransactions")) {
      const gchar *type;
      g_variant_get(params, "(ttu&s)", NULL, NULL, NULL, &type);
      if (*type && !g_str_equal(type, "incoming") && !g_str_equal(type, "outgoing")) {
        g_set_error_literal(&err, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                            "type must be \"incoming\", \"outgoing\" or \"\"");
        ok = FALSE;
      }
    } else if (g_str_equal(method, "LookupInvoice")) {
      const gchar *q;
      g_variant_get(params, "(&s)", &q);
      gboolean hex64 = strlen(q) == 64;
      for (const gchar *p = q; hex64 && *p; p++) hex64 = g_ascii_isxdigit(*p);
      if (!hex64 && g_ascii_strncasecmp(q, "ln", 2) != 0) {
        g_set_error_literal(&err, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                            "expected a 64-hex payment hash or a BOLT-11 invoice");
        ok = FALSE;
      }
    }
  } else if (g_str_equal(method, "MakeInvoice")) {
    guint32 amount;
    g_variant_get(params, "(u&su)", &amount, NULL, NULL);
    c->op = NWA_OP_RECEIVE;
    if (amount == 0) {
      g_set_error_literal(&err, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "amount_msat must be > 0");
      ok = FALSE;
    }
  } else if (g_str_equal(method, "PayInvoice")) {
    const gchar *bolt11;
    guint32 amount;
    g_variant_get(params, "(&su)", &bolt11, &amount);
    ok = prepare_pay(c, bolt11, amount, &err);
  } else if (g_str_equal(method, "Pair")) {
    const gchar *uri;
    g_variant_get(params, "(&s)", &uri);
    ok = prepare_pair(c, uri, &err);
  } else if (g_str_equal(method, "Unpair")) {
    c->op = NWA_OP_UNPAIR;
  } else if (g_str_equal(method, "OpenUri")) {
    /* Answer now; the link flow reports to the user, not the caller. */
    const gchar *uri;
    g_variant_get(params, "(&s)", &uri);
    OpenUriJob *j = g_new0(OpenUriJob, 1);
    j->svc = s;
    j->uri = g_strdup(uri);
    call_return(c, NULL, NULL);
    nwa_caller_identify_async(s->bus, sender, s->cancel, on_open_uri_caller, j);
    return;
  } else if (g_str_equal(method, "GetBudget")) {
    const gchar *app;
    g_variant_get(params, "(&s)", &app);
    c->target_app = g_strdup(app);
  } else if (g_str_equal(method, "SetBudget")) {
    const gchar *app;
    guint32 limit;
    g_variant_get(params, "(&su)", &app, &limit);
    c->target_app = g_strdup(app);
    c->new_limit = limit;
  } else {
    g_set_error(&err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method);
    ok = FALSE;
  }
  if (!ok) {
    call_error(c, err);
    return;
  }

  /* Identity is re-read from the bus for every call (no cache): a process
   * may exec() a different binary while keeping its connection. */
  nwa_caller_identify_async(s->bus, sender, s->cancel, on_caller, c);
}

static GVariant *
handle_get_property(GDBusConnection *bus, const gchar *sender, const gchar *path,
                    const gchar *iface, const gchar *prop, GError **error, gpointer data)
{
  (void)bus; (void)sender; (void)path; (void)iface; (void)error;
  NwaService *s = data;
  if (g_str_equal(prop, "Paired"))
    return g_variant_new_boolean(s->client != NULL);
  if (g_str_equal(prop, "WalletPubkey"))
    return g_variant_new_string(s->client ? nwa_nwc_client_get_wallet_pubkey(s->client) : "");
  if (g_str_equal(prop, "Lud16")) {
    const gchar *l = s->client ? nwa_nwc_client_get_lud16(s->client) : NULL;
    return g_variant_new_string(l ? l : "");
  }
  if (g_str_equal(prop, "Relays")) {
    const gchar *const empty[] = { NULL };
    return g_variant_new_strv(s->client ? nwa_nwc_client_get_relays(s->client) : empty, -1);
  }
  return NULL;
}

static const GDBusInterfaceVTable vtable = {
  .method_call = handle_method_call,
  .get_property = handle_get_property,
};

/* ---------------------------------------------------------------------- */
/* scheme handler                                                          */

/* @caller: (transfer full). */
static void
open_uri_dispatch(NwaService *s, NwaCaller *caller, const gchar *uri, gboolean via_link)
{
  NwaUri u;
  GError *err = NULL;
  if (!caller->same_uid) {
    nwa_caller_free(caller);
    return;
  }
  if (!nwa_uri_parse(uri, &u, &err)) {
    nwa_ui_show_message("Cannot open link", err->message, "This link cannot be paid");
    g_error_free(err);
    nwa_caller_free(caller);
    return;
  }
  Call *c = g_new0(Call, 1);
  c->svc = s;
  c->caller = caller;
  c->via_link = via_link;

  switch (u.kind) {
    case NWA_URI_LIGHTNING_INVOICE:
      c->method = g_strdup("PayInvoice");
      if (!prepare_pay(c, u.bolt11, u.amount_msat, &err)) {
        call_error(c, err);
        break;
      }
      run_policy(c);
      break;
    case NWA_URI_NWC_PAIRING:
      c->method = g_strdup("Pair");
      if (!prepare_pair(c, u.nwc_uri, &err)) {
        call_error(c, err);
        break;
      }
      run_policy(c);
      break;
    case NWA_URI_LNURL:
      /* TODO(nostrc-prqu.8): LNURL-pay / lightning addresses */
      nwa_ui_show_message("Not supported yet",
                          "LNURL links and Lightning addresses are not supported yet. "
                          "Ask the recipient for a Lightning invoice instead.",
                          "LNURL is not supported yet");
      call_free(c);
      break;
    case NWA_URI_BITCOIN_ONCHAIN: {
      g_autofree gchar *amt = u.amount_msat ? nwa_ui_format_msat(u.amount_msat) : NULL;
      g_autofree gchar *body = g_strdup_printf("bitcoin:%s%s%s\n\nThis wallet pays over Lightning "
                                               "only. Use an on-chain wallet for this address.",
                                               u.address, amt ? " — " : "", amt ? amt : "");
      nwa_ui_show_message("On-chain payment", body, "On-chain payments are not supported");
      call_free(c);
      break;
    }
  }
  nwa_uri_clear(&u);
}

void
nwa_service_open_uri(NwaService *s, const gchar *uri)
{
  open_uri_dispatch(s, nwa_caller_new_self(), uri, FALSE);
}

static void
on_open_uri_caller(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  OpenUriJob *j = data;
  GError *err = NULL;
  NwaCaller *caller = nwa_caller_identify_finish(res, &err);
  if (caller) {
    g_debug("nostr-wallet-agent: OpenUri from %s app=%s", caller->sender,
            caller->app_id ? caller->app_id : "(none)");
    open_uri_dispatch(j->svc, caller, j->uri, TRUE);
  } else {
    g_message("nostr-wallet-agent: OpenUri: %s", err->message);
    g_error_free(err);
  }
  memset(j->uri, 0, strlen(j->uri));
  g_free(j->uri);
  g_free(j);
}

/* ---------------------------------------------------------------------- */
/* lifecycle                                                               */

static GSettings *
maybe_settings(void)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (!src) return NULL;
  g_autoptr(GSettingsSchema) schema = g_settings_schema_source_lookup(src, NWA_SETTINGS_SCHEMA, TRUE);
  if (!schema) {
    g_message("nostr-wallet-agent: GSettings schema %s not installed; using defaults",
              NWA_SETTINGS_SCHEMA);
    return NULL;
  }
  return g_settings_new(NWA_SETTINGS_SCHEMA);
}

NwaService *
nwa_service_new(GDBusConnection *bus, GError **error)
{
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(nwa_dbus_xml, error);
  if (!node) return NULL;

  NwaService *s = g_new0(NwaService, 1);
  s->bus = g_object_ref(bus);
  s->node = node;
  s->cancel = g_cancellable_new();
  s->prompts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  s->settings = maybe_settings();
  s->origin_bridges = nwa_caller_origin_bridges();
  const gchar *eph = g_getenv("NOSTR_WALLET_AGENT_EPHEMERAL");
  s->ephemeral = eph && *eph && g_strcmp0(eph, "0") != 0;

  g_autofree gchar *path = nwa_budget_store_default_path();
  s->budgets = nwa_budget_store_new(path, NULL, NULL);
  GError *lerr = NULL;
  if (!nwa_budget_store_load(s->budgets, &lerr)) {
    g_warning("nostr-wallet-agent: cannot read %s: %s", path, lerr->message);
    g_clear_error(&lerr);
  }

  s->reg_id = g_dbus_connection_register_object(bus, NWA_OBJECT_PATH, node->interfaces[0],
                                                &vtable, s, NULL, error);
  if (!s->reg_id) {
    nwa_service_free(s);
    return NULL;
  }
  if (s->ephemeral) {
    g_message("nostr-wallet-agent: NOSTR_WALLET_AGENT_EPHEMERAL set; pairing is not persisted");
  } else {
    KeyringJob *j = g_new0(KeyringJob, 1);
    j->op = KR_LOOKUP;
    GTask *t = g_task_new(NULL, s->cancel, on_keyring_loaded, s);
    g_task_set_task_data(t, j, (GDestroyNotify)keyring_job_free);
    g_task_run_in_thread(t, keyring_thread);
    g_object_unref(t);
  }
  return s;
}

void
nwa_service_free(NwaService *s)
{
  if (!s) return;
  g_cancellable_cancel(s->cancel);
  if (s->reg_id) g_dbus_connection_unregister_object(s->bus, s->reg_id);
  if (s->client) {
    g_signal_handler_disconnect(s->client, s->notify_handler);
    nwa_nwc_client_stop(s->client);
    g_clear_object(&s->client);
  }
  nwa_budget_store_free(s->budgets);
  g_clear_object(&s->settings);
  g_hash_table_unref(s->prompts);
  g_strfreev(s->origin_bridges);
  g_dbus_node_info_unref(s->node);
  g_object_unref(s->cancel);
  g_object_unref(s->bus);
  g_free(s);
}
