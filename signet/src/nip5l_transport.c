/* SPDX-License-Identifier: MIT
 *
 * nip5l_transport.c - NIP-5L Unix socket transport for Signet v2.
 *
 * Line-delimited NIP-46 JSON over Unix socket.
 * Auth: SO_PEERCRED for same-host or Nostr challenge for forwarded.
 * Uses GLib GSocketService for the listener.
 */

#include "signet/nip5l_transport.h"
#include "signet/key_store.h"
#include "signet/capability.h"
#include "signet/nostr_auth.h"
#include "signet/store.h"
#include "signet/relay_pool.h"
#include "signet/audit_logger.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <json.h>
#include <sodium.h>

/* libnostr event signing + NIP-04 */
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip04.h>
#include <nostr/nip44/nip44.h>

/* NIP-46 JSON-RPC framing: line-delimited JSON. */
#define NIP5L_MAX_LINE 65536

/* Per-connection state. */
typedef struct {
  char *agent_id;
  char *pubkey_hex;
  bool authenticated;
  GIOChannel *channel;
} Nip5lConnState;

static void nip5l_conn_state_free(Nip5lConnState *s) {
  if (!s) return;
  g_free(s->agent_id);
  g_free(s->pubkey_hex);
  if (s->channel) {
    g_io_channel_shutdown(s->channel, FALSE, NULL);
    g_io_channel_unref(s->channel);
  }
  g_free(s);
}

struct SignetNip5lServer {
  char *socket_path;
  SignetKeyStore *keys;
  SignetPolicyRegistry *policy;
  SignetStore *store;
  SignetChallengeStore *challenges;
  SignetAuditLogger *audit;
  const SignetFleetRegistry *fleet;
  SignetRelayPool *relays;

  GSocketService *service;
  GList *connections;  /* list of Nip5lConnState* */
  GMutex mu;
};

static int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

/* ----------------------------- message handling --------------------------- */

/* Process a single NIP-46 JSON-RPC line from client.
 * Returns response JSON (caller frees), or NULL to close. */
static char *nip5l_handle_message(SignetNip5lServer *ns,
                                    Nip5lConnState *cs,
                                    const char *line) {
  if (!line || !line[0]) return NULL;

  /* Before authentication, only "challenge" and "authenticate" are allowed. */
  if (!cs->authenticated) {
    /* Expect: {"method":"get_challenge","params":["<agent_id>"]} or
     *         {"method":"authenticate","params":["<signed_event_json>"]} */

    /* Simple JSON parsing via libnostr json helpers. */
    char *method = NULL;
    nostr_json_get_string(line, "method", &method);

    if (method && strcmp(method, "get_challenge") == 0) {
      char *agent_id = NULL;
      nostr_json_get_array_string(line, "params", 0, &agent_id);

      if (!agent_id || !agent_id[0]) {
        free(method);
        free(agent_id);
        return g_strdup("{\"error\":\"agent_id required\"}");
      }

      int64_t now = signet_now_unix();
      char *challenge = signet_challenge_issue(ns->challenges, agent_id, now);
      free(agent_id);
      free(method);

      if (!challenge)
        return g_strdup("{\"error\":\"failed to issue challenge\"}");

      char *resp = g_strdup_printf("{\"result\":\"%s\"}", challenge);
      g_free(challenge);
      return resp;
    }

    if (method && strcmp(method, "authenticate") == 0) {
      char *auth_event = NULL;
      nostr_json_get_array_string(line, "params", 0, &auth_event);

      int64_t now = signet_now_unix();
      char *agent_id = NULL;
      char *pubkey_hex = NULL;
      SignetAuthResult ar = signet_auth_verify(
          ns->challenges, ns->fleet, auth_event, now, &agent_id, &pubkey_hex);
      free(auth_event);
      free(method);

      if (ar != SIGNET_AUTH_OK) {
        g_free(agent_id);
        g_free(pubkey_hex);
        return g_strdup_printf("{\"error\":\"%s\"}", signet_auth_result_string(ar));
      }

      cs->agent_id = agent_id;
      cs->pubkey_hex = pubkey_hex;
      cs->authenticated = true;
      return g_strdup_printf("{\"result\":\"authenticated\",\"agent_id\":\"%s\"}", agent_id);
    }

    free(method);
    return g_strdup("{\"error\":\"authenticate first\"}");
  }

  /* Authenticated — dispatch NIP-46 methods. */
  char *method = NULL;
  nostr_json_get_string(line, "method", &method);
  if (!method)
    return g_strdup("{\"error\":\"missing method\"}");

  /* Capability check (skip if policy not configured). */
  if (ns->policy && !signet_policy_evaluate(ns->policy, cs->agent_id, method, -1)) {
    free(method);
    return g_strdup("{\"error\":\"capability denied\"}");
  }

  char *resp = NULL;

  if (strcmp(method, "get_public_key") == 0) {
    char pubkey_hex[65];
    if (!signet_key_store_get_agent_pubkey(ns->keys, cs->agent_id, pubkey_hex, sizeof(pubkey_hex))) {
      resp = g_strdup("{\"error\":\"key not found\"}");
    } else {
      resp = g_strdup_printf("{\"result\":\"%s\"}", pubkey_hex);
    }

  } else if (strcmp(method, "sign_event") == 0) {
    char *event_json = NULL;
    nostr_json_get_array_string(line, "params", 0, &event_json);
    if (!event_json || !event_json[0]) {
      signet_audit_log_common(ns->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
          &(SignetAuditCommonFields){ .identity = cs->agent_id, .method = "sign_event",
            .decision = "error", .reason_code = "missing_params" },
          "{\"transport\":\"nip5l\"}");
      resp = g_strdup("{\"error\":\"missing event json\"}");
      free(event_json);
    } else {
      SignetLoadedKey lk;
      memset(&lk, 0, sizeof(lk));
      if (!signet_key_store_load_agent_key(ns->keys, cs->agent_id, &lk)) {
        resp = g_strdup("{\"error\":\"key not found\"}");
      } else {
        NostrEvent *ev = nostr_event_new();
        if (!ev || nostr_event_deserialize(ev, event_json) != 0) {
          if (ev) nostr_event_free(ev);
          resp = g_strdup("{\"error\":\"invalid event json\"}");
        } else {
          char sk_hex[65];
          for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", lk.secret_key[i]);
          sk_hex[64] = '\0';
          int src = nostr_event_sign(ev, sk_hex);
          sodium_memzero(sk_hex, sizeof(sk_hex));
          if (src != 0) {
            resp = g_strdup("{\"error\":\"signing failed\"}");
          } else {
            char *signed_json = nostr_event_serialize(ev);
            if (signed_json) {
              signet_audit_log_common(ns->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
                  &(SignetAuditCommonFields){ .identity = cs->agent_id, .method = "sign_event",
                    .decision = "allow", .reason_code = "ok" },
                  "{\"transport\":\"nip5l\"}");
              resp = g_strdup_printf("{\"result\":%s}", signed_json);
            } else {
              resp = g_strdup("{\"error\":\"serialization failed\"}");
            }
            free(signed_json);
          }
          nostr_event_free(ev);
        }
        signet_loaded_key_clear(&lk);
      }
      free(event_json);
    }

  } else if (strcmp(method, "nip04_encrypt") == 0) {
    char *plaintext = NULL, *peer_pk = NULL;
    nostr_json_get_array_string(line, "params", 0, &peer_pk);
    nostr_json_get_array_string(line, "params", 1, &plaintext);
    SignetLoadedKey lk;
    memset(&lk, 0, sizeof(lk));
    if (!peer_pk || !plaintext || !signet_key_store_load_agent_key(ns->keys, cs->agent_id, &lk)) {
      resp = g_strdup("{\"error\":\"missing params or key not found\"}");
    } else {
      char sk_hex[65];
      for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", lk.secret_key[i]);
      sk_hex[64] = '\0';
      char *ct = NULL, *err_msg = NULL;
      if (nostr_nip04_encrypt(plaintext, peer_pk, sk_hex, &ct, &err_msg) != 0) {
        resp = g_strdup_printf("{\"error\":\"%s\"}", err_msg ? err_msg : "encrypt failed");
        free(err_msg);
      } else {
        signet_audit_log_common(ns->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
            &(SignetAuditCommonFields){ .identity = cs->agent_id, .method = "nip04_encrypt",
              .decision = "allow", .reason_code = "ok" },
            "{\"transport\":\"nip5l\",\"algo\":\"nip04\"}");
        resp = g_strdup_printf("{\"result\":\"%s\"}", ct);
        free(ct);
      }
      sodium_memzero(sk_hex, sizeof(sk_hex));
      signet_loaded_key_clear(&lk);
    }
    free(peer_pk);
    free(plaintext);

  } else if (strcmp(method, "nip04_decrypt") == 0) {
    char *ciphertext = NULL, *peer_pk = NULL;
    nostr_json_get_array_string(line, "params", 0, &peer_pk);
    nostr_json_get_array_string(line, "params", 1, &ciphertext);
    SignetLoadedKey lk;
    memset(&lk, 0, sizeof(lk));
    if (!peer_pk || !ciphertext || !signet_key_store_load_agent_key(ns->keys, cs->agent_id, &lk)) {
      resp = g_strdup("{\"error\":\"missing params or key not found\"}");
    } else {
      char sk_hex[65];
      for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", lk.secret_key[i]);
      sk_hex[64] = '\0';
      char *pt = NULL, *err_msg = NULL;
      if (nostr_nip04_decrypt(ciphertext, peer_pk, sk_hex, &pt, &err_msg) != 0) {
        resp = g_strdup_printf("{\"error\":\"%s\"}", err_msg ? err_msg : "decrypt failed");
        free(err_msg);
      } else {
        signet_audit_log_common(ns->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
            &(SignetAuditCommonFields){ .identity = cs->agent_id, .method = "nip04_decrypt",
              .decision = "allow", .reason_code = "ok" },
            "{\"transport\":\"nip5l\",\"algo\":\"nip04\"}");
        resp = g_strdup_printf("{\"result\":\"%s\"}", pt);
        free(pt);
      }
      sodium_memzero(sk_hex, sizeof(sk_hex));
      signet_loaded_key_clear(&lk);
    }
    free(peer_pk);
    free(ciphertext);

  } else if (strcmp(method, "nip44_encrypt") == 0) {
    char *plaintext = NULL, *peer_pk_hex = NULL;
    nostr_json_get_array_string(line, "params", 0, &peer_pk_hex);
    nostr_json_get_array_string(line, "params", 1, &plaintext);
    SignetLoadedKey lk;
    memset(&lk, 0, sizeof(lk));
    if (!peer_pk_hex || !plaintext || !signet_key_store_load_agent_key(ns->keys, cs->agent_id, &lk)) {
      resp = g_strdup("{\"error\":\"missing params or key not found\"}");
    } else {
      /* Decode hex peer pubkey to 32 bytes. */
      uint8_t peer_pk[32];
      bool pk_ok = (strlen(peer_pk_hex) == 64);
      if (pk_ok) {
        for (int i = 0; i < 32; i++) {
          unsigned int byte;
          if (sscanf(peer_pk_hex + i * 2, "%2x", &byte) != 1) { pk_ok = false; break; }
          peer_pk[i] = (uint8_t)byte;
        }
      }
      if (!pk_ok) {
        resp = g_strdup("{\"error\":\"invalid peer pubkey hex\"}");
      } else {
        char *ct = NULL;
        if (nostr_nip44_encrypt_v2(lk.secret_key, peer_pk,
                                   (const uint8_t *)plaintext, strlen(plaintext),
                                   &ct) != 0) {
          resp = g_strdup("{\"error\":\"nip44 encrypt failed\"}");
        } else {
          signet_audit_log_common(ns->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
              &(SignetAuditCommonFields){ .identity = cs->agent_id, .method = "nip44_encrypt",
                .decision = "allow", .reason_code = "ok" },
              "{\"transport\":\"nip5l\",\"algo\":\"nip44\"}");
          resp = g_strdup_printf("{\"result\":\"%s\"}", ct);
          free(ct);
        }
      }
      signet_loaded_key_clear(&lk);
    }
    free(peer_pk_hex);
    free(plaintext);

  } else if (strcmp(method, "nip44_decrypt") == 0) {
    char *ciphertext = NULL, *peer_pk_hex = NULL;
    nostr_json_get_array_string(line, "params", 0, &peer_pk_hex);
    nostr_json_get_array_string(line, "params", 1, &ciphertext);
    SignetLoadedKey lk;
    memset(&lk, 0, sizeof(lk));
    if (!peer_pk_hex || !ciphertext || !signet_key_store_load_agent_key(ns->keys, cs->agent_id, &lk)) {
      resp = g_strdup("{\"error\":\"missing params or key not found\"}");
    } else {
      /* Decode hex peer pubkey to 32 bytes. */
      uint8_t peer_pk[32];
      bool pk_ok = (strlen(peer_pk_hex) == 64);
      if (pk_ok) {
        for (int i = 0; i < 32; i++) {
          unsigned int byte;
          if (sscanf(peer_pk_hex + i * 2, "%2x", &byte) != 1) { pk_ok = false; break; }
          peer_pk[i] = (uint8_t)byte;
        }
      }
      if (!pk_ok) {
        resp = g_strdup("{\"error\":\"invalid peer pubkey hex\"}");
      } else {
        uint8_t *raw_pt = NULL;
        size_t raw_pt_len = 0;
        if (nostr_nip44_decrypt_v2(lk.secret_key, peer_pk, ciphertext, &raw_pt, &raw_pt_len) != 0) {
          resp = g_strdup("{\"error\":\"nip44 decrypt failed\"}");
        } else {
          /* NUL-terminate the decrypted bytes for JSON. */
          char *pt_str = (char *)g_malloc(raw_pt_len + 1);
          memcpy(pt_str, raw_pt, raw_pt_len);
          pt_str[raw_pt_len] = '\0';
          free(raw_pt);
          signet_audit_log_common(ns->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
              &(SignetAuditCommonFields){ .identity = cs->agent_id, .method = "nip44_decrypt",
                .decision = "allow", .reason_code = "ok" },
              "{\"transport\":\"nip5l\",\"algo\":\"nip44\"}");
          resp = g_strdup_printf("{\"result\":\"%s\"}", pt_str);
          g_free(pt_str);
        }
      }
      signet_loaded_key_clear(&lk);
    }
    free(peer_pk_hex);
    free(ciphertext);

  } else if (strcmp(method, "ping") == 0) {
    resp = g_strdup("{\"result\":\"pong\"}");

  } else if (strcmp(method, "get_relays") == 0) {
    /* Build a JSON object mapping relay URLs to read/write hints. */
    if (ns->relays) {
      size_t n_urls = 0;
      const char *const *urls = signet_relay_pool_get_urls(ns->relays, &n_urls);
      GString *rb = g_string_new("{\"result\":{");
      for (size_t ri = 0; ri < n_urls; ri++) {
        if (ri > 0) g_string_append_c(rb, ',');
        g_string_append_printf(rb, "\"%s\":{\"read\":true,\"write\":true}",
                               urls[ri] ? urls[ri] : "");
      }
      g_string_append(rb, "}}");
      resp = g_string_free(rb, FALSE);
    } else {
      resp = g_strdup("{\"result\":{}}");
    }

  } else {
    resp = g_strdup("{\"error\":\"unknown method\"}");
  }

  free(method);
  return resp;
}

/* ----------------------------- per-client thread -------------------------- */

typedef struct {
  SignetNip5lServer *ns;
  Nip5lConnState *cs;
  GSocketConnection *conn_ref; /* ref'd to keep the socket alive */
} Nip5lClientThreadData;

static gpointer nip5l_client_thread(gpointer data) {
  Nip5lClientThreadData *td = (Nip5lClientThreadData *)data;
  SignetNip5lServer *ns = td->ns;
  Nip5lConnState *cs = td->cs;

  GError *err = NULL;
  gchar *line = NULL;
  gsize len = 0;
  GIOStatus status;

  while ((status = g_io_channel_read_line(cs->channel, &line, &len, NULL, &err))
         == G_IO_STATUS_NORMAL) {
    if (!line || len == 0) break;

    /* Strip trailing newline. */
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

    char *resp = nip5l_handle_message(ns, cs, line);
    g_free(line);
    line = NULL;

    if (resp) {
      /* Send response + newline. */
      gsize written;
      g_io_channel_write_chars(cs->channel, resp, -1, &written, NULL);
      g_io_channel_write_chars(cs->channel, "\n", 1, &written, NULL);
      g_io_channel_flush(cs->channel, NULL);
      g_free(resp);
    }
  }

  if (err) g_error_free(err);
  g_free(line);

  g_mutex_lock(&ns->mu);
  ns->connections = g_list_remove(ns->connections, cs);
  g_mutex_unlock(&ns->mu);

  nip5l_conn_state_free(cs);
  g_object_unref(td->conn_ref); /* release the connection ref */
  g_free(td);
  return NULL;
}

/* ----------------------------- GIO callbacks ------------------------------ */

static gboolean
nip5l_on_incoming(GSocketService *service,
                   GSocketConnection *connection,
                   GObject *source_object,
                   gpointer user_data) {
  (void)service;
  (void)source_object;
  SignetNip5lServer *ns = (SignetNip5lServer *)user_data;

  Nip5lConnState *cs = g_new0(Nip5lConnState, 1);

  GSocket *sock = g_socket_connection_get_socket(connection);
  int fd = g_socket_get_fd(sock);
  cs->channel = g_io_channel_unix_new(fd);
  g_io_channel_set_encoding(cs->channel, NULL, NULL);
  g_io_channel_set_line_term(cs->channel, "\n", 1);

  g_mutex_lock(&ns->mu);
  ns->connections = g_list_prepend(ns->connections, cs);
  g_mutex_unlock(&ns->mu);

  /* Spawn a dedicated thread per client so the listener stays available
   * for new connections.  Ref the GSocketConnection to keep the underlying
   * fd alive until the thread finishes. */
  Nip5lClientThreadData *td = g_new0(Nip5lClientThreadData, 1);
  td->ns = ns;
  td->cs = cs;
  td->conn_ref = g_object_ref(connection);

  GThread *t = g_thread_new("signet-nip5l-client", nip5l_client_thread, td);
  if (t) {
    g_thread_unref(t); /* detach — thread manages its own lifetime */
  } else {
    /* Thread creation failed — clean up inline. */
    g_mutex_lock(&ns->mu);
    ns->connections = g_list_remove(ns->connections, cs);
    g_mutex_unlock(&ns->mu);
    nip5l_conn_state_free(cs);
    g_object_unref(td->conn_ref);
    g_free(td);
  }

  return TRUE;
}

/* ------------------------------ public API -------------------------------- */

SignetNip5lServer *signet_nip5l_server_new(const SignetNip5lServerConfig *cfg) {
  if (!cfg || !cfg->keys || !cfg->challenges)
    return NULL;  /* policy may be NULL (skips capability check) */

  SignetNip5lServer *ns = g_new0(SignetNip5lServer, 1);
  if (!ns) return NULL;

  ns->socket_path = g_strdup(cfg->socket_path
      ? cfg->socket_path : "/run/signet/nip5l.sock");
  ns->keys = cfg->keys;
  ns->policy = cfg->policy;
  ns->store = cfg->store;
  ns->challenges = cfg->challenges;
  ns->audit = cfg->audit;
  ns->fleet = cfg->fleet;
  ns->relays = cfg->relays;
  g_mutex_init(&ns->mu);

  return ns;
}

void signet_nip5l_server_free(SignetNip5lServer *ns) {
  if (!ns) return;
  signet_nip5l_server_stop(ns);
  g_mutex_lock(&ns->mu);
  g_list_free_full(ns->connections, (GDestroyNotify)nip5l_conn_state_free);
  ns->connections = NULL;
  g_mutex_unlock(&ns->mu);
  g_mutex_clear(&ns->mu);
  g_free(ns->socket_path);
  g_free(ns);
}

int signet_nip5l_server_start(SignetNip5lServer *ns) {
  if (!ns) return -1;
  if (ns->service) return 0;

  /* Remove stale socket file. */
  (void)unlink(ns->socket_path);

  ns->service = g_socket_service_new();

  GSocketAddress *addr = g_unix_socket_address_new(ns->socket_path);
  GError *err = NULL;
  gboolean ok = g_socket_listener_add_address(
      G_SOCKET_LISTENER(ns->service), addr,
      G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
      NULL, NULL, &err);
  g_object_unref(addr);

  if (!ok) {
    if (err) {
      g_warning("signet-nip5l: failed to listen on %s: %s",
                ns->socket_path, err->message);
      g_error_free(err);
    }
    g_object_unref(ns->service);
    ns->service = NULL;
    return -1;
  }

  g_signal_connect(ns->service, "incoming",
                   G_CALLBACK(nip5l_on_incoming), ns);

  g_socket_service_start(ns->service);
  return 0;
}

void signet_nip5l_server_stop(SignetNip5lServer *ns) {
  if (!ns || !ns->service) return;
  g_socket_service_stop(ns->service);
  g_object_unref(ns->service);
  ns->service = NULL;
  (void)unlink(ns->socket_path);
}
