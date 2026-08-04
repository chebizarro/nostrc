/* SPDX-License-Identifier: MIT
 *
 * credential_access.c - Unified credential access control.
 *
 * See credential_access.h for the enforcement order. Security posture is
 * fail-closed everywhere: a missing policy registry denies, an unknown agent
 * denies, an unauditable success is rolled back and denied.
 */

#include "signet/credential_access.h"

#include "signet/capability.h"
#include "signet/revocation.h"
#include "signet/store.h"
#include "signet/store_audit.h"
#include "signet/store_leases.h"
#include "signet/audit_logger.h"

#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>
#include <sodium.h>

const char *signet_cred_access_reason(SignetCredAccessStatus status) {
  switch (status) {
    case SIGNET_CRED_ACCESS_OK:            return "ok";
    case SIGNET_CRED_ACCESS_NOT_FOUND:     return "not_found";
    case SIGNET_CRED_ACCESS_EXPIRED:       return "expired";
    case SIGNET_CRED_ACCESS_REVOKED:       return "revoked";
    case SIGNET_CRED_ACCESS_NO_CAPABILITY: return "no_capability";
    case SIGNET_CRED_ACCESS_RATE_LIMITED:  return "rate_limited";
    case SIGNET_CRED_ACCESS_DENY_LISTED:   return "deny_listed";
    case SIGNET_CRED_ACCESS_NOT_OWNER:     return "not_owner";
    case SIGNET_CRED_ACCESS_TYPE_DENIED:   return "type_denied";
    case SIGNET_CRED_ACCESS_LEASE_INVALID: return "lease_invalid";
    case SIGNET_CRED_ACCESS_ERROR:
    default:                               return "internal_error";
  }
}

/* Build the redacted audit detail. Only enumerated, non-secret identifiers go
 * in: capability name, decision, stable reason code, and lease id. Neither the
 * payload nor any caller-controlled free text is ever included, so redaction
 * holds by construction. */
static char *signet_cred_access_detail_json(const char *capability,
                                            const char *decision,
                                            const char *reason,
                                            const char *lease_id) {
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "capability");
  json_builder_add_string_value(b, capability ? capability : "");
  json_builder_set_member_name(b, "decision");
  json_builder_add_string_value(b, decision);
  json_builder_set_member_name(b, "reason");
  json_builder_add_string_value(b, reason);
  if (lease_id && lease_id[0]) {
    json_builder_set_member_name(b, "lease_id");
    json_builder_add_string_value(b, lease_id);
  }
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  JsonGenerator *gen = json_generator_new();
  char *json = NULL;
  if (root && gen) {
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, FALSE);
    json = json_generator_to_data(gen, NULL);
  }
  if (gen) g_object_unref(gen);
  if (root) json_node_free(root);
  g_object_unref(b);
  return json;
}

/* Append the mandatory hash-chain entry (and the optional JSONL record).
 * Returns 0 when the chain append succeeded. */
static int signet_cred_access_audit(const SignetCredentialAccessContext *ctx,
                                    const SignetCredentialAccessRequest *req,
                                    int64_t now,
                                    SignetCredAccessStatus status,
                                    const char *lease_id) {
  const char *decision = (status == SIGNET_CRED_ACCESS_OK) ? "allow"
                       : (status == SIGNET_CRED_ACCESS_ERROR) ? "error"
                       : "deny";
  const char *reason = signet_cred_access_reason(status);
  char *detail = signet_cred_access_detail_json(
      req && req->capability ? req->capability : "", decision, reason, lease_id);
  const char *agent = (req && req->agent_id && req->agent_id[0])
                          ? req->agent_id : "unknown";
  const char *secret_id = (req && req->credential_id && req->credential_id[0])
                              ? req->credential_id : NULL;
  const char *transport = (req && req->transport && req->transport[0])
                              ? req->transport : NULL;
  int rc = signet_audit_log_append(ctx->store, now, agent, "credential_access",
                                   secret_id, transport, detail);
  if (ctx->logger) {
    (void)signet_audit_log_common(ctx->logger, SIGNET_AUDIT_EVENT_KEY_ACCESS,
        &(SignetAuditCommonFields){ .identity = agent,
          .method = "credential_access", .decision = decision,
          .reason_code = reason }, detail);
  }
  g_free(detail);
  return rc;
}

static SignetCredAccessStatus signet_cred_access_map_secret_result(
    SignetSecretResult src) {
  switch (src) {
    case SIGNET_SECRET_NOT_FOUND: return SIGNET_CRED_ACCESS_NOT_FOUND;
    case SIGNET_SECRET_EXPIRED:   return SIGNET_CRED_ACCESS_EXPIRED;
    case SIGNET_SECRET_REVOKED:   return SIGNET_CRED_ACCESS_REVOKED;
    default:                      return SIGNET_CRED_ACCESS_ERROR;
  }
}

SignetCredAccessStatus signet_credential_access_acquire(
    const SignetCredentialAccessContext *ctx,
    const SignetCredentialAccessRequest *req,
    int64_t now,
    SignetCredentialAccessGrant *out_grant) {
  if (!ctx || !ctx->store || !req || !out_grant) return SIGNET_CRED_ACCESS_ERROR;
  memset(out_grant, 0, sizeof(*out_grant));

  SignetCredAccessStatus status = SIGNET_CRED_ACCESS_ERROR;
  char issued_lease[33] = {0};

  /* 0) Request shape: an authenticated caller, a target, and an EXPLICIT
   * capability are mandatory. */
  if (!req->agent_id || !req->agent_id[0] ||
      !req->credential_id || !req->credential_id[0] ||
      !req->capability || !req->capability[0]) {
    status = SIGNET_CRED_ACCESS_ERROR;
    goto audited_out;
  }

  /* 1) Deny-list precedence: resolve the caller's pubkey from custody
   * metadata (never from the request) and refuse deny-listed identities
   * before any capability grant is considered. An unknown caller identity
   * fails closed as not-owner (it cannot own anything). */
  {
    SignetAgentMeta meta;
    memset(&meta, 0, sizeof(meta));
    int arc = signet_store_get_agent_meta(ctx->store, req->agent_id, &meta);
    if (arc < 0) {
      status = SIGNET_CRED_ACCESS_ERROR;
      goto audited_out;
    }
    if (arc == 1) {
      signet_agent_meta_clear(&meta);
      status = SIGNET_CRED_ACCESS_NOT_OWNER;
      goto audited_out;
    }
    bool denied = false;
    if (ctx->deny && meta.pubkey && meta.pubkey[0])
      denied = signet_deny_list_contains(ctx->deny, meta.pubkey);
    signet_agent_meta_clear(&meta);
    if (denied) {
      status = SIGNET_CRED_ACCESS_DENY_LISTED;
      goto audited_out;
    }
  }

  /* 2) Explicit capability. No policy registry means nothing was ever
   * granted: fail closed. */
  if (!ctx->policy ||
      !signet_policy_has_capability(ctx->policy, req->agent_id,
                                    req->capability)) {
    status = SIGNET_CRED_ACCESS_NO_CAPABILITY;
    goto audited_out;
  }

  /* 3) Rate limit. */
  if (!signet_policy_rate_limit_check(ctx->policy, req->agent_id,
                                      req->capability)) {
    status = SIGNET_CRED_ACCESS_RATE_LIMITED;
    goto audited_out;
  }

  /* 4) Metadata-only gate: ownership and status are decided BEFORE any
   * decryption. Revocation takes precedence over expiry (both derive from
   * the metadata status). */
  SignetSecretType secret_type;
  int64_t secret_expires_at = 0;
  {
    SignetSecretMetadata smeta;
    memset(&smeta, 0, sizeof(smeta));
    SignetSecretResult src = signet_store_get_secret_metadata(
        ctx->store, req->credential_id, now, &smeta);
    if (src != SIGNET_SECRET_OK) {
      signet_secret_metadata_clear(&smeta);
      status = signet_cred_access_map_secret_result(src);
      goto audited_out;
    }
    bool owner = smeta.agent_id && strcmp(smeta.agent_id, req->agent_id) == 0;
    SignetSecretStatus sstatus = smeta.status;
    secret_type = smeta.secret_type;
    secret_expires_at = smeta.expires_at;
    signet_secret_metadata_clear(&smeta);
    if (!owner) {
      /* Not-owner outranks the credential's own status: a stranger learns
       * nothing about another agent's credential lifecycle. */
      status = SIGNET_CRED_ACCESS_NOT_OWNER;
      goto audited_out;
    }
    if (sstatus == SIGNET_SECRET_STATUS_REVOKED) {
      status = SIGNET_CRED_ACCESS_REVOKED;
      goto audited_out;
    }
    if (sstatus == SIGNET_SECRET_STATUS_EXPIRED) {
      status = SIGNET_CRED_ACCESS_EXPIRED;
      goto audited_out;
    }
  }

  /* 5) Type deny rules. */
  if (!signet_policy_type_allowed(ctx->policy, req->agent_id,
                                  signet_secret_type_to_string(secret_type))) {
    status = SIGNET_CRED_ACCESS_TYPE_DENIED;
    goto audited_out;
  }

  /* 6) One-use lease burn: a presented lease is consumed atomically. A lease
   * that is already burned, expired, revoked, or bound to a different
   * agent/credential fails closed. */
  if (req->lease_id && req->lease_id[0]) {
    int lrc = signet_store_consume_lease(ctx->store, req->lease_id,
                                         req->credential_id, req->agent_id,
                                         now);
    if (lrc != 0) {
      status = (lrc == 1) ? SIGNET_CRED_ACCESS_LEASE_INVALID
                          : SIGNET_CRED_ACCESS_ERROR;
      goto audited_out;
    }
  }

  /* 7) Decrypt (owner and status already proven on metadata; re-checked here
   * against races). */
  {
    SignetSecretResult src = signet_store_get_secret_at(
        ctx->store, req->credential_id, now, &out_grant->record);
    if (src != SIGNET_SECRET_OK) {
      status = signet_cred_access_map_secret_result(src);
      goto audited_out;
    }
    /* Paranoia: the row's owner must still be the caller. */
    if (!out_grant->record.agent_id ||
        strcmp(out_grant->record.agent_id, req->agent_id) != 0) {
      signet_secret_record_clear(&out_grant->record);
      status = SIGNET_CRED_ACCESS_NOT_OWNER;
      goto audited_out;
    }
  }

  /* 8) Tracking lease. A grant we cannot track is a grant we do not make. */
  if (req->issue_lease) {
    uint8_t raw[16];
    randombytes_buf(raw, sizeof(raw));
    for (int i = 0; i < 16; i++)
      g_snprintf(issued_lease + i * 2, 3, "%02x", raw[i]);
    sodium_memzero(raw, sizeof(raw));
    int64_t ttl = req->lease_ttl_seconds > 0 ? req->lease_ttl_seconds : 3600;
    int64_t lease_expires = now + ttl;
    if (secret_expires_at > 0 && lease_expires > secret_expires_at)
      lease_expires = secret_expires_at;
    char *meta = signet_cred_access_detail_json(req->capability, "allow", "ok",
                                                NULL);
    int lrc = signet_store_issue_lease(ctx->store, issued_lease,
                                       req->credential_id, req->agent_id,
                                       now, lease_expires, meta);
    g_free(meta);
    if (lrc != 0) {
      signet_secret_record_clear(&out_grant->record);
      issued_lease[0] = '\0';
      status = SIGNET_CRED_ACCESS_ERROR;
      goto audited_out;
    }
    out_grant->lease_id = g_strdup(issued_lease);
    out_grant->lease_expires_at = lease_expires;
  }

  status = SIGNET_CRED_ACCESS_OK;

audited_out:
  /* Mandatory chain audit on EVERY outcome. A success that cannot be audited
   * is rolled back: burn the lease we just issued, wipe the payload, deny. */
  if (signet_cred_access_audit(ctx, req, now, status,
                               issued_lease[0] ? issued_lease : NULL) != 0) {
    if (status == SIGNET_CRED_ACCESS_OK) {
      if (out_grant->lease_id)
        (void)signet_store_revoke_lease(ctx->store, out_grant->lease_id, now);
      signet_credential_access_grant_clear(out_grant);
      status = SIGNET_CRED_ACCESS_ERROR;
    }
  }
  return status;
}

void signet_credential_access_grant_clear(SignetCredentialAccessGrant *grant) {
  if (!grant) return;
  signet_secret_record_clear(&grant->record);
  g_free(grant->lease_id);
  memset(grant, 0, sizeof(*grant));
}
