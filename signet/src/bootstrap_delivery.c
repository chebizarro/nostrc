/* SPDX-License-Identifier: MIT
 *
 * bootstrap_delivery.c - Fleet Commander bootstrap delivery via NIP-17.
 *
 * Uses libnostr NIP-17 gift-wrapped DMs for secure token delivery.
 */

#include "signet/bootstrap_delivery.h"
#include "signet/store.h"
#include "signet/store_tokens.h"
#include "signet/relay_pool.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <glib.h>
#include <sqlite3.h>
#include <nostr-event.h>
#include <nostr/nip17/nip17.h>
#include <json.h>

/* ----------------------------- send ---------------------------------------- */

int signet_bootstrap_send(const char *fleet_sk_hex,
                            const char *bootstrap_pubkey_hex,
                            const SignetBootstrapMessage *msg,
                            SignetRelayPool *relay_pool) {
  if (!fleet_sk_hex || !bootstrap_pubkey_hex || !msg || !msg->token ||
      !msg->agent_id || !relay_pool)
    return -1;

  /* Build JSON content for the DM. */
  GString *json = g_string_sized_new(256);
  g_string_append_printf(json,
      "{\"type\":\"bootstrap\",\"token\":\"%s\",\"agent_id\":\"%s\"",
      msg->token, msg->agent_id);
  if (msg->bootstrap_url)
    g_string_append_printf(json, ",\"bootstrap_url\":\"%s\"", msg->bootstrap_url);
  if (msg->expires_at > 0)
    g_string_append_printf(json, ",\"expires_at\":%" PRId64, msg->expires_at);
  if (msg->relay_urls && msg->n_relay_urls > 0) {
    g_string_append(json, ",\"relay_urls\":[");
    for (size_t i = 0; i < msg->n_relay_urls; i++) {
      if (i > 0) g_string_append_c(json, ',');
      g_string_append_printf(json, "\"%s\"", msg->relay_urls[i]);
    }
    g_string_append_c(json, ']');
  }
  g_string_append_c(json, '}');

  char *content = g_string_free(json, FALSE);

  /* Wrap as NIP-17 gift-wrapped DM. */
  NostrEvent *gift_wrap = nostr_nip17_wrap_dm(fleet_sk_hex,
                                                bootstrap_pubkey_hex,
                                                content);
  g_free(content);

  if (!gift_wrap)
    return -1;

  /* Serialize and publish to relay pool. */
  char *event_json = nostr_event_serialize(gift_wrap);
  nostr_event_free(gift_wrap);

  if (!event_json)
    return -1;

  int rc = signet_relay_pool_publish_event_json(relay_pool, event_json);
  free(event_json);

  return rc;
}

/* ----------------------------- receive ------------------------------------- */

int signet_bootstrap_receive(const char *gift_wrap_json,
                               const char *bootstrap_sk_hex,
                               SignetBootstrapReceived *out) {
  if (!gift_wrap_json || !bootstrap_sk_hex || !out)
    return -1;
  memset(out, 0, sizeof(*out));

  /* Deserialize the gift wrap event. */
  NostrEvent *gw = nostr_event_new();
  if (!gw) return -1;

  if (!nostr_event_deserialize(gw, gift_wrap_json)) {
    nostr_event_free(gw);
    return -1;
  }

  /* Decrypt using NIP-17. */
  char *content = NULL;
  char *sender_pubkey = NULL;
  int rc = nostr_nip17_decrypt_dm(gw, bootstrap_sk_hex, &content, &sender_pubkey);
  nostr_event_free(gw);

  if (rc != 0 || !content) {
    free(content);
    free(sender_pubkey);
    return -1;
  }

  out->sender_pubkey = sender_pubkey;

  /* Parse the JSON content. */
  char *token = NULL;
  char *agent_id = NULL;
  char *bootstrap_url = NULL;
  int64_t expires_at = 0;

  nostr_json_get_string(content, "token", &token);
  nostr_json_get_string(content, "agent_id", &agent_id);
  nostr_json_get_string(content, "bootstrap_url", &bootstrap_url);
  nostr_json_get_int64(content, "expires_at", &expires_at);

  if (!token || !agent_id) {
    free(token);
    free(agent_id);
    free(bootstrap_url);
    free(content);
    signet_bootstrap_received_clear(out);
    return -1;
  }

  out->token = token;
  out->agent_id = agent_id;
  out->bootstrap_url = bootstrap_url;
  out->expires_at = expires_at;

  /* Parse relay_urls array. */
  char **urls = NULL;
  size_t n_urls = 0;
  if (nostr_json_get_string_array(content, "relay_urls", &urls, &n_urls) == 0) {
    out->relay_urls = urls;
    out->n_relay_urls = n_urls;
  }

  free(content);
  return 0;
}

/* ----------------------------- ACK ----------------------------------------- */

int signet_bootstrap_send_ack(const char *agent_sk_hex,
                                const char *fleet_pubkey_hex,
                                const char *agent_id,
                                SignetRelayPool *relay_pool) {
  if (!agent_sk_hex || !fleet_pubkey_hex || !agent_id || !relay_pool)
    return -1;

  char *content = g_strdup_printf(
      "{\"type\":\"bootstrap_ack\",\"agent_id\":\"%s\"}", agent_id);

  NostrEvent *gift_wrap = nostr_nip17_wrap_dm(agent_sk_hex,
                                                fleet_pubkey_hex,
                                                content);
  g_free(content);

  if (!gift_wrap)
    return -1;

  char *event_json = nostr_event_serialize(gift_wrap);
  nostr_event_free(gift_wrap);

  if (!event_json)
    return -1;

  int rc = signet_relay_pool_publish_event_json(relay_pool, event_json);
  free(event_json);

  return rc;
}

/* ----------------------------- reissue check ------------------------------- */

bool signet_bootstrap_needs_reissue(SignetStore *store,
                                      const char *agent_id,
                                      int64_t now) {
  if (!store || !agent_id) return false;

  /* Check if agent has been bootstrapped (has a key in the store). */
  SignetAgentRecord rec;
  memset(&rec, 0, sizeof(rec));
  int rc = signet_store_get_agent(store, agent_id, &rec);

  if (rc == 0) {
    /* Agent already exists — no reissue needed. */
    signet_agent_record_clear(&rec);
    return false;
  }

  /* Agent doesn't exist yet — check if there's an active (non-expired,
   * non-consumed) token for this agent. If a valid token exists, no reissue
   * is needed. If all tokens are expired or consumed, reissue. */
  sqlite3 *db = signet_store_get_db(store);
  if (!db) return true; /* can't check — assume reissue needed */

  const char *sql =
      "SELECT COUNT(*) FROM bootstrap_tokens "
      "WHERE agent_id = ? AND used_at IS NULL AND expires_at >= ?;";
  sqlite3_stmt *stmt = NULL;
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return true; /* query failed — assume reissue needed */

  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, now);

  int active_tokens = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    active_tokens = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  /* If there are active, unexpired tokens, no reissue needed. */
  return (active_tokens == 0);
}

/* ----------------------------- cleanup ------------------------------------- */

void signet_bootstrap_received_clear(SignetBootstrapReceived *recv) {
  if (!recv) return;
  free(recv->token);
  free(recv->agent_id);
  free(recv->bootstrap_url);
  free(recv->sender_pubkey);
  if (recv->relay_urls) {
    for (size_t i = 0; i < recv->n_relay_urls; i++)
      free(recv->relay_urls[i]);
    free(recv->relay_urls);
  }
  memset(recv, 0, sizeof(*recv));
}
