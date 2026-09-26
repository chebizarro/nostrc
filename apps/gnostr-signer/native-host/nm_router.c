/* nm_router.c - request router for the NIP-07 bridge (nostrc-jjyp)
 *
 * Wire protocol (extension -> host), one JSON object per frame:
 *   {"id": "<1..64 printable ASCII>", "method": "<name>",
 *    "origin": "https://site.example", "params": {...}}
 * Replies (host -> extension), correlated by id, possibly out of order:
 *   {"id": "...", "result": <value>}
 *   {"id": "...", "error": {"code": "<nm_error_code_str>", "message": "..."}}
 * A frame that cannot be attributed to a request (bad JSON, oversized)
 * gets an error reply with "id": null.
 *
 * "origin" is filled in by the extension's background script from the
 * browser-provided sender, never from page-supplied data; the host
 * re-validates it and derives the app_id from it (nm_policy.c).
 */
#include "nm_router.h"
#include "native_messaging.h"
#include "nm_policy.h"

#include <string.h>

#ifndef NM_HOST_VERSION
#define NM_HOST_VERSION "0.2.0"
#endif

static const NmProvider *const providers[] = {
  &nm_provider_nip07,
  &nm_provider_webln,
};

struct _NmRouter {
  gint ref;
  gboolean closed;
  GDBusConnection *bus;
  NmRouterConfig cfg;
  gchar *identity;
  gchar *bus_name;
  gchar *wallet_bus_name;
  NmReplyFunc reply;
  gpointer reply_data;
  GHashTable *in_flight; /* id -> NULL */
  GCancellable *cancel;
};

static NmRouter *router_ref(NmRouter *r) { g_atomic_int_inc(&r->ref); return r; }

static void router_unref(NmRouter *r) {
  if (!g_atomic_int_dec_and_test(&r->ref)) return;
  g_clear_object(&r->bus);
  g_clear_object(&r->cancel);
  g_hash_table_unref(r->in_flight);
  g_free(r->identity);
  g_free(r->bus_name);
  g_free(r->wallet_bus_name);
  g_free(r);
}

NmRouter *nm_router_new(GDBusConnection *bus, const NmRouterConfig *config,
                        NmReplyFunc reply, gpointer reply_data) {
  NmRouter *r = g_new0(NmRouter, 1);
  r->ref = 1;
  r->bus = bus ? g_object_ref(bus) : NULL;
  if (config) r->cfg = *config;
  r->identity = g_strdup(r->cfg.identity ? r->cfg.identity : "");
  r->bus_name = g_strdup(r->cfg.signer_bus_name ? r->cfg.signer_bus_name : "org.nostr.Signer");
  r->wallet_bus_name = g_strdup(r->cfg.wallet_bus_name ? r->cfg.wallet_bus_name : "org.nostr.Wallet1");
  r->cfg.identity = r->identity;
  r->cfg.signer_bus_name = r->bus_name;
  r->cfg.wallet_bus_name = r->wallet_bus_name;
  if (r->cfg.call_timeout_ms <= 0) r->cfg.call_timeout_ms = 30000;
  if (r->cfg.approval_timeout_ms <= 0) r->cfg.approval_timeout_ms = 120000;
  /* agent approval-timeout (120 s) + request-timeout (60 s) + slack */
  if (r->cfg.wallet_timeout_ms <= 0) r->cfg.wallet_timeout_ms = 190000;
  r->reply = reply;
  r->reply_data = reply_data;
  r->in_flight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  r->cancel = g_cancellable_new();
  return r;
}

void nm_router_free(NmRouter *r) {
  if (!r) return;
  r->closed = TRUE;
  g_cancellable_cancel(r->cancel);
  router_unref(r);
}

guint nm_router_in_flight(NmRouter *r) { return g_hash_table_size(r->in_flight); }
const NmRouterConfig *nm_router_get_config(NmRouter *r) { return &r->cfg; }
GCancellable *nm_router_get_cancellable(NmRouter *r) { return r->cancel; }

GDBusConnection *nm_router_get_bus(NmRouter *r) {
  if (!r->bus) {
    g_autoptr(GError) err = NULL;
    r->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!r->bus)
      g_message("session bus unavailable: %s", err ? err->message : "?");
  }
  return r->bus;
}

/* ---- serialization ------------------------------------------------------ */

static void emit(NmRouter *r, JsonNode *root /* transfer full */) {
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  gsize len = 0;
  g_autofree gchar *json = json_generator_to_data(gen, &len);
  json_node_unref(root);
  if (!r->closed && r->reply) r->reply(json, len, r->reply_data);
}

static JsonNode *error_envelope(const gchar *id, NmErrorCode code, const gchar *message) {
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  if (id) json_builder_add_string_value(b, id);
  else json_builder_add_null_value(b);
  json_builder_set_member_name(b, "error");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "code");
  json_builder_add_string_value(b, nm_error_code_str(code));
  json_builder_set_member_name(b, "message");
  json_builder_add_string_value(b, message ? message : nm_error_default_message(code));
  json_builder_end_object(b);
  json_builder_end_object(b);
  return json_builder_get_root(b);
}

static void reply_error_raw(NmRouter *r, const gchar *id, NmErrorCode code, const gchar *message) {
  emit(r, error_envelope(id, code, message));
}

void nm_router_reply_frame_error(NmRouter *r, NmErrorCode code) {
  reply_error_raw(r, NULL, code, NULL);
}

static void request_free(NmRequest *req) {
  NmRouter *r = req->router;
  g_hash_table_remove(r->in_flight, req->id);
  g_free(req->id);
  g_free(req->method);
  g_free(req->app_id);
  if (req->root) json_node_unref(req->root);
  g_free(req);
  router_unref(r);
}

void nm_request_reply_result(NmRequest *req, JsonNode *result) {
  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "id", req->id);
  json_object_set_member(o, "result", result ? result : json_node_new(JSON_NODE_NULL));
  JsonNode *root = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(root, o);

  /* Enforce the host->browser frame limit here so an oversized plaintext
   * becomes a clean too_large error instead of a dropped connection. */
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  gsize len = 0;
  g_autofree gchar *json = json_generator_to_data(gen, &len);
  json_node_unref(root);
  if (len > NM_MAX_MESSAGE_SIZE) {
    reply_error_raw(req->router, req->id, NM_ERR_TOO_LARGE, "Result exceeds the 1 MiB message limit");
  } else if (!req->router->closed && req->router->reply) {
    req->router->reply(json, len, req->router->reply_data);
  }
  request_free(req);
}

void nm_request_reply_string(NmRequest *req, const gchar *result) {
  JsonNode *n = json_node_new(JSON_NODE_VALUE);
  json_node_set_string(n, result ? result : "");
  nm_request_reply_result(req, n);
}

void nm_request_reply_error(NmRequest *req, NmErrorCode code, const gchar *message) {
  reply_error_raw(req->router, req->id, code, message);
  request_free(req);
}

const gchar *nm_request_param_string(NmRequest *req, const gchar *name) {
  JsonNode *n = json_object_get_member(req->params, name);
  if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(n);
}

/* ---- parsing / dispatch ------------------------------------------------- */

static const NmProvider *find_provider(const gchar *method) {
  for (gsize i = 0; i < G_N_ELEMENTS(providers); i++)
    for (const gchar *const *m = providers[i]->methods; m && *m; m++)
      if (g_strcmp0(*m, method) == 0) return providers[i];
  return NULL;
}

static gchar *parse_id(JsonNode *n) {
  if (!n || !JSON_NODE_HOLDS_VALUE(n)) return NULL;
  gchar *id = NULL;
  if (json_node_get_value_type(n) == G_TYPE_STRING)
    id = g_strdup(json_node_get_string(n));
  else if (json_node_get_value_type(n) == G_TYPE_INT64)
    id = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(n));
  if (!id) return NULL;
  gsize n_chars = strlen(id);
  if (n_chars == 0 || n_chars > NM_MAX_ID_LEN) { g_free(id); return NULL; }
  for (gsize i = 0; i < n_chars; i++)
    if (!g_ascii_isprint(id[i])) { g_free(id); return NULL; }
  return id;
}

static void reply_hello(NmRouter *r, const gchar *id) {
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  json_builder_add_string_value(b, id);
  json_builder_set_member_name(b, "result");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "host");
  json_builder_add_string_value(b, NM_HOST_NAME);
  json_builder_set_member_name(b, "version");
  json_builder_add_string_value(b, NM_HOST_VERSION);
  json_builder_set_member_name(b, "protocol");
  json_builder_add_int_value(b, NM_PROTOCOL_VERSION);
  json_builder_set_member_name(b, "providers");
  json_builder_begin_object(b);
  for (gsize i = 0; i < G_N_ELEMENTS(providers); i++) {
    json_builder_set_member_name(b, providers[i]->name);
    json_builder_begin_array(b);
    for (const gchar *const *m = providers[i]->methods; m && *m; m++)
      json_builder_add_string_value(b, *m);
    json_builder_end_array(b);
  }
  json_builder_end_object(b);
  json_builder_end_object(b);
  json_builder_end_object(b);
  emit(r, json_builder_get_root(b));
}

void nm_router_handle(NmRouter *r, const gchar *json, gsize len) {
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json || !json_parser_load_from_data(parser, json, (gssize)len, NULL)) {
    reply_error_raw(r, NULL, NM_ERR_INVALID_REQUEST, "Malformed JSON");
    return;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    reply_error_raw(r, NULL, NM_ERR_INVALID_REQUEST, "Request must be a JSON object");
    return;
  }
  JsonObject *obj = json_node_get_object(root);

  g_autofree gchar *id = parse_id(json_object_get_member(obj, "id"));
  if (!id) {
    reply_error_raw(r, NULL, NM_ERR_INVALID_REQUEST, "Missing or invalid request id");
    return;
  }

  if (nm_json_has_nul_escape(json, len)) {
    reply_error_raw(r, id, NM_ERR_INVALID_REQUEST, "Strings must not contain NUL characters");
    return;
  }

  JsonNode *mnode = json_object_get_member(obj, "method");
  if (!mnode || !JSON_NODE_HOLDS_VALUE(mnode) || json_node_get_value_type(mnode) != G_TYPE_STRING) {
    reply_error_raw(r, id, NM_ERR_INVALID_REQUEST, "Missing method");
    return;
  }
  const gchar *method = json_node_get_string(mnode);

  if (g_strcmp0(method, "host.hello") == 0) {
    reply_hello(r, id);
    return;
  }

  const NmProvider *prov = find_provider(method);
  if (!prov) {
    reply_error_raw(r, id, NM_ERR_UNKNOWN_METHOD, NULL);
    return;
  }

  if (g_hash_table_contains(r->in_flight, id)) {
    reply_error_raw(r, id, NM_ERR_BUSY, "Duplicate request id");
    return;
  }
  if (g_hash_table_size(r->in_flight) >= NM_MAX_IN_FLIGHT) {
    reply_error_raw(r, id, NM_ERR_BUSY, NULL);
    return;
  }

  g_autofree gchar *app_id = NULL;
  gboolean origin_needed = prov->requires_origin &&
    !(prov->origin_optional && g_strv_contains(prov->origin_optional, method));
  if (origin_needed) {
    JsonNode *onode = json_object_get_member(obj, "origin");
    const gchar *origin = NULL;
    if (onode && JSON_NODE_HOLDS_VALUE(onode) && json_node_get_value_type(onode) == G_TYPE_STRING)
      origin = json_node_get_string(onode);
    const gchar *why = NULL;
    app_id = nm_origin_to_app_id(origin, &why);
    if (!app_id) {
      g_autofree gchar *msg = g_strdup_printf("Origin refused: %s", why ? why : "invalid");
      reply_error_raw(r, id, NM_ERR_ORIGIN_DENIED, msg);
      return;
    }
  }

  JsonNode *pnode = json_object_get_member(obj, "params");
  if (pnode && !JSON_NODE_HOLDS_NULL(pnode) && !JSON_NODE_HOLDS_OBJECT(pnode)) {
    reply_error_raw(r, id, NM_ERR_INVALID_REQUEST, "params must be an object");
    return;
  }

  NmRequest *req = g_new0(NmRequest, 1);
  req->router = router_ref(r);
  req->id = g_steal_pointer(&id);
  req->method = g_strdup(method);
  req->app_id = g_steal_pointer(&app_id);
  req->root = json_node_ref(root);
  if (pnode && JSON_NODE_HOLDS_OBJECT(pnode)) {
    req->params = json_node_get_object(pnode);
  } else {
    /* Park an empty object on the root so params outlives the parser. */
    json_object_set_object_member(obj, "params", json_object_new());
    req->params = json_object_get_object_member(obj, "params");
  }
  g_hash_table_add(r->in_flight, g_strdup(req->id));
  prov->dispatch(req);
}
