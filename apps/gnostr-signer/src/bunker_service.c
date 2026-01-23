/* bunker_service.c - NIP-46 bunker service implementation
 *
 * Integrates with the nip46 library for protocol handling.
 * Uses secure memory for handling sensitive data like signatures.
 * Includes rate limiting to prevent brute force attacks (nostrc-1g1).
 * Includes session management for client approval tracking (nostrc-09n).
 */
#include "bunker_service.h"
#include "accounts_store.h"
#include "secret_store.h"
#include "settings_manager.h"
#include "secure-memory.h"
#include "secure-mem.h"
#include "rate-limiter.h"
#include "client_session.h"

#include <nostr/nip46/nip46_bunker.h>
#include <nostr/nip46/nip46_uri.h>
#include <nostr/nip46/nip46_msg.h>
#include <nostr/nip19/nip19.h>
#include <keys.h>
#include <string.h>
#include <time.h>

struct _BunkerService {
  BunkerState state;
  gchar *error_message;

  /* NIP-46 session */
  NostrNip46Session *session;

  /* Identity */
  gchar *identity_npub;
  gchar *identity_pubkey_hex;

  /* Configuration */
  gchar **relays;
  gsize n_relays;
  gchar **allowed_methods;
  gchar **allowed_pubkeys;
  gchar **auto_approve_kinds;

  /* Active connections */
  GHashTable *connections;  /* client_pubkey -> BunkerConnection* */
  gchar *current_signing_client;  /* Current client making a sign request */

  /* Pending requests */
  GHashTable *pending_requests;  /* request_id -> BunkerSignRequest* */

  /* Callbacks */
  BunkerStateChangedCb state_cb;
  gpointer state_cb_ud;
  BunkerConnectionCb conn_cb;
  gpointer conn_cb_ud;
  BunkerAuthorizeCb auth_cb;
  gpointer auth_cb_ud;
};

void bunker_connection_free(BunkerConnection *conn) {
  if (!conn) return;
  g_free(conn->client_pubkey);
  g_free(conn->app_name);
  g_strfreev(conn->permissions);
  g_free(conn);
}

void bunker_sign_request_free(BunkerSignRequest *req) {
  if (!req) return;
  g_free(req->request_id);
  g_free(req->client_pubkey);
  g_free(req->method);
  g_free(req->event_json);
  g_free(req->preview);
  g_free(req);
}

static void set_state(BunkerService *bs, BunkerState state, const gchar *error) {
  if (!bs) return;

  bs->state = state;
  g_free(bs->error_message);
  bs->error_message = error ? g_strdup(error) : NULL;

  if (bs->state_cb) {
    bs->state_cb(state, error, bs->state_cb_ud);
  }
}

/* NIP-46 callbacks */
static int bunker_authorize_cb(const char *client_pubkey_hex,
                               const char *perms_csv,
                               void *user_data) {
  BunkerService *bs = (BunkerService*)user_data;
  if (!bs) return 0;

  /* Check rate limiting first (nostrc-1g1) */
  GnRateLimiter *limiter = gn_rate_limiter_get_default();
  guint remaining_seconds = 0;
  GnRateLimitStatus rate_status = gn_rate_limiter_check_client(limiter,
                                                                client_pubkey_hex,
                                                                &remaining_seconds);

  if (rate_status != GN_RATE_LIMIT_ALLOWED) {
    gchar *error_msg = gn_rate_limiter_format_error_message(rate_status, remaining_seconds);
    g_message("bunker: rejecting rate-limited client %s: %s", client_pubkey_hex, error_msg);
    g_free(error_msg);
    return 0;
  }

  /* Check allowed pubkeys */
  if (bs->allowed_pubkeys && bs->allowed_pubkeys[0]) {
    gboolean found = FALSE;
    for (gint i = 0; bs->allowed_pubkeys[i]; i++) {
      if (g_strcmp0(bs->allowed_pubkeys[i], client_pubkey_hex) == 0) {
        found = TRUE;
        break;
      }
    }
    if (!found) {
      /* Record failed attempt for rate limiting */
      gn_rate_limiter_record_client_attempt(limiter, client_pubkey_hex, FALSE);
      g_message("bunker: rejecting unauthorized client %s", client_pubkey_hex);
      return 0;
    }
  }

  /* Successful authorization - reset rate limit for this client */
  gn_rate_limiter_record_client_attempt(limiter, client_pubkey_hex, TRUE);

  /* Create connection entry */
  BunkerConnection *conn = g_new0(BunkerConnection, 1);
  conn->client_pubkey = g_strdup(client_pubkey_hex);
  conn->connected_at = (gint64)time(NULL);

  if (perms_csv && *perms_csv) {
    conn->permissions = g_strsplit(perms_csv, ",", -1);
  }

  g_hash_table_replace(bs->connections, g_strdup(client_pubkey_hex), conn);

  /* Store current client for sign callbacks */
  g_free(bs->current_signing_client);
  bs->current_signing_client = g_strdup(client_pubkey_hex);

  if (bs->conn_cb) {
    bs->conn_cb(conn, bs->conn_cb_ud);
  }

  g_message("bunker: authorized client %s", client_pubkey_hex);
  return 1;
}

static char *bunker_sign_cb(const char *event_json,
                            void *user_data) {
  BunkerService *bs = (BunkerService*)user_data;
  if (!bs || !event_json) return NULL;

  /* Check if auto-approve based on event kind */
  gint kind = 0;

  /* Simple kind extraction from JSON */
  const gchar *p = strstr(event_json, "\"kind\"");
  if (p) {
    p = strchr(p, ':');
    if (p) {
      p++;
      while (*p == ' ') p++;
      kind = (gint)g_ascii_strtoll(p, NULL, 10);
    }
  }

  /* Check auto-approve based on event kind */
  gboolean auto_approve = FALSE;
  if (bs->auto_approve_kinds) {
    for (gint i = 0; bs->auto_approve_kinds[i]; i++) {
      gint ak = (gint)g_ascii_strtoll(bs->auto_approve_kinds[i], NULL, 10);
      if (ak == kind) {
        auto_approve = TRUE;
        break;
      }
    }
  }

  /* Check for active client session (nostrc-09n) */
  if (!auto_approve && bs->current_signing_client) {
    GnClientSessionManager *sess_mgr = gn_client_session_manager_get_default();
    if (gn_client_session_manager_has_active_session(sess_mgr,
                                                      bs->current_signing_client,
                                                      bs->identity_npub)) {
      /* Active session exists - auto-approve and touch session */
      gn_client_session_manager_touch_session(sess_mgr,
                                               bs->current_signing_client,
                                               bs->identity_npub);
      auto_approve = TRUE;
      g_debug("bunker: auto-approved via active session for %s", bs->current_signing_client);
    }
  }

  if (!auto_approve && bs->auth_cb) {
    /* Create request for UI prompt */
    BunkerSignRequest *req = g_new0(BunkerSignRequest, 1);
    req->request_id = g_strdup_printf("bunker_%ld_%d", (long)time(NULL), g_random_int());
    req->client_pubkey = g_strdup(bs->current_signing_client);
    req->method = g_strdup("sign_event");
    req->event_json = g_strdup(event_json);
    req->event_kind = kind;

    /* Create preview */
    const gchar *content_start = strstr(event_json, "\"content\"");
    if (content_start) {
      content_start = strchr(content_start, ':');
      if (content_start) {
        content_start++;
        while (*content_start == ' ' || *content_start == '"') content_start++;
        const gchar *content_end = content_start;
        while (*content_end && *content_end != '"') content_end++;
        gsize len = MIN((gsize)(content_end - content_start), 100);
        req->preview = g_strndup(content_start, len);
      }
    }
    if (!req->preview) {
      req->preview = g_strdup_printf("Event kind %d", kind);
    }

    g_hash_table_insert(bs->pending_requests, g_strdup(req->request_id), req);

    /* Ask UI for approval */
    if (!bs->auth_cb(req, bs->auth_cb_ud)) {
      /* Denied */
      g_hash_table_remove(bs->pending_requests, req->request_id);
      return NULL;
    }
  }

  /* Sign the event using our identity
   * Note: signature is returned in secure memory to prevent leakage */
  gchar *signature = NULL;
  SecretStoreResult rc = secret_store_sign_event(event_json, bs->identity_npub, &signature);

  if (rc != SECRET_STORE_OK || !signature) {
    g_warning("bunker: sign failed: %d", rc);
    return NULL;
  }

  /* Build signed event JSON - for now just return signature
   * The nip46 library expects the full signed event
   *
   * Note: The caller (nip46 library) is responsible for freeing this.
   * We make a regular copy since the library may not use secure memory. */
  gchar *result = g_strdup(signature);

  /* Securely clear and free our secure copy */
  gn_secure_strfree(signature);

  return result;
}

BunkerService *bunker_service_new(void) {
  BunkerService *bs = g_new0(BunkerService, 1);
  bs->state = BUNKER_STATE_STOPPED;
  bs->connections = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)bunker_connection_free);
  bs->pending_requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify)bunker_sign_request_free);
  return bs;
}

void bunker_service_free(BunkerService *bs) {
  if (!bs) return;

  bunker_service_stop(bs);

  g_free(bs->error_message);
  g_free(bs->identity_npub);
  g_free(bs->identity_pubkey_hex);
  g_free(bs->current_signing_client);
  g_strfreev(bs->relays);
  g_strfreev(bs->allowed_methods);
  g_strfreev(bs->allowed_pubkeys);
  g_strfreev(bs->auto_approve_kinds);
  g_hash_table_destroy(bs->connections);
  g_hash_table_destroy(bs->pending_requests);
  g_free(bs);
}

gboolean bunker_service_start(BunkerService *bs,
                              const gchar *const *relays,
                              const gchar *identity) {
  if (!bs || !identity) return FALSE;

  if (bs->state == BUNKER_STATE_RUNNING) {
    return TRUE;
  }

  /* Check if identity is watch-only - cannot start bunker for watch-only accounts */
  AccountsStore *as = accounts_store_get_default();
  if (accounts_store_is_watch_only(as, identity)) {
    set_state(bs, BUNKER_STATE_ERROR, "Cannot start bunker for watch-only account (no private key)");
    g_warning("bunker: cannot start for watch-only identity %s", identity);
    return FALSE;
  }

  set_state(bs, BUNKER_STATE_STARTING, NULL);

  /* Store identity */
  g_free(bs->identity_npub);
  bs->identity_npub = g_strdup(identity);

  /* Convert npub to hex if needed */
  if (g_str_has_prefix(identity, "npub1")) {
    guint8 pk[32];
    if (nostr_nip19_decode_npub(identity, pk) == 0) {
      gchar *hex = g_malloc(65);
      for (gint i = 0; i < 32; i++) {
        g_snprintf(hex + i*2, 3, "%02x", pk[i]);
      }
      g_free(bs->identity_pubkey_hex);
      bs->identity_pubkey_hex = hex;
    }
  } else {
    g_free(bs->identity_pubkey_hex);
    bs->identity_pubkey_hex = g_strdup(identity);
  }

  /* Store relays */
  g_strfreev(bs->relays);
  if (relays) {
    gsize n = 0;
    while (relays[n]) n++;
    bs->n_relays = n;
    bs->relays = g_new0(gchar*, n + 1);
    for (gsize i = 0; i < n; i++) {
      bs->relays[i] = g_strdup(relays[i]);
    }
  }

  /* Create NIP-46 bunker session */
  NostrNip46BunkerCallbacks cbs = {
    .authorize_cb = bunker_authorize_cb,
    .sign_cb = bunker_sign_cb,
    .user_data = bs
  };

  bs->session = nostr_nip46_bunker_new(&cbs);
  if (!bs->session) {
    set_state(bs, BUNKER_STATE_ERROR, "Failed to create bunker session");
    return FALSE;
  }

  /* Start listening */
  if (bs->relays && bs->n_relays > 0) {
    int rc = nostr_nip46_bunker_listen(bs->session,
                                       (const char *const *)bs->relays,
                                       bs->n_relays);
    if (rc != 0) {
      g_warning("bunker: listen returned %d (may be expected for stub)", rc);
      /* Don't fail - the library may not have full relay support yet */
    }
  }

  set_state(bs, BUNKER_STATE_RUNNING, NULL);
  g_message("bunker: started for identity %s", bs->identity_npub);
  return TRUE;
}

void bunker_service_stop(BunkerService *bs) {
  if (!bs) return;

  if (bs->session) {
    nostr_nip46_session_free(bs->session);
    bs->session = NULL;
  }

  g_hash_table_remove_all(bs->connections);
  g_hash_table_remove_all(bs->pending_requests);

  set_state(bs, BUNKER_STATE_STOPPED, NULL);
  g_message("bunker: stopped");
}

BunkerState bunker_service_get_state(BunkerService *bs) {
  return bs ? bs->state : BUNKER_STATE_STOPPED;
}

gchar *bunker_service_get_bunker_uri(BunkerService *bs, const gchar *secret) {
  if (!bs || !bs->identity_pubkey_hex) return NULL;

  gchar *uri = NULL;
  int rc = nostr_nip46_bunker_issue_bunker_uri(bs->session,
                                               bs->identity_pubkey_hex,
                                               (const char *const *)bs->relays,
                                               bs->n_relays,
                                               secret,
                                               &uri);
  if (rc != 0 || !uri) {
    /* Build manually if library fails
     * Use GnSecureString for the secret parameter handling */
    GString *s = g_string_new("bunker://");
    g_string_append(s, bs->identity_pubkey_hex);

    gboolean first = TRUE;
    if (bs->relays) {
      for (gsize i = 0; bs->relays[i]; i++) {
        g_string_append_printf(s, "%srelay=%s", first ? "?" : "&", bs->relays[i]);
        first = FALSE;
      }
    }
    if (secret && *secret) {
      /* Note: The secret in the URI should be treated carefully;
       * ideally this URI should not be logged or stored persistently */
      g_string_append_printf(s, "%ssecret=%s", first ? "?" : "&", secret);
    }

    uri = g_string_free(s, FALSE);
  }

  return uri;
}

gboolean bunker_service_handle_connect_uri(BunkerService *bs, const gchar *uri) {
  if (!bs || !uri) return FALSE;

  if (!g_str_has_prefix(uri, "nostrconnect://")) {
    g_warning("bunker: invalid connect URI: %s", uri);
    return FALSE;
  }

  /* Parse the URI */
  NostrNip46ConnectURI parsed = {0};
  if (nostr_nip46_uri_parse_connect(uri, &parsed) != 0) {
    g_warning("bunker: failed to parse connect URI");
    return FALSE;
  }

  /* Create connection entry */
  if (parsed.client_pubkey_hex) {
    BunkerConnection *conn = g_new0(BunkerConnection, 1);
    conn->client_pubkey = g_strdup(parsed.client_pubkey_hex);
    conn->connected_at = (gint64)time(NULL);

    g_hash_table_replace(bs->connections, g_strdup(parsed.client_pubkey_hex), conn);

    if (bs->conn_cb) {
      bs->conn_cb(conn, bs->conn_cb_ud);
    }

    g_message("bunker: accepted connection from %s", parsed.client_pubkey_hex);
  }

  nostr_nip46_uri_connect_free(&parsed);
  return TRUE;
}

GPtrArray *bunker_service_list_connections(BunkerService *bs) {
  if (!bs) return NULL;

  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)bunker_connection_free);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, bs->connections);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    BunkerConnection *conn = (BunkerConnection*)value;

    BunkerConnection *copy = g_new0(BunkerConnection, 1);
    copy->client_pubkey = g_strdup(conn->client_pubkey);
    copy->app_name = g_strdup(conn->app_name);
    copy->permissions = g_strdupv(conn->permissions);
    copy->connected_at = conn->connected_at;
    copy->last_request = conn->last_request;
    copy->request_count = conn->request_count;

    g_ptr_array_add(arr, copy);
  }

  return arr;
}

gboolean bunker_service_disconnect_client(BunkerService *bs, const gchar *client_pubkey) {
  if (!bs || !client_pubkey) return FALSE;
  return g_hash_table_remove(bs->connections, client_pubkey);
}

void bunker_service_set_allowed_methods(BunkerService *bs, const gchar *const *methods) {
  if (!bs) return;
  g_strfreev(bs->allowed_methods);
  bs->allowed_methods = g_strdupv((gchar**)methods);
}

void bunker_service_set_allowed_pubkeys(BunkerService *bs, const gchar *const *pubkeys) {
  if (!bs) return;
  g_strfreev(bs->allowed_pubkeys);
  bs->allowed_pubkeys = g_strdupv((gchar**)pubkeys);
}

void bunker_service_set_auto_approve_kinds(BunkerService *bs, const gchar *const *kinds) {
  if (!bs) return;
  g_strfreev(bs->auto_approve_kinds);
  bs->auto_approve_kinds = g_strdupv((gchar**)kinds);
}

void bunker_service_set_state_callback(BunkerService *bs,
                                       BunkerStateChangedCb cb,
                                       gpointer user_data) {
  if (!bs) return;
  bs->state_cb = cb;
  bs->state_cb_ud = user_data;
}

void bunker_service_set_connection_callback(BunkerService *bs,
                                            BunkerConnectionCb cb,
                                            gpointer user_data) {
  if (!bs) return;
  bs->conn_cb = cb;
  bs->conn_cb_ud = user_data;
}

void bunker_service_set_authorize_callback(BunkerService *bs,
                                           BunkerAuthorizeCb cb,
                                           gpointer user_data) {
  if (!bs) return;
  bs->auth_cb = cb;
  bs->auth_cb_ud = user_data;
}

void bunker_service_authorize_response(BunkerService *bs,
                                       const gchar *request_id,
                                       gboolean approved) {
  if (!bs || !request_id) return;

  BunkerSignRequest *req = g_hash_table_lookup(bs->pending_requests, request_id);
  if (!req) {
    g_warning("bunker: unknown request_id %s", request_id);
    return;
  }

  /* The actual signing is handled synchronously in sign_cb for now */
  /* This would be used for async UI approval flow */

  g_hash_table_remove(bs->pending_requests, request_id);
}

void bunker_service_create_client_session(BunkerService *bs,
                                          const gchar *client_pubkey,
                                          const gchar *app_name,
                                          gboolean persistent,
                                          gint64 ttl_seconds) {
  if (!bs || !client_pubkey) return;

  /* Get connection info if available */
  BunkerConnection *conn = g_hash_table_lookup(bs->connections, client_pubkey);
  const gchar *name = app_name ? app_name : (conn ? conn->app_name : NULL);

  /* Create session via session manager (nostrc-09n) */
  GnClientSessionManager *sess_mgr = gn_client_session_manager_get_default();

  /* Convert permissions to bitmask */
  guint perms = GN_PERM_CONNECT | GN_PERM_SIGN_EVENT | GN_PERM_GET_PUBLIC_KEY;
  if (conn && conn->permissions) {
    for (gint i = 0; conn->permissions[i]; i++) {
      if (g_strcmp0(conn->permissions[i], "encrypt") == 0)
        perms |= GN_PERM_ENCRYPT;
      if (g_strcmp0(conn->permissions[i], "decrypt") == 0)
        perms |= GN_PERM_DECRYPT;
    }
  }

  gn_client_session_manager_create_session(sess_mgr,
                                            client_pubkey,
                                            bs->identity_npub,
                                            name,
                                            perms,
                                            persistent,
                                            ttl_seconds);

  g_debug("bunker: created client session for %s (persistent=%d, ttl=%ld)",
          client_pubkey, persistent, (long)ttl_seconds);
}
