/* nm_provider_nip07.c - window.nostr (NIP-07) -> org.nostr.Signer
 * (nostrc-jjyp)
 *
 * Every method is a thin, validated forward to the desktop signer daemon;
 * the host never holds key material. Mapping (see
 * nips/nip55l/dbus/org.nostr.Signer.xml):
 *
 *   getPublicKey()            GetPublicKeyForApp(app_id) -> npub, returned as hex
 *   signEvent(event)          SignEvent(canonical_event, identity, app_id)
 *                             -> complete signed event (nip55l >= 0.2.0);
 *                             app_id = page origin, so the signer's
 *                             ApprovalRequested dialog and ACL key on it
 *   getRelays()               GetRelaysForApp(app_id) -> {url: {read, write}};
 *                             Error.NotFound -> {}
 *   nip04.encrypt(pk, text)   NIP04EncryptForApp(text, pk, identity, app_id)
 *   nip04.decrypt(pk, ct)     NIP04DecryptForApp(ct, pk, identity, app_id)
 *   nip44.encrypt(pk, text)   NIP44EncryptForApp(text, pk, identity, app_id)
 *   nip44.decrypt(pk, ct)     NIP44DecryptForApp(ct, pk, identity, app_id)
 *
 * Every call carries the validated page origin. The daemon resolves whether
 * that origin is verified or only claimed and gates each operation. All calls
 * can wait for consent and therefore use the approval timeout.
 */
#include "nm_router.h"
#include "nm_policy.h"


#define SIGNER_PATH  "/org/nostr/signer"
#define SIGNER_IFACE "org.nostr.Signer"

static const gchar *const nip07_methods[] = {
  "getPublicKey", "signEvent", "getRelays",
  "nip04.encrypt", "nip04.decrypt", "nip44.encrypt", "nip44.decrypt",
  NULL
};

typedef enum { OP_PUBKEY, OP_SIGN, OP_RELAYS, OP_STRING } Op;

static const gchar *obj_str(JsonObject *o, const gchar *name) {
  JsonNode *n = o ? json_object_get_member(o, name) : NULL;
  if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
  return json_node_get_string(n);
}

typedef struct {
  NmRequest *req;
  Op op;
  gchar *sent_event; /* OP_SIGN: canonical unsigned event, to verify the reply */
} Call;

static void on_reply(GObject *src, GAsyncResult *res, gpointer user_data) {
  Call *c = user_data;
  NmRequest *req = c->req;
  Op op = c->op;
  g_autofree gchar *sent_event = c->sent_event;
  g_free(c);

  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (!ret) {
    NmErrorCode code = nm_error_from_dbus(err);
    if (op == OP_RELAYS && code == NM_ERR_NOT_FOUND) {
      /* NIP-07: no relays configured is an empty object, not an error. */
      JsonNode *empty = json_node_new(JSON_NODE_OBJECT);
      json_node_take_object(empty, json_object_new());
      nm_request_reply_result(req, empty);
      return;
    }
    if (code == NM_ERR_NOT_FOUND) code = NM_ERR_INTERNAL;
    g_debug("%s failed: %s", req->method, err ? err->message : "?");
    nm_request_reply_error(req, code, NULL);
    return;
  }

  const gchar *s = NULL;
  g_variant_get(ret, "(&s)", &s);

  switch (op) {
    case OP_PUBKEY: {
      g_autofree gchar *hex = nm_pubkey_to_hex(s);
      if (!hex) {
        nm_request_reply_error(req, NM_ERR_INTERNAL, "Signer returned an unparseable public key");
        return;
      }
      nm_request_reply_string(req, hex);
      return;
    }
    case OP_SIGN: {
      g_autoptr(JsonParser) p = json_parser_new();
      JsonNode *root = NULL;
      if (json_parser_load_from_data(p, s, -1, NULL)) root = json_parser_get_root(p);
      JsonObject *o = (root && JSON_NODE_HOLDS_OBJECT(root)) ? json_node_get_object(root) : NULL;
      if (!o || !obj_str(o, "sig")) {
        /* Pre-0.2.0 signers returned a bare signature string. */
        nm_request_reply_error(req, NM_ERR_UNSUPPORTED,
                               "Signer returned no signed event (org.nostr.Signer >= nip55l 0.2.0 required)");
        return;
      }
      const gchar *why = NULL;
      if (!nm_signed_event_matches(sent_event, root, &why)) {
        g_message("signEvent: rejecting signer reply: %s", why ? why : "?");
        nm_request_reply_error(req, NM_ERR_INTERNAL, "Signer returned an event that does not match the request");
        return;
      }
      nm_request_reply_result(req, json_node_copy(root));
      return;
    }
    case OP_RELAYS: {
      JsonNode *relays = nm_relays_to_nip07(s);
      if (!relays) {
        nm_request_reply_error(req, NM_ERR_INTERNAL, "Signer returned an unparseable relay list");
        return;
      }
      nm_request_reply_result(req, relays);
      return;
    }
    case OP_STRING:
      nm_request_reply_string(req, s);
      return;
  }
}

static void call_signer_full(NmRequest *req, Op op, const gchar *member, GVariant *args,
                             const gchar *sent_event) {
  GDBusConnection *bus = nm_router_get_bus(req->router);
  if (!bus) {
    if (args) g_variant_unref(g_variant_ref_sink(args));
    nm_request_reply_error(req, NM_ERR_SIGNER_UNAVAILABLE, NULL);
    return;
  }
  const NmRouterConfig *cfg = nm_router_get_config(req->router);
  Call *c = g_new0(Call, 1);
  c->req = req;
  c->op = op;
  c->sent_event = g_strdup(sent_event);
  g_dbus_connection_call(bus, cfg->signer_bus_name, SIGNER_PATH, SIGNER_IFACE, member,
                         args, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
                         cfg->approval_timeout_ms,
                         nm_router_get_cancellable(req->router), on_reply, c);
}

static void call_signer(NmRequest *req, Op op, const gchar *member, GVariant *args) {
  call_signer_full(req, op, member, args, NULL);
}

static void do_crypto(NmRequest *req, const gchar *member, const gchar *text_param) {
  const gchar *text = nm_request_param_string(req, text_param);
  g_autofree gchar *pk = nm_pubkey_normalize(nm_request_param_string(req, "pubkey"));
  if (!pk) {
    nm_request_reply_error(req, NM_ERR_INVALID_REQUEST, "pubkey must be 64 hex characters");
    return;
  }
  if (!text) {
    g_autofree gchar *msg = g_strdup_printf("%s must be a string", text_param);
    nm_request_reply_error(req, NM_ERR_INVALID_REQUEST, msg);
    return;
  }
  const NmRouterConfig *cfg = nm_router_get_config(req->router);
  call_signer(req, OP_STRING, member, g_variant_new("(ssss)", text, pk, cfg->identity, req->app_id));
}

static void nip07_dispatch(NmRequest *req) {
  const gchar *m = req->method;
  const NmRouterConfig *cfg = nm_router_get_config(req->router);

  if (g_strcmp0(m, "getPublicKey") == 0) {
    call_signer(req, OP_PUBKEY, "GetPublicKeyForApp", g_variant_new("(s)", req->app_id));
  } else if (g_strcmp0(m, "getRelays") == 0) {
    call_signer(req, OP_RELAYS, "GetRelaysForApp", g_variant_new("(s)", req->app_id));
  } else if (g_strcmp0(m, "signEvent") == 0) {
    const gchar *why = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autofree gchar *ev = nm_event_canonicalize(json_object_get_member(req->params, "event"), now, &why);
    if (!ev) {
      g_autofree gchar *msg = g_strdup_printf("Invalid event: %s", why ? why : "malformed");
      nm_request_reply_error(req, NM_ERR_INVALID_REQUEST, msg);
      return;
    }
    call_signer_full(req, OP_SIGN, "SignEvent",
                     g_variant_new("(sss)", ev, cfg->identity, req->app_id), ev);
  } else if (g_strcmp0(m, "nip04.encrypt") == 0) {
    do_crypto(req, "NIP04EncryptForApp", "plaintext");
  } else if (g_strcmp0(m, "nip04.decrypt") == 0) {
    do_crypto(req, "NIP04DecryptForApp", "ciphertext");
  } else if (g_strcmp0(m, "nip44.encrypt") == 0) {
    do_crypto(req, "NIP44EncryptForApp", "plaintext");
  } else if (g_strcmp0(m, "nip44.decrypt") == 0) {
    do_crypto(req, "NIP44DecryptForApp", "ciphertext");
  } else {
    nm_request_reply_error(req, NM_ERR_UNKNOWN_METHOD, NULL);
  }
}

const NmProvider nm_provider_nip07 = {
  .name = "nip07",
  .methods = nip07_methods,
  .requires_origin = TRUE,
  .dispatch = nip07_dispatch,
};
