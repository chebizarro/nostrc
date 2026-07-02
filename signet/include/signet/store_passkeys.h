/* SPDX-License-Identifier: MIT */
/*
 * store_passkeys.h - Dedicated passkey credential vault for Signet.
 *
 * Passkeys intentionally do not reuse the generic `secrets` table: that table
 * has one-row-per-agent assumptions that do not fit WebAuthn credentials. This
 * module stores lookup metadata in passkey_credentials and wraps the versioned
 * credential payload under a caller-provided fleet PSK so rows are portable
 * across signet instances in the same fleet.
 */
#ifndef SIGNET_STORE_PASSKEYS_H
#define SIGNET_STORE_PASSKEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SignetStore SignetStore;

#define SIGNET_PASSKEY_PSK_LEN 32
#define SIGNET_PASSKEY_RP_ID_HASH_LEN 32
#define SIGNET_PASSKEY_AAGUID_LEN 16
#define SIGNET_PASSKEY_COSE_ALG_ES256 (-7)
#define SIGNET_PASSKEY_PAYLOAD_VERSION 1
#define SIGNET_PASSKEY_EXPORT_FORMAT_VERSION 1

/* Input for creating one credential. sign_count is always persisted as 0. */
typedef struct {
  const uint8_t *credential_id;
  size_t credential_id_len;
  const char *agent_id;
  const char *rp_id;
  const uint8_t *user_handle;
  size_t user_handle_len;
  const uint8_t *aaguid; /* 16 bytes */
  bool discoverable;
  int64_t created_at;

  /* Versioned encrypted payload fields. */
  const char *backend_id;       /* e.g. "software-openssl" */
  int cose_alg;                 /* must be -7 for Phase 1 */
  const uint8_t *key_blob;      /* private key blob/handle; encrypted only */
  size_t key_blob_len;
  const uint8_t *cose_public_key;
  size_t cose_public_key_len;
  const char *user_name;         /* optional RP user metadata */
  const char *user_display_name; /* optional RP user metadata */
} SignetPasskeyCreate;

typedef struct {
  uint8_t *credential_id;
  size_t credential_id_len;
  char *agent_id;
  char *rp_id;
  uint8_t rp_id_hash[SIGNET_PASSKEY_RP_ID_HASH_LEN];
  uint8_t *user_handle;
  size_t user_handle_len;
  uint32_t sign_count; /* always 0 for synced passkeys */
  uint8_t aaguid[SIGNET_PASSKEY_AAGUID_LEN];
  bool discoverable;
  int64_t created_at;
  int64_t updated_at;

  /* Decrypted versioned payload. Clear promptly with
   * signet_passkey_credential_clear(). */
  uint16_t payload_version;
  char *backend_id;
  int cose_alg;
  uint8_t *key_blob; /* mlock'd private key blob/handle */
  size_t key_blob_len;
  uint8_t *cose_public_key;
  size_t cose_public_key_len;
  char *user_name;
  char *user_display_name;
} SignetPasskeyCredential;

/* Create one credential in a single transaction.
 * Returns 0 on success, 1 for duplicate credential_id, -1 on error. */
int signet_store_passkey_create(SignetStore *store,
                                const SignetPasskeyCreate *credential,
                                const uint8_t *fleet_psk,
                                size_t fleet_psk_len);

/* Find and decrypt a credential by credential_id.
 * Returns 0 on success, 1 if not found, -1 on error/decryption failure. */
int signet_store_passkey_find_by_credential_id(SignetStore *store,
                                               const uint8_t *credential_id,
                                               size_t credential_id_len,
                                               const uint8_t *fleet_psk,
                                               size_t fleet_psk_len,
                                               SignetPasskeyCredential *out);

/* Find and decrypt discoverable credentials for an agent/RP pair.
 * Returns 0 on success (including zero rows), -1 on error/decryption failure. */
int signet_store_passkey_find_by_agent_rp(SignetStore *store,
                                          const char *agent_id,
                                          const char *rp_id,
                                          const uint8_t *fleet_psk,
                                          size_t fleet_psk_len,
                                          SignetPasskeyCredential **out_records,
                                          size_t *out_count);

/* Check an excludeCredentials list against this agent/RP.
 * Returns 0 on query success and writes *out_has_match, -1 on error. */
int signet_store_passkey_has_excluded(SignetStore *store,
                                      const char *agent_id,
                                      const char *rp_id,
                                      const uint8_t *const *credential_ids,
                                      const size_t *credential_id_lens,
                                      size_t credential_id_count,
                                      bool *out_has_match);

/* Export one credential into a versioned, self-describing binary container.
 * The container carries lookup metadata plus the PSK-wrapped payload+nonce as
 * stored, so importing with a different fleet PSK fails authentication.
 * Returns 0 on success, 1 if not found/not owned by agent_id, -1 on error. */
int signet_store_passkey_export_container(SignetStore *store,
                                          const char *agent_id,
                                          const uint8_t *credential_id,
                                          size_t credential_id_len,
                                          const uint8_t *fleet_psk,
                                          size_t fleet_psk_len,
                                          uint8_t **out_container,
                                          size_t *out_container_len);

/* Import a container for agent_id after validating it decrypts under fleet_psk.
 * Returns 0 on success, 1 for duplicate credential_id, -1 on invalid container,
 * wrong PSK, or storage error. On success, out (optional) is decrypted. */
int signet_store_passkey_import_container(SignetStore *store,
                                          const char *agent_id,
                                          const uint8_t *container,
                                          size_t container_len,
                                          const uint8_t *fleet_psk,
                                          size_t fleet_psk_len,
                                          int64_t now,
                                          SignetPasskeyCredential *out);

void signet_passkey_export_container_free(uint8_t *container);

void signet_passkey_credential_clear(SignetPasskeyCredential *credential);
void signet_passkey_credential_list_free(SignetPasskeyCredential *records,
                                         size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_PASSKEYS_H */
