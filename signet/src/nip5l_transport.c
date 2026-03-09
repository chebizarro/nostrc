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

  /* Capability check. */
  if (!signet_policy_evaluate(ns->policy, cs->agent_id, method, -1)) {
    free(method);
    return g_strdup("{\"error\":\"capability denied\"}");
  }

  char *resp = NULL;

  if (strcmp(method, "get_public_key") == 0) {
    SignetLoadedKey lk;
    memset(&lk, 0, sizeof(lk));
    if (!signet_key_store_load_agent_key(ns->keys, cs->agent_id, &lk)) {
      resp = g_strdup("{\"error\":\"key not found\"}");
    } else {
      uint8_t pk[32];
      if (crypto_scalarmult_ed25519_base_noclamp(pk, lk.secret_key) != 0) {
        resp = g_strdup("{\"error\":\"key derivation failed\"}");
      } else {
        char hex[65];
        for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", pk[i]);
        hex[64] = '\0';
        sodium_memzero(pk, sizeof(pk));
        resp = g_strdup_printf("{\"result\":\"%s\"}", hex);
      }
      signet_loaded_key_clear(&lk);
    }

  } else if (strcmp(method, "sign_event") == 0 ||
             strcmp(method, "nip04_encrypt") == 0 ||
             strcmp(method, "nip04_decrypt") == 0 ||
             strcmp(method, "nip44_encrypt") == 0 ||
             strcmp(method, "nip44_decrypt") == 0) {
    /* TODO: Wire to signing/encryption pipeline. */
    resp = g_strdup("{\"error\":\"not yet implemented\"}");

  } else if (strcmp(method, "ping") == 0) {
    resp = g_strdup("{\"result\":\"pong\"}");

  } else if (strcmp(method, "get_relays") == 0) {
    resp = g_strdup("{\"result\":{}}");

  } else {
    resp = g_strdup("{\"error\":\"unknown method\"}");
  }

  free(method);
  return resp;
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

  /* Read loop in a thread to avoid blocking the service. */
  /* For production, this should use GSource/async IO. Here we use
   * a simple synchronous read for clarity. */
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
  return TRUE;
}

/* ------------------------------ public API -------------------------------- */

SignetNip5lServer *signet_nip5l_server_new(const SignetNip5lServerConfig *cfg) {
  if (!cfg || !cfg->keys || !cfg->policy || !cfg->challenges)
    return NULL;

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
