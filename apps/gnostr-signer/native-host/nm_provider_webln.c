/* nm_provider_webln.c - window.webln (WebLN) -> org.nostr.Wallet1
 * (nostrc-jjyp)
 *
 * Every call is a validated forward to the desktop wallet agent
 * (gnome/nostr-wallet-agent). The agent identifies callers from their bus
 * connection, which for this host would be the browser for every site, so
 * page calls use the agent's origin-taking *For methods: the agent checks
 * that the caller really is this host (exe path + inode) and then keys
 * policy, budget, "Always allow this site", ledger and dialog on the page
 * origin (gnome/nostr-wallet-agent/README.md "Web origins").
 *
 *   webln.status            (no origin) Introspect + Paired property; never
 *                           prompts. {available, paired}: the extension
 *                           injects window.webln only when both are true.
 *   webln.enable()          same probe; {enabled: true} or not_paired /
 *                           wallet_unavailable / unsupported. The per-site
 *                           consent prompt lives in the extension.
 *   webln.getInfo()         GetInfoFor(origin)            -> WebLN getInfo
 *   webln.getBalance()      GetBalanceFor(origin)         -> {balance, currency: "sats"}
 *   webln.makeInvoice(args) MakeInvoiceFor(origin, msat, memo, 0)
 *                                                         -> {paymentRequest, rHash}
 *   webln.sendPayment(pr)   PayInvoiceFor(origin, pr, 0)  -> {preimage}
 *   webln.keysend / signMessage / verifyMessage / lnurl   -> unsupported
 *
 * Nothing here approves anything: read/receive may raise the agent's
 * wallet-access dialog and every payment goes through the agent's budget
 * or payment dialog, so wallet calls use the long wallet timeout.
 */
#include "nm_router.h"
#include "nm_webln.h"

#include <string.h>

#define WALLET_PATH  "/org/nostr/Wallet1"
#define WALLET_IFACE "org.nostr.Wallet1"

static const gchar *const webln_methods[] = {
  "webln.status", "webln.enable", "webln.getInfo", "webln.getBalance",
  "webln.makeInvoice", "webln.sendPayment",
  "webln.keysend", "webln.signMessage", "webln.verifyMessage", "webln.lnurl",
  NULL
};

static const gchar *const origin_optional[] = { "webln.status", NULL };

static const gchar *const unsupported_methods[] = {
  "webln.keysend", "webln.signMessage", "webln.verifyMessage", "webln.lnurl", NULL
};

/* ---- availability probe (status / enable) -------------------------------- */

typedef struct {
  NmRequest *req;
  gboolean enable;
} Probe;

static void probe_finish(Probe *p, gboolean available, gboolean paired, NmErrorCode why,
                         const gchar *message) {
  NmRequest *req = p->req;
  gboolean enable = p->enable;
  g_free(p);
  if (enable) {
    if (!available) nm_request_reply_error(req, why, message);
    else if (!paired) nm_request_reply_error(req, NM_ERR_NOT_PAIRED, NULL);
    else {
      g_autoptr(JsonBuilder) b = json_builder_new();
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "enabled");
      json_builder_add_boolean_value(b, TRUE);
      json_builder_end_object(b);
      nm_request_reply_result(req, json_builder_get_root(b));
    }
    return;
  }
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "available");
  json_builder_add_boolean_value(b, available);
  json_builder_set_member_name(b, "paired");
  json_builder_add_boolean_value(b, available && paired);
  if (!available) {
    json_builder_set_member_name(b, "reason");
    json_builder_add_string_value(b, nm_error_code_str(why));
  }
  json_builder_end_object(b);
  nm_request_reply_result(req, json_builder_get_root(b));
}

static void on_paired(GObject *src, GAsyncResult *res, gpointer user_data) {
  Probe *p = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (!ret) {
    g_autofree gchar *msg = NULL;
    NmErrorCode code = nm_webln_error_from_dbus(err, &msg);
    probe_finish(p, FALSE, FALSE, code, msg);
    return;
  }
  g_autoptr(GVariant) v = NULL;
  g_variant_get(ret, "(v)", &v);
  gboolean paired = g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
  probe_finish(p, TRUE, paired, NM_ERR_INTERNAL, NULL);
}

static void on_introspect(GObject *src, GAsyncResult *res, gpointer user_data) {
  Probe *p = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (!ret) {
    g_autofree gchar *msg = NULL;
    NmErrorCode code = nm_webln_error_from_dbus(err, &msg);
    if (code == NM_ERR_UNSUPPORTED) code = NM_ERR_WALLET_UNAVAILABLE; /* no such object */
    g_debug("webln: wallet agent not reachable: %s", err->message);
    probe_finish(p, FALSE, FALSE, code, NULL);
    return;
  }
  const gchar *xml = NULL;
  g_variant_get(ret, "(&s)", &xml);
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
  GDBusInterfaceInfo *iface = node ? g_dbus_node_info_lookup_interface(node, WALLET_IFACE) : NULL;
  if (!iface || !g_dbus_interface_info_lookup_method(iface, "GetInfoFor") ||
      !g_dbus_interface_info_lookup_method(iface, "PayInvoiceFor")) {
    /* An agent without per-site methods would charge every site to the
     * browser's budget: do not offer WebLN at all. */
    probe_finish(p, FALSE, FALSE, NM_ERR_UNSUPPORTED,
                 "The wallet agent does not support per-site requests; update nostr-wallet-agent");
    return;
  }
  NmRequest *req = p->req;
  const NmRouterConfig *cfg = nm_router_get_config(req->router);
  g_dbus_connection_call(G_DBUS_CONNECTION(src), cfg->wallet_bus_name, WALLET_PATH,
                         "org.freedesktop.DBus.Properties", "Get",
                         g_variant_new("(ss)", WALLET_IFACE, "Paired"), G_VARIANT_TYPE("(v)"),
                         G_DBUS_CALL_FLAGS_NONE, cfg->call_timeout_ms,
                         nm_router_get_cancellable(req->router), on_paired, p);
}

static void probe(NmRequest *req, gboolean enable) {
  Probe *p = g_new0(Probe, 1);
  p->req = req;
  p->enable = enable;
  GDBusConnection *bus = nm_router_get_bus(req->router);
  if (!bus) {
    probe_finish(p, FALSE, FALSE, NM_ERR_WALLET_UNAVAILABLE, NULL);
    return;
  }
  const NmRouterConfig *cfg = nm_router_get_config(req->router);
  /* Introspecting an activatable name starts the agent if needed. */
  g_dbus_connection_call(bus, cfg->wallet_bus_name, WALLET_PATH,
                         "org.freedesktop.DBus.Introspectable", "Introspect", NULL,
                         G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, cfg->call_timeout_ms,
                         nm_router_get_cancellable(req->router), on_introspect, p);
}

/* ---- wallet calls ------------------------------------------------------- */

typedef enum { W_INFO, W_BALANCE, W_INVOICE, W_PAY } WalletOp;

typedef struct {
  NmRequest *req;
  WalletOp op;
} WalletCall;

static JsonNode *object1(const gchar *k1, JsonNode *v1, const gchar *k2, JsonNode *v2) {
  JsonObject *o = json_object_new();
  json_object_set_member(o, k1, v1);
  if (k2) json_object_set_member(o, k2, v2);
  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, o);
  return n;
}

static JsonNode *str_node(const gchar *s) {
  JsonNode *n = json_node_new(JSON_NODE_VALUE);
  json_node_set_string(n, s ? s : "");
  return n;
}

static void on_wallet_reply(GObject *src, GAsyncResult *res, gpointer user_data) {
  WalletCall *c = user_data;
  NmRequest *req = c->req;
  WalletOp op = c->op;
  g_free(c);

  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (!ret) {
    g_autofree gchar *msg = NULL;
    NmErrorCode code = nm_webln_error_from_dbus(err, &msg);
    if (code == NM_ERR_TIMEOUT && op == W_PAY) {
      g_free(msg);
      msg = g_strdup("The wallet did not answer in time; the payment may still have been made");
    }
    g_debug("%s failed: %s", req->method, err->message);
    nm_request_reply_error(req, code, msg);
    return;
  }

  switch (op) {
    case W_INFO: {
      g_autoptr(GVariant) info = g_variant_get_child_value(ret, 0);
      JsonNode *n = nm_webln_info_from_vardict(info);
      if (!n) nm_request_reply_error(req, NM_ERR_NOT_PAIRED, NULL);
      else nm_request_reply_result(req, n);
      return;
    }
    case W_BALANCE: {
      guint64 msat = 0;
      g_variant_get(ret, "(t)", &msat);
      JsonNode *bal = json_node_new(JSON_NODE_VALUE);
      json_node_set_int(bal, (gint64)(msat / 1000u)); /* whole sats, rounded down */
      nm_request_reply_result(req, object1("balance", bal, "currency", str_node("sats")));
      return;
    }
    case W_INVOICE: {
      const gchar *bolt11 = NULL, *hash = NULL;
      g_variant_get(ret, "(&s&s)", &bolt11, &hash);
      nm_request_reply_result(req, object1("paymentRequest", str_node(bolt11), "rHash", str_node(hash)));
      return;
    }
    case W_PAY: {
      const gchar *preimage = NULL;
      guint64 fees = 0;
      g_variant_get(ret, "(&st)", &preimage, &fees);
      nm_request_reply_result(req, object1("preimage", str_node(preimage), NULL, NULL));
      return;
    }
  }
}

static void wallet_call(NmRequest *req, WalletOp op, const gchar *member, GVariant *args,
                        const gchar *reply_type) {
  GDBusConnection *bus = nm_router_get_bus(req->router);
  if (!bus) {
    g_variant_unref(g_variant_ref_sink(args));
    nm_request_reply_error(req, NM_ERR_WALLET_UNAVAILABLE, NULL);
    return;
  }
  const NmRouterConfig *cfg = nm_router_get_config(req->router);
  WalletCall *c = g_new0(WalletCall, 1);
  c->req = req;
  c->op = op;
  g_dbus_connection_call(bus, cfg->wallet_bus_name, WALLET_PATH, WALLET_IFACE, member, args,
                         G_VARIANT_TYPE(reply_type), G_DBUS_CALL_FLAGS_NONE, cfg->wallet_timeout_ms,
                         nm_router_get_cancellable(req->router), on_wallet_reply, c);
}

static void webln_dispatch(NmRequest *req) {
  const gchar *m = req->method;

  if (g_strv_contains(unsupported_methods, m)) {
    g_autofree gchar *msg = g_strdup_printf("%s is not supported by the desktop wallet", m + strlen("webln."));
    nm_request_reply_error(req, NM_ERR_UNSUPPORTED, msg);
  } else if (!strcmp(m, "webln.status")) {
    probe(req, FALSE);
  } else if (!strcmp(m, "webln.enable")) {
    probe(req, TRUE);
  } else if (!strcmp(m, "webln.getInfo")) {
    wallet_call(req, W_INFO, "GetInfoFor", g_variant_new("(s)", req->app_id), "(a{sv})");
  } else if (!strcmp(m, "webln.getBalance")) {
    wallet_call(req, W_BALANCE, "GetBalanceFor", g_variant_new("(s)", req->app_id), "(t)");
  } else if (!strcmp(m, "webln.makeInvoice")) {
    guint32 msat = 0;
    const gchar *memo = NULL, *why = NULL;
    NmErrorCode code = NM_ERR_INVALID_REQUEST;
    if (!nm_webln_resolve_invoice(req->params, &msat, &memo, &code, &why)) {
      nm_request_reply_error(req, code, why);
      return;
    }
    wallet_call(req, W_INVOICE, "MakeInvoiceFor",
                g_variant_new("(susu)", req->app_id, msat, memo, (guint32)0), "(ss)");
  } else if (!strcmp(m, "webln.sendPayment")) {
    const gchar *pr = nm_request_param_string(req, "paymentRequest");
    if (!nm_webln_is_bolt11(pr)) {
      nm_request_reply_error(req, NM_ERR_INVALID_REQUEST, "paymentRequest must be a BOLT-11 invoice");
      return;
    }
    wallet_call(req, W_PAY, "PayInvoiceFor", g_variant_new("(ssu)", req->app_id, pr, (guint32)0), "(st)");
  } else {
    nm_request_reply_error(req, NM_ERR_UNKNOWN_METHOD, NULL);
  }
}

const NmProvider nm_provider_webln = {
  .name = "webln",
  .methods = webln_methods,
  .requires_origin = TRUE,
  .origin_optional = origin_optional,
  .dispatch = webln_dispatch,
};
