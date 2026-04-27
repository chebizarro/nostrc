/* SPDX-License-Identifier: MIT
 *
 * dbus_common.c - Shared authenticated D-Bus Signer/Credentials dispatch.
 */

#include "dbus_common.h"

#include "signet/key_store.h"
#include "signet/capability.h"
#include "signet/store.h"
#include "signet/store_leases.h"
#include "signet/store_secrets.h"
#include "signet/audit_logger.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>
#include <sodium.h>

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip04.h>
#include <nostr/nip44/nip44.h>

static int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out_hex) {
  for (size_t i = 0; i < len; i++)
    sprintf(out_hex + i * 2, "%02x", bytes[i]);
  out_hex[len * 2] = '\0';
}

static bool hex_to_bytes32(const char *hex, uint8_t out[32]) {
  if (!hex || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
    out[i] = (uint8_t)byte;
  }
  return true;
}

static void sha256_hex(const char *input,
                       char out_hex[crypto_hash_sha256_BYTES * 2 + 1]) {
  uint8_t hash_raw[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(hash_raw, (const uint8_t *)input, strlen(input));
  bytes_to_hex(hash_raw, sizeof(hash_raw), out_hex);
  sodium_memzero(hash_raw, sizeof(hash_raw));
}

static char *build_session_metadata(const char *transport,
                                    const char *service_url,
                                    const char *session_token_hash) {
  JsonBuilder *builder = json_builder_new();
  if (!builder) return NULL;

  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "session_token_hash");
  json_builder_add_string_value(builder, session_token_hash ? session_token_hash : "");

  json_builder_set_member_name(builder, "transport");
  json_builder_add_string_value(builder, transport ? transport : "dbus");

  json_builder_set_member_name(builder, "service_url");
  json_builder_add_string_value(builder, service_url ? service_url : "");

  json_builder_end_object(builder);

  JsonGenerator *generator = json_generator_new();
  if (!generator) {
    g_object_unref(builder);
    return NULL;
  }

  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  json_generator_set_pretty(generator, FALSE);
  char *json = json_generator_to_data(generator, NULL);

  json_node_free(root);
  g_object_unref(generator);
  g_object_unref(builder);
  return json;
}

static const char *transport_name(const SignetDbusDispatchContext *ctx) {
  return (ctx && ctx->transport) ? ctx->transport : "dbus";
}

static void audit_key_access(const SignetDbusDispatchContext *ctx,
                             const char *agent_id,
                             const char *method,
                             const char *decision,
                             const char *reason_code) {
  if (!ctx || !ctx->audit) return;
  char detail[96];
  snprintf(detail, sizeof(detail), "{\"transport\":\"%s\"}", transport_name(ctx));
  signet_audit_log_common(ctx->audit, SIGNET_AUDIT_EVENT_KEY_ACCESS,
      &(SignetAuditCommonFields){ .identity = agent_id, .method = method,
        .decision = decision, .reason_code = reason_code }, detail);
}

static void audit_sign_request(const SignetDbusDispatchContext *ctx,
                               const char *agent_id,
                               const char *method,
                               const char *detail_json) {
  if (!ctx || !ctx->audit) return;
  char detail[128];
  if (!detail_json) {
    snprintf(detail, sizeof(detail), "{\"transport\":\"%s\"}", transport_name(ctx));
    detail_json = detail;
  }
  signet_audit_log_common(ctx->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
      &(SignetAuditCommonFields){ .identity = agent_id, .method = method,
        .decision = "allow", .reason_code = "ok" }, detail_json);
}

static void handle_get_public_key(const SignetDbusDispatchContext *ctx,
                                  const char *agent_id,
                                  GDBusMethodInvocation *invocation) {
  char pubkey_hex[65];
  if (!signet_key_store_get_agent_pubkey(ctx->keys, agent_id,
                                         pubkey_hex, sizeof(pubkey_hex))) {
    audit_key_access(ctx, agent_id, "GetPublicKey", "error", "key_not_found");
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.NotFound", "Agent key not found");
    return;
  }

  audit_key_access(ctx, agent_id, "GetPublicKey", "allow", "ok");
  g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", pubkey_hex));
}

static void handle_sign_event(const SignetDbusDispatchContext *ctx,
                              const char *agent_id,
                              GVariant *parameters,
                              GDBusMethodInvocation *invocation) {
  const char *event_json = NULL;
  g_variant_get(parameters, "(&s)", &event_json);

  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  if (!signet_key_store_load_agent_key(ctx->keys, agent_id, &lk)) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.NotFound", "Agent key not found");
    return;
  }

  NostrEvent *ev = nostr_event_new();
  if (!ev || !nostr_event_deserialize_compact(ev, event_json, NULL)) {
    if (ev) nostr_event_free(ev);
    signet_loaded_key_clear(&lk);
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.BadRequest", "Invalid event JSON");
    return;
  }

  char sk_hex[65];
  bytes_to_hex(lk.secret_key, 32, sk_hex);
  int sign_rc = nostr_event_sign(ev, sk_hex);
  sodium_memzero(sk_hex, sizeof(sk_hex));
  signet_loaded_key_clear(&lk);

  if (sign_rc != 0) {
    nostr_event_free(ev);
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Internal", "Signing failed");
    return;
  }

  char *signed_json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  if (!signed_json) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Internal", "Serialization failed");
    return;
  }

  audit_sign_request(ctx, agent_id, "SignEvent", NULL);
  g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", signed_json));
  free(signed_json);
}

static bool parse_algorithm(const char *algo,
                            bool *out_use_nip04,
                            bool *out_use_nip44,
                            GDBusMethodInvocation *invocation) {
  bool use_nip44 = (algo && strcmp(algo, "nip44") == 0);
  bool use_nip04 = (!algo || algo[0] == '\0' || strcmp(algo, "nip04") == 0);
  if (!use_nip04 && !use_nip44) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.BadRequest",
        "Unsupported algorithm; use 'nip04', 'nip44', or empty");
    return false;
  }
  *out_use_nip04 = use_nip04;
  *out_use_nip44 = use_nip44;
  return true;
}

static void handle_encrypt(const SignetDbusDispatchContext *ctx,
                           const char *agent_id,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation) {
  const char *plaintext = NULL, *peer_pubkey = NULL, *algo = NULL;
  g_variant_get(parameters, "(&s&s&s)", &plaintext, &peer_pubkey, &algo);

  bool use_nip04 = false, use_nip44 = false;
  if (!parse_algorithm(algo, &use_nip04, &use_nip44, invocation)) return;

  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  if (!signet_key_store_load_agent_key(ctx->keys, agent_id, &lk)) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.NotFound", "Agent key not found");
    return;
  }

  char *result_ct = NULL;
  int rc = -1;

  if (use_nip44) {
    uint8_t peer_pk[32];
    if (!hex_to_bytes32(peer_pubkey, peer_pk)) {
      signet_loaded_key_clear(&lk);
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.BadRequest", "Invalid peer pubkey hex");
      return;
    }
    rc = nostr_nip44_encrypt_v2(lk.secret_key, peer_pk,
                                (const uint8_t *)plaintext, strlen(plaintext),
                                &result_ct);
  } else if (use_nip04) {
    char sk_hex[65];
    bytes_to_hex(lk.secret_key, 32, sk_hex);
    char *err_msg = NULL;
    rc = nostr_nip04_encrypt(plaintext, peer_pubkey, sk_hex, &result_ct, &err_msg);
    sodium_memzero(sk_hex, sizeof(sk_hex));
    free(err_msg);
  }
  signet_loaded_key_clear(&lk);

  if (rc != 0 || !result_ct) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Internal", "Encryption failed");
    return;
  }

  char audit_detail[128];
  snprintf(audit_detail, sizeof(audit_detail),
           "{\"transport\":\"%s\",\"algo\":\"%s\"}",
           transport_name(ctx), use_nip44 ? "nip44" : "nip04");
  audit_sign_request(ctx, agent_id, "Encrypt", audit_detail);
  g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", result_ct));
  free(result_ct);
}

static void handle_decrypt(const SignetDbusDispatchContext *ctx,
                           const char *agent_id,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation) {
  const char *ciphertext = NULL, *peer_pubkey = NULL, *algo = NULL;
  g_variant_get(parameters, "(&s&s&s)", &ciphertext, &peer_pubkey, &algo);

  bool use_nip04 = false, use_nip44 = false;
  if (!parse_algorithm(algo, &use_nip04, &use_nip44, invocation)) return;

  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  if (!signet_key_store_load_agent_key(ctx->keys, agent_id, &lk)) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.NotFound", "Agent key not found");
    return;
  }

  char *result_pt = NULL;
  int rc = -1;

  if (use_nip44) {
    uint8_t peer_pk[32];
    if (!hex_to_bytes32(peer_pubkey, peer_pk)) {
      signet_loaded_key_clear(&lk);
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.BadRequest", "Invalid peer pubkey hex");
      return;
    }
    uint8_t *raw_pt = NULL;
    size_t raw_pt_len = 0;
    rc = nostr_nip44_decrypt_v2(lk.secret_key, peer_pk, ciphertext,
                                &raw_pt, &raw_pt_len);
    if (rc == 0 && raw_pt) {
      result_pt = (char *)malloc(raw_pt_len + 1);
      if (result_pt) {
        memcpy(result_pt, raw_pt, raw_pt_len);
        result_pt[raw_pt_len] = '\0';
      }
      free(raw_pt);
    }
  } else if (use_nip04) {
    char sk_hex[65];
    bytes_to_hex(lk.secret_key, 32, sk_hex);
    char *err_msg = NULL;
    rc = nostr_nip04_decrypt(ciphertext, peer_pubkey, sk_hex, &result_pt, &err_msg);
    sodium_memzero(sk_hex, sizeof(sk_hex));
    free(err_msg);
  }
  signet_loaded_key_clear(&lk);

  if (rc != 0 || !result_pt) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Internal", "Decryption failed");
    return;
  }

  char audit_detail[128];
  snprintf(audit_detail, sizeof(audit_detail),
           "{\"transport\":\"%s\",\"algo\":\"%s\"}",
           transport_name(ctx), use_nip44 ? "nip44" : "nip04");
  audit_sign_request(ctx, agent_id, "Decrypt", audit_detail);
  g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", result_pt));
  free(result_pt);
}

static void handle_get_session(const SignetDbusDispatchContext *ctx,
                               const char *agent_id,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation) {
  const char *service_url = NULL;
  g_variant_get(parameters, "(&s)", &service_url);

  uint8_t raw[32];
  randombytes_buf(raw, sizeof(raw));
  char token_hex[65];
  bytes_to_hex(raw, sizeof(raw), token_hex);
  sodium_memzero(raw, sizeof(raw));

  int64_t now = signet_now_unix();
  int64_t expires_at = now + 24 * 60 * 60;

  if (!ctx->store) {
    /* Preserve legacy store-less transport behavior: return an opaque token
     * without server-side validation state. When a store is configured, the
     * lease metadata below is the source of truth and persists only the token
     * hash, never the raw token. */
    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(sx)", token_hex, (gint64)expires_at));
    sodium_memzero(token_hex, sizeof(token_hex));
    return;
  }

  uint8_t lid_raw[16];
  randombytes_buf(lid_raw, sizeof(lid_raw));
  char lease_id[33];
  bytes_to_hex(lid_raw, sizeof(lid_raw), lease_id);
  sodium_memzero(lid_raw, sizeof(lid_raw));

  char token_hash[crypto_hash_sha256_BYTES * 2 + 1];
  sha256_hex(token_hex, token_hash);
  char *meta = build_session_metadata(transport_name(ctx), service_url, token_hash);
  if (!meta) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Internal", "Failed to build lease metadata");
    return;
  }

  if (signet_store_issue_lease(ctx->store, lease_id, "session",
                               agent_id, now, expires_at, meta) != 0) {
    g_free(meta);
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Internal", "Failed to persist session lease");
    return;
  }
  g_free(meta);

  g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(sx)", token_hex, (gint64)expires_at));
  sodium_memzero(token_hex, sizeof(token_hex));
}

static void handle_get_token(const SignetDbusDispatchContext *ctx,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation) {
  const char *cred_type = NULL;
  g_variant_get(parameters, "(&s)", &cred_type);

  if (!ctx->store) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.NotConfigured", "Store not available");
    return;
  }

  SignetSecretRecord rec;
  memset(&rec, 0, sizeof(rec));
  int rc = signet_store_get_secret(ctx->store, cred_type, &rec);
  if (rc != 0) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.NotFound", "Credential not found");
    return;
  }

  int64_t expires_at = signet_now_unix() + 3600;
  char *token_hex = NULL;
  if (rec.payload && rec.payload_len > 0) {
    token_hex = g_malloc(rec.payload_len * 2 + 1);
    bytes_to_hex(rec.payload, rec.payload_len, token_hex);
  }

  g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(sx)", token_hex ? token_hex : "", (gint64)expires_at));
  g_free(token_hex);
  signet_secret_record_clear(&rec);
}

static void handle_list_credentials(const SignetDbusDispatchContext *ctx,
                                    const char *agent_id,
                                    GDBusMethodInvocation *invocation) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

  if (ctx->store) {
    char **ids = NULL;
    char **labels = NULL;
    size_t count = 0;
    if (signet_store_list_secrets(ctx->store, agent_id, &ids, &labels, &count) == 0) {
      for (size_t i = 0; i < count; i++) {
        if (ids[i])
          g_variant_builder_add(&builder, "s", ids[i]);
      }
      signet_store_free_secret_list(ids, labels, count);
    }
  }

  g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(as)", &builder));
}

void signet_dbus_dispatch_authenticated(const SignetDbusDispatchContext *ctx,
                                        const char *agent_id,
                                        const gchar *interface_name,
                                        const gchar *method_name,
                                        GVariant *parameters,
                                        GDBusMethodInvocation *invocation) {
  if (!ctx || !ctx->keys || !agent_id || !method_name || !interface_name) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Internal", "Invalid dispatch context");
    return;
  }

  if (ctx->policy && !signet_policy_evaluate(ctx->policy, agent_id, method_name, -1)) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.CapabilityDenied",
        "Operation not permitted by policy");
    return;
  }

  if (strcmp(interface_name, "net.signet.Signer") == 0) {
    if (strcmp(method_name, "GetPublicKey") == 0) {
      handle_get_public_key(ctx, agent_id, invocation);
    } else if (strcmp(method_name, "SignEvent") == 0) {
      handle_sign_event(ctx, agent_id, parameters, invocation);
    } else if (strcmp(method_name, "Encrypt") == 0) {
      handle_encrypt(ctx, agent_id, parameters, invocation);
    } else if (strcmp(method_name, "Decrypt") == 0) {
      handle_decrypt(ctx, agent_id, parameters, invocation);
    } else {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.UnknownMethod", "Unknown signer method");
    }
  } else if (strcmp(interface_name, "net.signet.Credentials") == 0) {
    if (strcmp(method_name, "GetSession") == 0) {
      handle_get_session(ctx, agent_id, parameters, invocation);
    } else if (strcmp(method_name, "GetToken") == 0) {
      handle_get_token(ctx, parameters, invocation);
    } else if (strcmp(method_name, "ListCredentials") == 0) {
      handle_list_credentials(ctx, agent_id, invocation);
    } else {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.UnknownMethod", "Unknown credentials method");
    }
  } else {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.UnknownInterface", "Unknown interface");
  }
}
