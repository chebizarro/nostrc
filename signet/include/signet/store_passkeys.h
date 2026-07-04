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

/**
 * SignetStore:
 * Opaque handle for the SQLCipher-backed Signet persistent store.
 *
 * Since: 1.0
 */
typedef struct SignetStore SignetStore;

/**
 * SIGNET_PASSKEY_PSK_LEN:
 *
 * Required length in bytes for the fleet passkey sync pre-shared key.
 *
 * Since: 1.0
 */
#define SIGNET_PASSKEY_PSK_LEN 32
/**
 * SIGNET_PASSKEY_RP_ID_HASH_LEN:
 *
 * Length in bytes of a WebAuthn relying-party ID hash.
 *
 * Since: 1.0
 */
#define SIGNET_PASSKEY_RP_ID_HASH_LEN 32
/**
 * SIGNET_PASSKEY_AAGUID_LEN:
 *
 * Length in bytes of a WebAuthn authenticator AAGUID.
 *
 * Since: 1.0
 */
#define SIGNET_PASSKEY_AAGUID_LEN 16
/**
 * SIGNET_PASSKEY_COSE_ALG_ES256:
 *
 * COSE algorithm identifier for ES256.
 *
 * Since: 1.0
 */
#define SIGNET_PASSKEY_COSE_ALG_ES256 (-7)
/**
 * SIGNET_PASSKEY_PAYLOAD_VERSION:
 *
 * Current encrypted passkey payload format version.
 *
 * Since: 1.0
 */
#define SIGNET_PASSKEY_PAYLOAD_VERSION 1
/**
 * SIGNET_PASSKEY_EXPORT_FORMAT_VERSION:
 *
 * Current passkey export container format version.
 *
 * Since: 1.0
 */
#define SIGNET_PASSKEY_EXPORT_FORMAT_VERSION 1

/* Input for creating one credential. sign_count is always persisted as 0. */
/**
 * SignetPasskeyCreate:
 * @credential_id: credential identifier bytes.
 * @credential_id_len: length of @credential_id in bytes.
 * @agent_id: agent identifier.
 * @rp_id: WebAuthn relying-party identifier.
 * @user_handle: WebAuthn user handle bytes.
 * @user_handle_len: length of @user_handle in bytes.
 * @aaguid: 16 bytes.
 * @discoverable: whether the credential is discoverable.
 * @created_at: creation time as Unix seconds.
 * @backend_id: e.g. "software-openssl".
 * @cose_alg: must be -7 for Phase 1.
 * @key_blob: private key blob/handle; encrypted only.
 * @key_blob_len: key blob len value.
 * @cose_public_key: cose public key value.
 * @cose_public_key_len: cose public key len value.
 * @user_name: optional RP user metadata.
 * @user_display_name: optional RP user metadata.
 *
 * Input fields for creating a passkey credential.
 *
 * Since: 1.0
 */
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

/**
 * SignetPasskeyCredential:
 * @credential_id: credential identifier bytes.
 * @credential_id_len: length of @credential_id in bytes.
 * @agent_id: agent identifier.
 * @rp_id: WebAuthn relying-party identifier.
 * @rp_id_hash: rp id hash value.
 * @user_handle: WebAuthn user handle bytes.
 * @user_handle_len: length of @user_handle in bytes.
 * @sign_count: always 0 for synced passkeys.
 * @aaguid: authenticator AAGUID bytes.
 * @discoverable: whether the credential is discoverable.
 * @created_at: creation time as Unix seconds.
 * @updated_at: updated at value.
 * @payload_version: payload version value.
 * @backend_id: backend id value.
 * @cose_alg: cose alg value.
 * @key_blob: mlock'd private key blob/handle.
 * @key_blob_len: key blob len value.
 * @cose_public_key: cose public key value.
 * @cose_public_key_len: cose public key len value.
 * @user_name: optional user name.
 * @user_display_name: optional display name.
 *
 * Decrypted passkey credential metadata and versioned payload.
 *
 * Ownership: clear instances with the corresponding *_clear() function to release heap data and wipe secrets where applicable.
 *
 * Since: 1.0
 */
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
/**
 * signet_store_passkey_create:
 * @store: (nullable): a #SignetStore
 * @credential: (not nullable): credential
 * @fleet_psk: (not nullable): fleet psk
 * @fleet_psk_len: length of @fleet_psk in bytes
 *
 * Create one credential in a single transaction. Returns 0 on success, 1 for duplicate credential_id, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_passkey_create(SignetStore *store,
                                const SignetPasskeyCreate *credential,
                                const uint8_t *fleet_psk,
                                size_t fleet_psk_len);

/* Find and decrypt a credential by credential_id.
 * Returns 0 on success, 1 if not found, -1 on error/decryption failure. */
/**
 * signet_store_passkey_find_by_credential_id:
 * @store: (nullable): a #SignetStore
 * @credential_id: (not nullable): credential id
 * @credential_id_len: length of @credential_id in bytes
 * @fleet_psk: (not nullable): fleet psk
 * @fleet_psk_len: length of @fleet_psk in bytes
 * @out: (out) (not nullable): output record to populate
 *
 * Find and decrypt a credential by credential_id. Returns 0 on success, 1 if not found, -1 on error/decryption failure.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_passkey_find_by_credential_id(SignetStore *store,
                                               const uint8_t *credential_id,
                                               size_t credential_id_len,
                                               const uint8_t *fleet_psk,
                                               size_t fleet_psk_len,
                                               SignetPasskeyCredential *out);

/* Find and decrypt discoverable credentials for an agent/RP pair.
 * Returns 0 on success (including zero rows), -1 on error/decryption failure. */
/**
 * signet_store_passkey_find_by_agent_rp:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @rp_id: (not nullable): rp id
 * @fleet_psk: (not nullable): fleet psk
 * @fleet_psk_len: length of @fleet_psk in bytes
 * @out_records: (out) (transfer full) (not nullable): return location for records
 * @out_count: (out) (not nullable): return location for the number of elements
 *
 * Find and decrypt discoverable credentials for an agent/RP pair. Returns 0 on success (including zero rows), -1 on error/decryption failure.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_passkey_find_by_agent_rp(SignetStore *store,
                                          const char *agent_id,
                                          const char *rp_id,
                                          const uint8_t *fleet_psk,
                                          size_t fleet_psk_len,
                                          SignetPasskeyCredential **out_records,
                                          size_t *out_count);

/* Check an excludeCredentials list against this agent/RP.
 * Returns 0 on query success and writes *out_has_match, -1 on error. */
/**
 * signet_store_passkey_has_excluded:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @rp_id: (not nullable): rp id
 * @credential_ids: (not nullable): credential ids
 * @credential_id_lens: (not nullable) (array): credential id lens
 * @credential_id_count: number of elements
 * @out_has_match: (out) (not nullable): return location for has match
 *
 * Check an excludeCredentials list against this agent/RP. Returns 0 on query success and writes *out_has_match, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
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
/**
 * signet_store_passkey_export_container:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @credential_id: (not nullable): credential id
 * @credential_id_len: length of @credential_id in bytes
 * @fleet_psk: (not nullable): fleet psk
 * @fleet_psk_len: length of @fleet_psk in bytes
 * @out_container: (out) (transfer full) (not nullable): return location for container
 * @out_container_len: (out) (not nullable): return location for container len
 *
 * Export one credential into a versioned, self-describing binary container. The container carries lookup metadata plus the PSK-wrapped payload+nonce as stored, so importing with a different fleet PSK fails authentication. Returns 0 on success, 1 if not found/not owned by agent_id, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
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
/**
 * signet_store_passkey_import_container:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @container: (not nullable): container
 * @container_len: length of @container in bytes
 * @fleet_psk: (not nullable): fleet psk
 * @fleet_psk_len: length of @fleet_psk in bytes
 * @now: current Unix time in seconds
 * @out: (out) (not nullable): output record to populate
 *
 * Import a container for agent_id after validating it decrypts under fleet_psk. Returns 0 on success, 1 for duplicate credential_id, -1 on invalid container, wrong PSK, or storage error. On success, out (optional) is decrypted.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_passkey_import_container(SignetStore *store,
                                          const char *agent_id,
                                          const uint8_t *container,
                                          size_t container_len,
                                          const uint8_t *fleet_psk,
                                          size_t fleet_psk_len,
                                          int64_t now,
                                          SignetPasskeyCredential *out);

/**
 * signet_passkey_export_container_free:
 * @container: (nullable): container
 *
 * signet passkey export container free.
 *
 * Since: 1.0
 */
void signet_passkey_export_container_free(uint8_t *container);

/**
 * signet_passkey_credential_clear:
 * @credential: (nullable): credential
 *
 * signet passkey credential clear.
 *
 * Since: 1.0
 */
void signet_passkey_credential_clear(SignetPasskeyCredential *credential);
/**
 * signet_passkey_credential_list_free:
 * @records: (nullable): records
 * @count: count
 *
 * signet passkey credential list free.
 *
 * Since: 1.0
 */
void signet_passkey_credential_list_free(SignetPasskeyCredential *records,
                                         size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_PASSKEYS_H */
