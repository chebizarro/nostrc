/* NIP-46 transport adapter. All policy and account cryptography belong to the
 * daemon gate, never the relay core ACL or a GUI-owned client session. */
#include "bunker_service.h"
#include "accounts_store.h"
#include "pairing_uri.h"
#include <openssl/rand.h>
#include "signer_gate.h"
#include "signer_approval_service.h"
#include "nip55l_dbus_errors.h"
#include <nostr/nip55l/signer_ops.h>
#include <nostr/nip55l/error.h>
#include <nostr/nip46/nip46_bunker.h>
#include <nostr/nip46/nip46_client.h>
#include <nostr/nip19/nip19.h>
#include <nostr-utils.h>
#include <nostr-gobject-1.0/nostr_nip46_bunker.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostrc-profile-guard.h>
#include <keys.h>
#include <secure_buf.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <time.h>

#define FRESHNESS 600
#define CACHE_MAX 1024
#define SEEN_MAX 16384

typedef struct {
  BunkerService *service;
  gchar *key, *client, *digest, *method, *identity, *offer_id;
  GPtrArray *requests;
  SignerGateHandle *handle;
  GSource *connect_delivery;
  gboolean completed;
  gint pairing_tier;
  gint64 expires;
  guint64 generation, revision;
  gchar *result, *error;
} Rpc;
typedef struct {
  BunkerService *service;
  gchar *id, *identity, *label, *client, *hints, *name, *request_id;
  gchar **relays;
  nostr_secure_buf secret;
  gint64 deadline;
  guint generation;
  gboolean outbound, approved, consumed, terminal;
  gint tier;
  SignerGateHandle *handle;
  NostrNip46CancelHandle *delivery_cancel;
  guint64 security_generation;
  GSource *expiry, *delivery;
} PairOffer;
static void offer_free(gpointer data) {
  PairOffer *o = data;
  if (o->expiry) { g_source_destroy(o->expiry); g_source_unref(o->expiry); }
  if (o->delivery) { g_source_destroy(o->delivery); g_source_unref(o->delivery); }
  nostr_nip46_cancel_handle_cancel(o->delivery_cancel); nostr_nip46_cancel_handle_unref(o->delivery_cancel);
  signer_gate_handle_unref(o->handle); secure_free(&o->secret);
  g_free(o->request_id); g_free(o->id); g_free(o->identity); g_free(o->label); g_free(o->client); g_free(o->hints); g_free(o->name);
  g_strfreev(o->relays); g_free(o);
}
static void deliver_offers(BunkerService *s);
struct _BunkerService {
  gint refs;
  guint generation;
  gboolean closing, starting, no_ui;
  BunkerState state;
  gchar *error_message, *identity_npub, *transport_pubkey, *seen_path;
  GNostrNip46Bunker *bunker;
  gchar **relays, **configured_relays, **allowed_methods, **allowed_pubkeys;
  GHashTable *connections, *rpcs, *relay_states, *offers;
  BunkerPairingCommitCb commit; gpointer commit_ud;
  GKeyFile *seen;
  gboolean seen_healthy;
  BunkerStateChangedCb state_cb; gpointer state_cb_ud;
  BunkerConnectionCb conn_cb; gpointer conn_cb_ud;
  BunkerClientLookupCb lookup; gpointer lookup_ud;
};
static void wipe(gchar *s) { if (s) { secure_wipe(s, strlen(s)); g_free(s); } }
void bunker_connection_free(BunkerConnection *c) {
  if (!c) return;
  g_free(c->client_pubkey); g_free(c->app_name); g_strfreev(c->permissions); g_free(c);
}
void bunker_sign_request_free(BunkerSignRequest *r) {
  if (!r) return;
  g_free(r->request_id); g_free(r->client_pubkey); g_free(r->method);
  wipe(r->event_json); g_free(r->preview); g_free(r);
}
static void rpc_free(gpointer data) {
  Rpc *r = data;
  if (r->connect_delivery) { g_source_destroy(r->connect_delivery); g_source_unref(r->connect_delivery); }
  signer_gate_handle_unref(r->handle); g_ptr_array_unref(r->requests);
  g_free(r->key); g_free(r->client); g_free(r->digest); g_free(r->method); g_free(r->identity);
  g_free(r->offer_id); wipe(r->result); g_free(r->error); g_free(r);
}
static void set_state(BunkerService *s, BunkerState state, const gchar *error) {
  if (s->state == state && !g_strcmp0(s->error_message, error)) return;
  const gchar *names[] = {"DISABLED", "STARTING", "LISTENING", "ERROR", "DEGRADED", "OFFLINE", "STOPPING"};
  g_message("BUNKER_STATE state=%s error=%s", names[state], error ? error : "none");
  s->state = state; g_free(s->error_message); s->error_message = g_strdup(error);
  if (s->state_cb) s->state_cb(state, error, s->state_cb_ud);
}
static void service_unref(BunkerService *s) {
  if (!g_atomic_int_dec_and_test(&s->refs)) return;
  g_free(s->error_message); g_free(s->identity_npub); g_free(s->transport_pubkey); g_free(s->seen_path);
  g_strfreev(s->relays); g_strfreev(s->configured_relays); g_strfreev(s->allowed_methods); g_strfreev(s->allowed_pubkeys);
  g_hash_table_unref(s->connections); g_hash_table_unref(s->rpcs); g_hash_table_unref(s->relay_states);
  g_hash_table_unref(s->offers); g_key_file_unref(s->seen); g_free(s);
}
static gboolean contains(gchar **values, const gchar *value) {
  return values && g_strv_contains((const gchar *const *)values, value);
}
static gboolean hex64(const gchar *s) {
  if (!s || strlen(s) != 64) return FALSE;
  for (guint i = 0; i < 64; i++) if (!g_ascii_isxdigit(s[i])) return FALSE;
  return TRUE;
}
static gchar *string_token(const gchar *s) {
  JsonNode *node = json_node_new(JSON_NODE_VALUE); json_node_set_string(node, s ? s : "");
  gchar *out = json_to_string(node, FALSE); json_node_free(node); return out;
}
static gchar *configured_relays_token(BunkerService *s) {
  JsonBuilder *b = json_builder_new(); json_builder_begin_array(b);
  gchar **relays = s->configured_relays ? s->configured_relays : s->relays;
  for (guint i = 0; relays && relays[i]; i++) json_builder_add_string_value(b, relays[i]);
  json_builder_end_array(b); JsonNode *node = json_builder_get_root(b);
  gchar *token = json_to_string(node, FALSE); json_node_free(node); g_object_unref(b); return token;
}
static gchar *relay_map(const gchar *list) {
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!list || !json_parser_load_from_data(parser, list, -1, NULL) || !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser))) return NULL;
  JsonArray *urls = json_node_get_array(json_parser_get_root(parser));
  g_autoptr(JsonBuilder) builder = json_builder_new(); json_builder_begin_object(builder);
  for (guint i = 0; i < json_array_get_length(urls); i++) {
    json_builder_set_member_name(builder, json_array_get_string_element(urls, i)); json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "read"); json_builder_add_boolean_value(builder, TRUE);
    json_builder_set_member_name(builder, "write"); json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);
  }
  json_builder_end_object(builder); g_autoptr(JsonNode) node = json_builder_get_root(builder);
  return json_to_string(node, FALSE);
}
static gboolean paired(BunkerService *s, const gchar *client, gchar **identity) {
  if (identity) *identity = NULL;
  return s->lookup && s->lookup(client, identity, s->lookup_ud);
}
static void complete(Rpc *r, const gchar *result, const gchar *error) {
  r->completed = TRUE; r->expires = time(NULL) + FRESHNESS;
  r->result = g_strdup(result); r->error = g_strdup(error);
  for (guint i = 0; i < r->requests->len; i++)
    nostr_nip46_bunker_request_complete(r->requests->pdata[i], result, error);
  g_ptr_array_set_size(r->requests, 0);
}
static gboolean connect_delivered(gpointer data) {
  Rpc *r = data; BunkerService *s = r->service;
  g_clear_pointer(&r->connect_delivery, g_source_unref);
  if (r->completed) return G_SOURCE_REMOVE;
  g_autofree gchar *identity = NULL;
  SignerApprovalService *current = signer_get_approval_service();
  if (!current || signer_approval_service_is_paused(current) ||
      r->generation != signer_approval_service_generation(current)) complete(r, NULL, "paused");
  else if (r->offer_id && !g_hash_table_contains(s->offers, r->offer_id)) complete(r, NULL, "pairing_expired");
  else if (!paired(s, r->client, &identity) &&
      (!s->commit || !s->commit(r->client, r->identity,
          r->offer_id ? ((PairOffer *)g_hash_table_lookup(s->offers, r->offer_id))->label : NULL, NULL,
          r->offer_id ? ((PairOffer *)g_hash_table_lookup(s->offers, r->offer_id))->tier : r->pairing_tier, s->commit_ud)))
    complete(r, NULL, "pairing_persistence_failed");
  else {
    if (identity) { g_free(r->identity); r->identity = g_strdup(identity); }
    PairOffer *offer = r->offer_id ? g_hash_table_lookup(s->offers, r->offer_id) : NULL;
    if (offer) offer->consumed = TRUE;
    complete(r, "\"ack\"", NULL);
    SignerApprovalService *approval = signer_get_approval_service();
    if (approval) {
      r->generation = signer_approval_service_generation(approval);
      r->revision = signer_permissions_revision(signer_approval_service_permissions(approval));
    }
  }
  return G_SOURCE_REMOVE;
}
static void gate_reply(gpointer data, const gchar *error, const gchar *message, const gchar *result) {
  (void)message;
  Rpc *r = data; BunkerService *s = r->service;
  if (r->completed) return;
  if (error) { complete(r, NULL, error); return; }
  if (!strcmp(r->method, "connect")) {
    PairOffer *offer = r->offer_id ? g_hash_table_lookup(s->offers, r->offer_id) : NULL;
    if (r->offer_id && (!offer || offer->terminal || offer->generation != s->generation || offer->deadline <= g_get_monotonic_time())) {
      complete(r, NULL, "pairing_expired"); return;
    }
    BunkerConnection *conn = g_new0(BunkerConnection, 1);
    conn->client_pubkey = g_strdup(r->client); conn->connected_at = time(NULL);
    g_hash_table_replace(s->connections, g_strdup(r->client), conn);
    if (s->conn_cb) s->conn_cb(conn, s->conn_cb_ud);
    /* The gate's terminal observer persists explicit consent before this
     * idle delivery. No acknowledgement is sent if durable pairing failed. */
    r->connect_delivery = g_idle_source_new();
    g_source_set_callback(r->connect_delivery, connect_delivered, r, NULL);
    g_source_attach(r->connect_delivery, g_main_context_get_thread_default());
    return;
  }
  gchar *token;
  if (!strcmp(r->method, "get_relays")) {
    g_autofree gchar *map = relay_map(result);
    if (!map) { complete(r, NULL, "invalid_configured_relays"); return; }
    token = string_token(map);
  } else if (!strcmp(r->method, "get_public_key")) {
    g_autoptr(GNostrNip19) decoded = result ? gnostr_nip19_decode(result, NULL) : NULL;
    const gchar *hex = decoded ? gnostr_nip19_get_pubkey(decoded) : result;
    if (!hex64(hex)) { complete(r, NULL, "invalid_account_key"); return; }
    token = string_token(hex);
  } else token = string_token(result);
  complete(r, token, NULL); wipe(token);
  SignerApprovalService *approval = signer_get_approval_service();
  if (approval) {
    r->generation = signer_approval_service_generation(approval);
    r->revision = signer_permissions_revision(signer_approval_service_permissions(approval));
  }
}
static void prune(BunkerService *s) {
  gint64 now = time(NULL);
  GHashTableIter it; gpointer value;
  g_hash_table_iter_init(&it, s->rpcs);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Rpc *r = value;
    if (r->completed && r->expires <= now) g_hash_table_iter_remove(&it);
  }
  gsize count = 0; g_auto(GStrv) groups = g_key_file_get_groups(s->seen, &count);
  for (gsize i = 0; i < count; i++) {
    if (!strcmp(groups[i], "meta")) continue;
    if (g_key_file_get_int64(s->seen, groups[i], "expires", NULL) < now)
      g_key_file_remove_group(s->seen, groups[i], NULL);
  }
}
static gboolean persist_seen(BunkerService *s) {
  g_autofree gchar *dir = g_path_get_dirname(s->seen_path);
  gsize len; g_autofree gchar *text = g_key_file_to_data(s->seen, &len, NULL);
  if (nostrc_profile_guard_refuses_path(s->seen_path) || g_mkdir_with_parents(dir, 0700) ||
      !g_file_set_contents_full(s->seen_path, text, len,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, NULL)) {
    s->seen_healthy = FALSE; return FALSE;
  }
  return TRUE;
}
static gboolean remember_seen(BunkerService *s, const gchar *event_id, const gchar *rpc_key, const gchar *digest, gint64 created) {
  if (!s->seen_healthy || !event_id || !hex64(event_id)) return FALSE;
  gsize count; g_auto(GStrv) groups = g_key_file_get_groups(s->seen, &count);
  if (count + 2 > SEEN_MAX) return FALSE; /* never evict a still-fresh replay guard */
  g_key_file_set_int64(s->seen, event_id, "expires", created + FRESHNESS);
  g_key_file_set_int64(s->seen, rpc_key, "expires", created + FRESHNESS);
  g_key_file_set_string(s->seen, rpc_key, "digest", digest);
  return persist_seen(s);
}
static const struct { const gchar *method; SignerGateOp op; guint min, max; } methods[] = {
  {"connect", SIGNER_GATE_CONNECT, 1, 4}, {"get_public_key", SIGNER_GATE_GET_PUBLIC_KEY, 0, 0},
  {"sign_event", SIGNER_GATE_SIGN_EVENT, 1, 1}, {"nip04_encrypt", SIGNER_GATE_NIP04_ENCRYPT, 2, 2},
  {"nip04_decrypt", SIGNER_GATE_NIP04_DECRYPT, 2, 2}, {"nip44_encrypt", SIGNER_GATE_NIP44_ENCRYPT, 2, 2},
  {"nip44_decrypt", SIGNER_GATE_NIP44_DECRYPT, 2, 2}, {"ping", SIGNER_GATE_PING, 0, 0},
  {"get_relays", SIGNER_GATE_GET_RELAYS, 0, 0}
};
static void request_received(GNostrNip46Bunker *bunker, NostrNip46BunkerRequest *request, gpointer data) {
  (void)bunker;
  BunkerService *s = data;
  const NostrNip46Request *rpc = nostr_nip46_bunker_request_rpc(request);
  const gchar *client = nostr_nip46_bunker_request_client(request);
  const gchar *event_id = nostr_nip46_bunker_request_event_id(request);
  gint64 created = nostr_nip46_bunker_request_created_at(request), now = time(NULL);
#define REJECT(code) do { nostr_nip46_bunker_request_complete(request, NULL, code); return; } while (0)
  if (s->closing || !s->bunker) REJECT("cancelled");
  if (created < now - FRESHNESS || created > now + FRESHNESS) REJECT("stale_request");
  guint index;
  for (index = 0; index < G_N_ELEMENTS(methods); index++) if (!strcmp(rpc->method, methods[index].method)) break;
  if (index == G_N_ELEMENTS(methods)) REJECT("unsupported_method");
  if (rpc->n_params < methods[index].min || rpc->n_params > methods[index].max) REJECT("invalid_params");
  if (strcmp(rpc->method, "connect") && !contains(s->allowed_methods, rpc->method)) REJECT("unsupported_method");
  g_autofree gchar *identity = NULL, *claimed_name = NULL;
  if (!strcmp(rpc->method, "connect") && rpc->n_params > 3 && rpc->params[3] && *rpc->params[3]) {
    g_autoptr(JsonParser) metadata = json_parser_new();
    if (strlen(rpc->params[3]) > 4096 || !json_parser_load_from_data(metadata, rpc->params[3], -1, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(metadata))) REJECT("invalid_params");
    JsonNode *name = json_object_get_member(json_node_get_object(json_parser_get_root(metadata)), "name");
    if (name) {
      if (!JSON_NODE_HOLDS_VALUE(name) || json_node_get_value_type(name) != G_TYPE_STRING) REJECT("invalid_params");
      const gchar *text = json_node_get_string(name);
      if (!text || strlen(text) > 160 || !g_utf8_validate(text, -1, NULL)) REJECT("invalid_params");
      for (const gchar *p = text; *p; p = g_utf8_next_char(p)) if (g_unichar_iscntrl(g_utf8_get_char(p))) REJECT("invalid_params");
      claimed_name = g_strdup(text);
    }
  }
  gboolean is_paired = paired(s, client, &identity);
  if (!strcmp(rpc->method, "connect")) {
    if (!rpc->params || g_strcmp0(rpc->params[0], s->transport_pubkey)) REJECT("invalid_transport");
    /* Token admission is checked below after deduplication; possession never
     * marks a client paired and a reserved offer cannot move to another peer. */
  } else if (!is_paired) REJECT("not_paired");
  if (!identity || !*identity) { g_free(identity); identity = g_strdup(s->identity_npub); }
  PairOffer *offer = NULL;
  if (!strcmp(rpc->method, "connect") && !is_paired) {
    GHashTableIter offers; gpointer value; g_hash_table_iter_init(&offers, s->offers);
    while (g_hash_table_iter_next(&offers, NULL, &value)) {
      PairOffer *candidate = value;
      if (candidate->outbound || candidate->terminal || candidate->consumed || candidate->generation != s->generation ||
          candidate->deadline <= g_get_monotonic_time() || rpc->n_params < 2) continue;
      /* Constant-time comparison of the fixed-length URL-safe token. */
      const gchar *token = candidate->secret.ptr;
      if (strlen(rpc->params[1]) != 43) continue;
      guint diff = 0; for (guint j = 0; j < 43; j++) diff |= (guint8)token[j] ^ (guint8)rpc->params[1][j];
      if (diff) continue;
      if (candidate->client && strcmp(candidate->client, client)) REJECT("pairing_reserved");
      offer = candidate; break;
    }
    if (!offer && rpc->n_params > 1 && *rpc->params[1]) REJECT("invalid_pairing_token");
    if (!offer && !contains(s->allowed_pubkeys, client)) REJECT("not_paired");
    if (offer) { g_free(identity); identity = g_strdup(offer->identity); }
  }
  if (!identity || !*identity) REJECT("identity_required");
  SignerApprovalService *approval = signer_get_approval_service();
  if (!approval) REJECT("signer_unavailable");
  if (signer_approval_service_is_paused(approval)) REJECT("paused");
  if (!signer_permissions_reload(signer_approval_service_permissions(approval), NULL)) REJECT("policy_unavailable");
  guint64 generation = signer_approval_service_generation(approval);
  guint64 revision = signer_permissions_revision(signer_approval_service_permissions(approval));
  prune(s);
  g_autofree gchar *rpc_join = g_strdup_printf("%s:%s", client, rpc->id);
  g_autofree gchar *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, rpc_join, -1);
  GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(hash, (const guchar *)rpc->method, strlen(rpc->method) + 1);
  for (gsize i = 0; i < rpc->n_params; i++) {
    if (!rpc->params[i]) { g_checksum_free(hash); REJECT("invalid_params"); }
    g_checksum_update(hash, (const guchar *)rpc->params[i], strlen(rpc->params[i]) + 1);
  }
  g_autofree gchar *digest = g_strdup(g_checksum_get_string(hash)); g_checksum_free(hash);
  const gchar *pinned_identity = methods[index].op == SIGNER_GATE_PING || methods[index].op == SIGNER_GATE_GET_RELAYS ? NULL : identity;
  Rpc *r = g_hash_table_lookup(s->rpcs, key);
  if (r) {
    if (strcmp(r->digest, digest)) REJECT("duplicate_rpc_changed_params");
    if (r->completed) {
      /* A result is not a continuing grant. Re-evaluate the exact pinned
       * scope without resolving an account or submitting another key use.
       * Paired CONNECT is admission-only; its ack conveys no signing scope. */
      if (!r->error && strcmp(r->method, "connect")) {
        gint kind = -1;
        const gchar *operation = r->method;
        if (!strcmp(operation, "sign_event")) {
          operation = "event";
          g_autoptr(JsonParser) event = json_parser_new();
          if (!json_parser_load_from_data(event, rpc->params[0], -1, NULL) ||
              !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(event))) REJECT("invalid_params");
          JsonObject *object = json_node_get_object(json_parser_get_root(event));
          if (!json_object_has_member(object, "kind")) REJECT("invalid_params");
          kind = json_object_get_int_member(object, "kind");
        }
        g_autofree gchar *principal = g_strdup_printf("nip46:%s", client);
        SignerPermissionResult permission = signer_permissions_evaluate(
            signer_approval_service_permissions(approval), principal, r->identity,
            operation, kind, FALSE, NULL);
        if (permission == SIGNER_PERMISSION_ERROR) REJECT("policy_unavailable");
        if (permission != SIGNER_PERMISSION_ALLOW) REJECT(ORG_NOSTR_SIGNER_ERR_APPROVAL);
      }
      if (r->generation != generation || g_strcmp0(r->identity, pinned_identity) ||
          (strcmp(r->method, "connect") && (r->revision != revision ||
           !signer_permissions_revision_is_current(signer_approval_service_permissions(approval), r->revision)))) REJECT("replay_policy_changed");
      nostr_nip46_bunker_request_complete(request, r->result, r->error); return;
    }
    if (r->requests->len >= 32) REJECT("rate_limited");
    if (!remember_seen(s, event_id, key, digest, created)) REJECT("replay_store_unavailable");
    g_ptr_array_add(r->requests, nostr_nip46_bunker_request_ref(request)); return;
  }
  if (g_key_file_has_group(s->seen, key)) {
    g_autofree gchar *previous = g_key_file_get_string(s->seen, key, "digest", NULL);
    REJECT(g_strcmp0(previous, digest) ? "duplicate_rpc_changed_params" : "replayed_request");
  }
  if (!event_id || g_key_file_has_group(s->seen, event_id)) REJECT("replayed_request");
  if (g_hash_table_size(s->rpcs) >= CACHE_MAX) {
    GHashTableIter it; gpointer v; Rpc *oldest = NULL;
    g_hash_table_iter_init(&it, s->rpcs);
    while (g_hash_table_iter_next(&it, NULL, &v)) { Rpc *c = v; if (c->completed && (!oldest || c->expires < oldest->expires)) oldest = c; }
    if (!oldest) REJECT("rate_limited");
    g_hash_table_remove(s->rpcs, oldest->key);
  }
  if (!remember_seen(s, event_id, key, digest, created)) REJECT("replay_store_unavailable");
  r = g_new0(Rpc, 1); r->pairing_tier = -1; r->service = s; r->key = g_strdup(key); r->client = g_strdup(client);
  r->digest = g_strdup(digest); r->method = g_strdup(rpc->method); r->identity = g_strdup(pinned_identity);
  r->generation = generation; r->revision = revision;
  if (offer) {
    if (!offer->client) offer->client = g_strdup(client);
    r->offer_id = g_strdup(offer->id);
  }
  r->requests = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_nip46_bunker_request_unref);
  g_ptr_array_add(r->requests, nostr_nip46_bunker_request_ref(request));
  g_hash_table_insert(s->rpcs, g_strdup(key), r);
  /* Only this callback from the core verified/decrypted envelope may create a
   * NIP-46 principal. Connect permissions are display hints, never authority. */
  g_autoptr(SignerCaller) who = signer_caller_for_nip46(client);
  if (!who) { complete(r, NULL, "invalid_client"); return; }
  SignerGateContext context = {.transport = SIGNER_TRANSPORT_NIP46, .no_approval_ui = s->no_ui,
      .existing_pairing = is_paired && !strcmp(rpc->method, "connect"),
      .pairing_relays = (const gchar *const *)(s->configured_relays ? s->configured_relays : s->relays), .claimed_name = claimed_name,
      .deadline = offer ? offer->deadline : 0};
  const gchar *a = "", *b = "";
  g_autofree gchar *relay_map = NULL;
  if (!strcmp(rpc->method, "get_relays")) { relay_map = configured_relays_token(s); a = relay_map; }
  else if (!strcmp(rpc->method, "sign_event")) a = rpc->params[0];
  else if (rpc->n_params == 2 && g_str_has_prefix(rpc->method, "nip")) { b = rpc->params[0]; a = rpc->params[1]; }
  else if (!strcmp(rpc->method, "connect") && rpc->n_params > 2) a = rpc->params[2];
  r->handle = signer_gate_submit_full(who, who->principal, methods[index].op, a, b, identity, &context, gate_reply, r);
#undef REJECT
}
static void relay_state(GNostrNip46Bunker *bunker, const gchar *relay, const gchar *state, gpointer data) {
  (void)bunker; BunkerService *s = data;
  if (!contains(s->relays, relay)) return;
  g_hash_table_replace(s->relay_states, g_strdup(relay), g_strdup(state));
  guint ready = 0;
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->relay_states);
  while (g_hash_table_iter_next(&it, NULL, &value)) if (!strcmp(value, "EOSE")) ready++;
  if (ready) set_state(s, ready == g_strv_length(s->relays) ? BUNKER_STATE_RUNNING : BUNKER_STATE_DEGRADED, NULL);
  else set_state(s, BUNKER_STATE_OFFLINE, "No usable relay subscription");
  deliver_offers(s);
}
BunkerService *bunker_service_new(void) {
  BunkerService *s = g_new0(BunkerService, 1); s->refs = 1;
  s->offers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, offer_free);
  s->connections = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)bunker_connection_free);
  s->rpcs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, rpc_free);
  s->relay_states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  s->seen = g_key_file_new(); s->seen_healthy = TRUE;
  s->seen_path = g_build_filename(g_get_user_config_dir(), "grotto", "bunker-seen.ini", NULL);
  if (nostrc_profile_guard_refuses_path(s->seen_path)) s->seen_healthy = FALSE;
  else if (g_file_test(s->seen_path, G_FILE_TEST_EXISTS)) {
    s->seen_healthy = g_key_file_load_from_file(s->seen, s->seen_path, G_KEY_FILE_NONE, NULL) &&
        g_key_file_get_integer(s->seen, "meta", "version", NULL) == 1;
    gsize count; g_auto(GStrv) groups = g_key_file_get_groups(s->seen, &count);
    if (count > SEEN_MAX) s->seen_healthy = FALSE;
    for (gsize i = 0; i < count; i++) {
      if (!strcmp(groups[i], "meta")) continue;
      g_autoptr(GError) error = NULL;
      gint64 expiry = g_key_file_get_int64(s->seen, groups[i], "expires", &error);
      if (!hex64(groups[i]) || error || expiry <= 0 || expiry > (gint64)time(NULL) + 2 * FRESHNESS) s->seen_healthy = FALSE;
      if (g_key_file_has_key(s->seen, groups[i], "digest", NULL)) {
        g_autofree gchar *digest = g_key_file_get_string(s->seen, groups[i], "digest", NULL);
        if (!hex64(digest)) s->seen_healthy = FALSE;
      }
    }
  } else g_key_file_set_integer(s->seen, "meta", "version", 1);
  return s;
}
void bunker_service_free(BunkerService *s) { if (s) { s->closing = TRUE; bunker_service_stop(s); service_unref(s); } }
typedef struct { BunkerService *service; guint generation; nostr_secure_buf key; gint result; } Start;
static void start_free(gpointer data) {
  Start *job = data; secure_free(&job->key); service_unref(job->service); g_free(job);
}
static void load_transport(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
  (void)source; (void)cancel; Start *job = data;
  job->result = nostr_nip55l_bunker_transport_load(job->key.ptr);
  g_task_return_boolean(task, TRUE);
}
static void transport_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; (void)data;
  Start *job = g_task_get_task_data(G_TASK(result)); BunkerService *s = job->service;
  if (job->generation != s->generation || s->closing) return;
  s->starting = FALSE;
  if (job->result) {
    set_state(s, BUNKER_STATE_ERROR, job->result == NOSTR_SIGNER_ERROR_NOT_FOUND ?
        "Transport key missing; reset bunker endpoint" : "Transport key unavailable"); return;
  }
  s->bunker = gnostr_nip46_bunker_new();
  gboolean installed = gnostr_nip46_bunker_set_transport_key(s->bunker, job->key.ptr, NULL) && gnostr_nip46_bunker_enable_requests(s->bunker);
  secure_wipe(job->key.ptr, job->key.len);
  if (!installed) { g_clear_object(&s->bunker); set_state(s, BUNKER_STATE_ERROR, "Invalid transport key"); return; }
  char *pk = NULL;
  nostr_nip46_bunker_get_transport_pubkey(gnostr_nip46_bunker_get_session(s->bunker), &pk);
  g_free(s->transport_pubkey); s->transport_pubkey = g_strdup(pk); free(pk);
  g_signal_connect(s->bunker, "request", G_CALLBACK(request_received), s);
  g_signal_connect(s->bunker, "relay-state", G_CALLBACK(relay_state), s);
  /* Opt-in core listen registers/dials asynchronously; no owning-thread wait. */
  if (!gnostr_nip46_bunker_listen(s->bunker, (const gchar *const *)s->relays, g_strv_length(s->relays), NULL)) {
    gnostr_nip46_bunker_stop(s->bunker); g_clear_object(&s->bunker);
    set_state(s, BUNKER_STATE_ERROR, "Failed to start bunker listener");
  }
}
gboolean bunker_service_start(BunkerService *s, const gchar *const *relays, const gchar *identity) {
  if (!s || s->closing) return FALSE;
  /* Initial client-URI approval may be waiting for its first transport. Keep
   * that offer alive; ordinary explicit starts retain stop/start semantics. */
  if (s->bunker || s->starting || !g_hash_table_size(s->offers)) bunker_service_stop(s);
  if (identity && *identity && accounts_store_is_watch_only(accounts_store_get_default(), identity)) {
    set_state(s, BUNKER_STATE_ERROR, "A signable identity is required"); return FALSE;
  }
  if (!relays || !relays[0]) { set_state(s, BUNKER_STATE_ERROR, "No bunker relays configured"); return FALSE; }
  if (!s->seen_healthy) { set_state(s, BUNKER_STATE_ERROR, "Replay protection unavailable"); return FALSE; }
  guint relay_count = 0;
  for (; relays[relay_count]; relay_count++) {
    if (relay_count >= 64 || strlen(relays[relay_count]) > 2048) {
      set_state(s, BUNKER_STATE_ERROR, "Relay configuration exceeds bounds"); return FALSE;
    }
    g_autoptr(GUri) uri = g_uri_parse(relays[relay_count], G_URI_FLAGS_NONE, NULL);
    const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL, *host = uri ? g_uri_get_host(uri) : NULL;
    g_autofree gchar *lower_host = host ? g_ascii_strdown(host, -1) : NULL;
    gboolean loopback = lower_host && (!strcmp(lower_host, "127.0.0.1") || !strcmp(lower_host, "::1") || !strcmp(lower_host, "localhost"));
    if (!uri || !host || !*host || g_uri_get_userinfo(uri) || g_uri_get_fragment(uri) ||
        (!g_strcmp0(scheme, "ws") ? !loopback : g_strcmp0(scheme, "wss")) || g_str_has_suffix(lower_host, ".onion")) {
      set_state(s, BUNKER_STATE_ERROR, "Invalid or unsupported relay URL"); return FALSE;
    }
  }
  g_autofree gchar *canonical = NULL;
  if (nostr_nip55l_relays_from_list(relays, relay_count, &canonical)) {
    set_state(s, BUNKER_STATE_ERROR, "Invalid or unsupported relay URL"); return FALSE;
  }
  g_autoptr(JsonParser) parsed = json_parser_new();
  if (!json_parser_load_from_data(parsed, canonical, -1, NULL) || !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parsed))) {
    set_state(s, BUNKER_STATE_ERROR, "Invalid relay configuration"); return FALSE;
  }
  JsonArray *urls = json_node_get_array(json_parser_get_root(parsed));
  g_auto(GStrv) candidate_relays = g_new0(gchar *, json_array_get_length(urls) + 1);
  for (guint i = 0; i < json_array_get_length(urls); i++) candidate_relays[i] = g_strdup(json_array_get_string_element(urls, i));
  g_autofree gchar *candidate_identity = g_strdup(identity ? identity : "");
  g_strfreev(s->relays); s->relays = g_steal_pointer(&candidate_relays);
  g_free(s->identity_npub); s->identity_npub = g_steal_pointer(&candidate_identity);
  Start *job = g_new0(Start, 1); job->service = s; job->generation = s->generation;
  job->key = secure_alloc(32);
  if (!job->key.ptr) { g_free(job); set_state(s, BUNKER_STATE_ERROR, "Transport key allocation failed"); return FALSE; }
  g_atomic_int_inc(&s->refs);
  g_autoptr(GTask) task = g_task_new(NULL, NULL, transport_ready, NULL);
  g_task_set_task_data(task, job, start_free);
  s->starting = TRUE; set_state(s, BUNKER_STATE_STARTING, NULL);
  g_task_run_in_thread(task, load_transport);
  return TRUE;
}
void bunker_service_stop(BunkerService *s) {
  if (!s) return;
  if (s->bunker) set_state(s, BUNKER_STATE_STOPPING, NULL);
  s->generation++;
  GHashTableIter offers; gpointer value_offer; g_hash_table_iter_init(&offers, s->offers);
  while (g_hash_table_iter_next(&offers, NULL, &value_offer)) signer_gate_cancel(((PairOffer *)value_offer)->handle);
  g_hash_table_remove_all(s->offers);
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->rpcs);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Rpc *r = value;
    if (!r->completed) { signer_gate_cancel(r->handle); if (!r->completed) complete(r, NULL, "cancelled"); }
  }
  if (s->bunker) {
    g_signal_handlers_disconnect_by_data(s->bunker, s);
    gnostr_nip46_bunker_stop(s->bunker);
    g_clear_object(&s->bunker);
  }
  s->starting = FALSE;
  g_hash_table_remove_all(s->connections); g_hash_table_remove_all(s->rpcs); g_hash_table_remove_all(s->relay_states);
  set_state(s, BUNKER_STATE_STOPPED, NULL);
}
gboolean bunker_service_reset_replay(BunkerService *s) {
  if (!s || s->bunker || s->starting) return FALSE;
  g_key_file_unref(s->seen); s->seen = g_key_file_new();
  g_key_file_set_integer(s->seen, "meta", "version", 1);
  s->seen_healthy = TRUE; return persist_seen(s);
}
gboolean bunker_service_replay_healthy(BunkerService *s) { return s && s->seen_healthy; }
gboolean bunker_service_restart(BunkerService *s) {
  if (!s) return FALSE;
  g_auto(GStrv) relays = g_strdupv(s->relays);
  g_autofree gchar *identity = g_strdup(s->identity_npub);
  return bunker_service_start(s, (const gchar *const *)relays, identity);
}
void bunker_service_fail_configuration(BunkerService *s) {
  if (s) { bunker_service_stop(s); set_state(s, BUNKER_STATE_ERROR, "Invalid desired bunker configuration"); }
}
void bunker_service_fail_transport(BunkerService *s) {
  if (s) set_state(s, BUNKER_STATE_ERROR, "Transport key reset failed");
}
BunkerState bunker_service_get_state(BunkerService *s) { return s ? s->state : BUNKER_STATE_STOPPED; }
const gchar *bunker_service_get_error(BunkerService *s) { return s ? s->error_message : NULL; }
const gchar *bunker_service_get_transport_pubkey(BunkerService *s) { return s ? s->transport_pubkey : NULL; }
gchar *bunker_service_get_bunker_uri(BunkerService *s, const gchar *secret) {
  if (!s || !s->bunker || s->state != BUNKER_STATE_RUNNING) return NULL;
  gchar **relays = s->configured_relays ? s->configured_relays : s->relays;
  if (!relays || !relays[0]) return NULL;
  gchar *uri = NULL;
  gnostr_nip46_bunker_issue_uri(s->bunker, s->transport_pubkey, (const gchar *const *)relays, g_strv_length(relays), secret, &uri, NULL);
  return uri;
}
gboolean bunker_service_disconnect_client(BunkerService *s, const gchar *client) {
  if (!s || !hex64(client)) return FALSE;
  g_autofree gchar *principal = g_strdup_printf("nip46:%s", client);
  signer_gate_connection_closed(principal);
  GList *offers = g_hash_table_get_keys(s->offers);
  for (GList *it = offers; it; it = it->next) {
    PairOffer *o = g_hash_table_lookup(s->offers, it->data);
    if (o && !g_strcmp0(o->client, client)) bunker_service_cancel_pairing(s, it->data);
  }
  g_list_free(offers);
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->rpcs);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Rpc *r = value;
    if (strcmp(r->client, client)) continue;
    /* Also fence a connect ack queued after the gate has terminalized, when
     * there is no longer a live gate continuation to cancel. */
    if (!r->completed) { signer_gate_cancel(r->handle); if (!r->completed) complete(r, NULL, "cancelled"); }
    g_hash_table_iter_remove(&it);
  }
  return g_hash_table_remove(s->connections, client);
}
GPtrArray *bunker_service_list_connections(BunkerService *s) {
  if (!s) return NULL;
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)bunker_connection_free);
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->connections);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    BunkerConnection *c = value, *copy = g_new0(BunkerConnection, 1); *copy = *c;
    copy->client_pubkey = g_strdup(c->client_pubkey); copy->app_name = g_strdup(c->app_name); copy->permissions = g_strdupv(c->permissions);
    g_ptr_array_add(out, copy);
  }
  return out;
}
void bunker_service_set_configured_relays(BunkerService *s, const gchar *const *relays) {
  if (!s) return;
  g_strfreev(s->configured_relays); s->configured_relays = g_strdupv((gchar **)relays);
}
void bunker_service_set_allowed_methods(BunkerService *s, const gchar *const *values) {
  if (!s) return;
  g_strfreev(s->allowed_methods); s->allowed_methods = g_strdupv((gchar **)values);
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->rpcs);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Rpc *r = value;
    if (!r->completed && strcmp(r->method, "connect") && !contains(s->allowed_methods, r->method)) {
      signer_gate_cancel(r->handle); if (!r->completed) complete(r, NULL, "unsupported_method");
    }
  }
}
void bunker_service_set_allowed_pubkeys(BunkerService *s, const gchar *const *values) {
  if (!s) return;
  g_strfreev(s->allowed_pubkeys); s->allowed_pubkeys = g_strdupv((gchar **)values);
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->rpcs);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Rpc *r = value;
    if (!r->completed && !strcmp(r->method, "connect") && !contains(s->allowed_pubkeys, r->client) && !paired(s, r->client, NULL)) {
      signer_gate_cancel(r->handle); if (!r->completed) complete(r, NULL, "cancelled");
    }
  }
}
void bunker_service_set_state_callback(BunkerService *s, BunkerStateChangedCb cb, gpointer data) { if (s) { s->state_cb = cb; s->state_cb_ud = data; } }
void bunker_service_set_connection_callback(BunkerService *s, BunkerConnectionCb cb, gpointer data) { if (s) { s->conn_cb = cb; s->conn_cb_ud = data; } }
void bunker_service_set_client_lookup(BunkerService *s, BunkerClientLookupCb cb, gpointer data) { if (s) { s->lookup = cb; s->lookup_ud = data; } }
void bunker_service_set_no_approval_ui(BunkerService *s, gboolean no_ui) { if (s) s->no_ui = no_ui; }
/* Retained source compatibility, deliberately not live authorisation paths. */
void bunker_service_set_auto_approve_kinds(BunkerService *s, const gchar *const *kinds) { (void)s; (void)kinds; }
void bunker_service_set_authorize_callback(BunkerService *s, BunkerAuthorizeCb cb, gpointer data) { (void)s; (void)cb; (void)data; }
void bunker_service_authorize_response(BunkerService *s, const gchar *id, gboolean allow) { (void)s; (void)id; (void)allow; }
void bunker_service_create_client_session(BunkerService *s, const gchar *pk, const gchar *name, gboolean persistent, gint64 ttl) { (void)s; (void)pk; (void)name; (void)persistent; (void)ttl; }
void bunker_service_set_pairing_commit(BunkerService *s, BunkerPairingCommitCb cb, gpointer data) {
  if (s) { s->commit = cb; s->commit_ud = data; }
}
static gboolean pairing_error(GError **error, GIOErrorEnum code) {
  g_set_error_literal(error, G_IO_ERROR, code, "Pairing unavailable; check the identity and bunker relay status"); return FALSE;
}
gboolean bunker_service_cancel_pairing(BunkerService *s, const gchar *id) {
  PairOffer *o = s && id ? g_hash_table_lookup(s->offers, id) : NULL;
  if (!o && s && id) {
    GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->offers);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
      PairOffer *candidate = value;
      if (!g_strcmp0(candidate->request_id, id)) { o = candidate; break; }
    }
  }
  if (!o) return FALSE;
  id = o->id;
  o->terminal = TRUE; signer_gate_cancel(o->handle);
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->rpcs);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Rpc *r = value;
    if (!g_strcmp0(r->offer_id, id) && !r->completed) {
      signer_gate_cancel(r->handle); if (!r->completed) complete(r, NULL, "pairing_cancelled");
    }
  }
  return g_hash_table_remove(s->offers, id);
}
gboolean bunker_service_pairing_tier(BunkerService *s, const gchar *client, gint tier) {
  if (!s || tier < SIGNER_TRUST_LOW || tier > SIGNER_TRUST_FULL) return FALSE;
  gboolean found = FALSE; GHashTableIter it; gpointer value;
  g_hash_table_iter_init(&it, s->offers);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    PairOffer *o = value;
    if (!o->terminal && !o->consumed && !g_strcmp0(o->client, client)) { o->tier = tier; found = TRUE; }
  }
  g_hash_table_iter_init(&it, s->rpcs);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Rpc *r = value;
    if (!r->completed && !strcmp(r->method, "connect") && !g_strcmp0(r->client, client)) { r->pairing_tier = tier; found = TRUE; }
  }
  return found;
}
void bunker_service_invalidate_pairings(BunkerService *s) {
  if (!s) return;
  GList *keys = g_hash_table_get_keys(s->offers);
  for (GList *it = keys; it; it = it->next) bunker_service_cancel_pairing(s, it->data);
  g_list_free(keys);
}
static gboolean offer_expired(gpointer data) {
  PairOffer *o = data; bunker_service_cancel_pairing(o->service, o->id); return G_SOURCE_REMOVE;
}
static PairOffer *offer_new(BunkerService *s, const gchar *identity, const gchar *label, GError **error) {
  SignerApprovalService *approval = signer_get_approval_service();
  if (!s || s->closing || s->no_ui || !s->seen_healthy || !approval || signer_approval_service_is_paused(approval)) {
    pairing_error(error, G_IO_ERROR_NOT_CONNECTED); return NULL;
  }
  if (g_hash_table_size(s->offers) >= 4) { pairing_error(error, G_IO_ERROR_BUSY); return NULL; }
  guint8 pubkey[32]; char *npub = NULL;
  gboolean valid = identity && (hex64(identity) ? nostr_hex2bin(pubkey, identity, sizeof(pubkey)) : !nostr_nip19_decode_npub(identity, pubkey));
  if (!valid || nostr_nip19_encode_npub(pubkey, &npub) || accounts_store_is_watch_only(accounts_store_get_default(), identity) ||
      !label || strlen(label) > 160 || !g_utf8_validate(label, -1, NULL)) {
    free(npub); pairing_error(error, G_IO_ERROR_INVALID_ARGUMENT); return NULL;
  }
  PairOffer *o = g_new0(PairOffer, 1); o->tier = -1; o->service = s;
  o->id = g_uuid_string_random(); o->identity = g_strdup(npub); free(npub);
  o->label = g_strdup(label); o->generation = s->generation;
  o->security_generation = signer_approval_service_generation(approval);
  o->delivery_cancel = nostr_nip46_cancel_handle_new();
  if (!o->delivery_cancel) { offer_free(o); pairing_error(error, G_IO_ERROR_FAILED); return NULL; }
  o->deadline = g_get_monotonic_time() + 300 * G_USEC_PER_SEC;
  o->expiry = g_timeout_source_new_seconds(300);
  g_source_set_callback(o->expiry, offer_expired, o, NULL);
  g_source_attach(o->expiry, g_main_context_get_thread_default());
  g_hash_table_insert(s->offers, g_strdup(o->id), o); return o;
}
GVariant *bunker_service_create_pairing(BunkerService *s, const gchar *identity, const gchar *label, GError **error) {
  if (!s || s->state != BUNKER_STATE_RUNNING) { pairing_error(error, G_IO_ERROR_NOT_CONNECTED); return NULL; }
  PairOffer *o = offer_new(s, identity, label, error); if (!o) return NULL;
  nostr_secure_buf random = secure_alloc(32); o->secret = secure_alloc(44);
  if (!random.ptr || !o->secret.ptr || RAND_bytes(random.ptr, 32) != 1) {
    secure_free(&random); bunker_service_cancel_pairing(s, o->id); pairing_error(error, G_IO_ERROR_FAILED); return NULL;
  }
  gchar *encoded = g_base64_encode(random.ptr, 32); secure_free(&random);
  for (guint i = 0; i < 43; i++) ((gchar *)o->secret.ptr)[i] = encoded[i] == '+' ? '-' : encoded[i] == '/' ? '_' : encoded[i];
  ((gchar *)o->secret.ptr)[43] = 0; wipe(encoded);
  gchar *uri = bunker_service_get_bunker_uri(s, o->secret.ptr);
  if (!uri) { bunker_service_cancel_pairing(s, o->id); pairing_error(error, G_IO_ERROR_FAILED); return NULL; }
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "pairing_id", g_variant_new_string(o->id));
  g_variant_builder_add(&b, "{sv}", "uri", g_variant_new_string(uri)); wipe(uri);
  g_variant_builder_add(&b, "{sv}", "expires_at", g_variant_new_uint64(time(NULL) + 300));
  return g_variant_ref_sink(g_variant_builder_end(&b));
}
static void deliver_offers(BunkerService *s) {
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->offers);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    PairOffer *o = value;
    if (!o->outbound || !o->approved || o->terminal || o->deadline <= g_get_monotonic_time()) continue;
    SignerApprovalService *approval = signer_get_approval_service();
    if (!approval || signer_approval_service_is_paused(approval) || o->security_generation != signer_approval_service_generation(approval) || !paired(s, o->client, NULL)) {
      o->terminal = TRUE; secure_free(&o->secret); continue;
    }
    gboolean ready = FALSE;
    for (guint i = 0; o->relays && o->relays[i]; i++)
      if (!g_strcmp0(g_hash_table_lookup(s->relay_states, o->relays[i]), "EOSE")) ready = TRUE;
    if (!ready) continue;
    if (!nostr_nip46_bunker_connect_response(gnostr_nip46_bunker_get_session(s->bunker), o->client, o->secret.ptr, o->delivery_cancel)) {
      o->consumed = o->terminal = TRUE; secure_free(&o->secret);
    }
  }
}
static gboolean outbound_delivered(gpointer data) {
  PairOffer *o = data; BunkerService *s = o->service;
  g_clear_pointer(&o->delivery, g_source_unref);
  SignerApprovalService *approval = signer_get_approval_service();
  if (o->terminal || o->generation != s->generation || o->deadline <= g_get_monotonic_time() ||
      !approval || signer_approval_service_is_paused(approval) || o->security_generation != signer_approval_service_generation(approval)) {
    o->terminal = TRUE; secure_free(&o->secret); return G_SOURCE_REMOVE;
  }
  o->approved = TRUE;
  if (!s->commit || !s->commit(o->client, o->identity, o->label, (const gchar *const *)o->relays, o->tier, s->commit_ud)) {
    o->terminal = TRUE; secure_free(&o->secret); return G_SOURCE_REMOVE;
  }
  deliver_offers(s); return G_SOURCE_REMOVE;
}
static void outbound_reply(gpointer data, const gchar *error, const gchar *message, const gchar *result) {
  (void)message; (void)result; PairOffer *o = data;
  if (error || o->terminal) { o->terminal = TRUE; secure_free(&o->secret); return; }
  o->delivery = g_idle_source_new();
  g_source_set_callback(o->delivery, outbound_delivered, o, NULL);
  g_source_attach(o->delivery, g_main_context_get_thread_default());
}
gchar *bunker_service_accept_connect_uri(BunkerService *s, const gchar *text, const gchar *identity, GError **error) {
  NostrNip46ConnectURI uri;
  if (!grotto_connect_parse(text, &uri, error)) return NULL;
  PairOffer *o = offer_new(s, identity, "", error);
  if (!o) { grotto_connect_clear(&uri); return NULL; }
  o->outbound = TRUE; o->client = g_strdup(uri.client_pubkey_hex);
  o->name = g_strdup(uri.name); o->hints = g_strdup(uri.perms_csv);
  o->relays = g_new0(gchar *, uri.n_relays + 1);
  for (gsize i = 0; i < uri.n_relays; i++) o->relays[i] = g_strdup(uri.relays[i]);
  o->secret = secure_alloc(strlen(uri.secret) + 1);
  if (!o->secret.ptr) {
    grotto_connect_clear(&uri); bunker_service_cancel_pairing(s, o->id); pairing_error(error, G_IO_ERROR_FAILED); return NULL;
  }
  strcpy(o->secret.ptr, uri.secret); grotto_connect_clear(&uri);
  g_autoptr(SignerCaller) who = signer_caller_for_nip46(o->client);
  SignerGateContext context = {.transport = SIGNER_TRANSPORT_NIP46, .claimed_name = o->name,
      .pairing_relays = (const gchar *const *)o->relays, .deadline = o->deadline, .flow_id = o->id};
  o->handle = signer_gate_submit_full(who, who->principal, SIGNER_GATE_CONNECT, o->hints ? o->hints : "",
      o->id, o->identity, &context, outbound_reply, o);
  gchar *id = signer_gate_handle_request_id(o->handle); o->request_id = g_strdup(id);
  if (!id) { bunker_service_cancel_pairing(s, o->id); pairing_error(error, G_IO_ERROR_FAILED); }
  return id;
}
gboolean bunker_service_reconfigure(BunkerService *s, const gchar *const *relays, const gchar *identity) {
  if (!s || !grotto_relays_validate(relays, relays ? g_strv_length((gchar **)relays) : 0, NULL)) return FALSE;
  g_autofree gchar *canonical = NULL;
  if (nostr_nip55l_relays_from_list(relays, g_strv_length((gchar **)relays), &canonical)) return FALSE;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, canonical, -1, NULL)) return FALSE;
  JsonArray *array = json_node_get_array(json_parser_get_root(parser));
  g_auto(GStrv) normalized = g_new0(gchar *, json_array_get_length(array) + 1);
  for (guint i = 0; i < json_array_get_length(array); i++) normalized[i] = g_strdup(json_array_get_string_element(array, i));
  relays = (const gchar *const *)normalized;
  if (!s->bunker) {
    if (s->starting) {
      g_strfreev(s->relays); s->relays = g_strdupv((gchar **)relays); return TRUE;
    }
    return bunker_service_start(s, relays, identity);
  }
  if (nostr_nip46_bunker_reconfigure_relays(gnostr_nip46_bunker_get_session(s->bunker), relays, g_strv_length((gchar **)relays))) return FALSE;
  GHashTableIter requests; gpointer value; g_hash_table_iter_init(&requests, s->rpcs);
  while (g_hash_table_iter_next(&requests, NULL, &value)) {
    Rpc *r = value; if (r->completed) continue;
    for (guint i = r->requests->len; i > 0; i--) {
      NostrNip46BunkerRequest *request = r->requests->pdata[i - 1];
      const gchar *source = nostr_nip46_bunker_request_relay(request);
      if (source && !g_strv_contains(relays, source)) {
        nostr_nip46_bunker_request_complete(request, NULL, "relay_removed");
        g_ptr_array_remove_index(r->requests, i - 1);
      }
    }
    if (!r->requests->len) { signer_gate_cancel(r->handle); if (!r->completed) complete(r, NULL, "relay_removed"); }
  }
  g_strfreev(s->relays); s->relays = g_strdupv((gchar **)relays);
  GHashTableIter it; gpointer key; g_hash_table_iter_init(&it, s->relay_states);
  while (g_hash_table_iter_next(&it, &key, NULL)) if (!contains(s->relays, key)) g_hash_table_iter_remove(&it);
  guint ready = 0;
  for (guint i = 0; s->relays[i]; i++) if (!g_strcmp0(g_hash_table_lookup(s->relay_states, s->relays[i]), "EOSE")) ready++;
  set_state(s, ready ? (ready == g_strv_length(s->relays) ? BUNKER_STATE_RUNNING : BUNKER_STATE_DEGRADED) : BUNKER_STATE_STARTING, NULL);
  return TRUE;
}
GVariant *bunker_service_relay_status(BunkerService *s) {
  GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("a{ss}"));
  for (guint i = 0; s && s->relays && s->relays[i]; i++) {
    const gchar *state = g_hash_table_lookup(s->relay_states, s->relays[i]);
    g_variant_builder_add(&rows, "{ss}", s->relays[i], state ? state : "STARTING");
  }
  return g_variant_ref_sink(g_variant_builder_end(&rows));
}
gboolean bunker_service_handle_connect_uri(BunkerService *s, const gchar *uri) {
  (void)s; (void)uri; return FALSE; /* Legacy call cannot choose an identity or supply consent. */
}
