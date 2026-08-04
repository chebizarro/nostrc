/* SPDX-License-Identifier: MIT
 *
 * store_secrets.h - Extended secret storage for Signet v2.
 *
 * Supports multiple credential types: nostr_nsec, ssh_key, api_token,
 * credential, certificate. All payloads are envelope-encrypted at rest.
 */

#ifndef SIGNET_STORE_SECRETS_H
#define SIGNET_STORE_SECRETS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SignetStore;

/**
 * SignetSecretType:
 * @SIGNET_SECRET_NOSTR_NSEC: signet secret nostr nsec
 * @SIGNET_SECRET_SSH_KEY: signet secret ssh key
 * @SIGNET_SECRET_API_TOKEN: signet secret api token
 * @SIGNET_SECRET_CREDENTIAL: signet secret credential
 * @SIGNET_SECRET_CERTIFICATE: signet secret certificate
 *
 * Credential categories supported by the extended secret store.
 *
 * Since: 1.0
 */
typedef enum {
  SIGNET_SECRET_NOSTR_NSEC = 0,
  SIGNET_SECRET_SSH_KEY,
  SIGNET_SECRET_API_TOKEN,
  SIGNET_SECRET_CREDENTIAL,
  SIGNET_SECRET_CERTIFICATE,
} SignetSecretType;

/**
 * SignetSecretRecord:
 * @id: record identifier.
 * @agent_id: agent identifier.
 * @agent_pubkey: agent public key.
 * @secret_type: secret type value.
 * @label: human-readable label.
 * @payload: decrypted; mlock'd.
 * @payload_len: length of @payload in bytes.
 * @policy_id: associated policy identifier.
 * @created_at: creation time as Unix seconds.
 * @rotated_at: rotated at value.
 * @expires_at: expiry time as Unix seconds, or 0 for no expiry.
 * @version: record version.
 * @active_version: currently active version.
 *
 * Decrypted extended secret record returned by the store.
 *
 * Ownership: clear instances with the corresponding *_clear() function to release heap data and wipe secrets where applicable.
 *
 * Since: 1.0
 */
typedef struct {
  char *id;
  char *agent_id;
  char *agent_pubkey;
  SignetSecretType secret_type;
  char *label;
  uint8_t *payload;         /* decrypted; mlock'd */
  size_t payload_len;
  char *policy_id;
  int64_t created_at;
  int64_t rotated_at;
  int64_t expires_at;
  int version;
  int active_version;
} SignetSecretRecord;

typedef enum {
  SIGNET_SECRET_OK = 0,
  SIGNET_SECRET_NOT_FOUND = 1,
  SIGNET_SECRET_EXISTS = 2,
  SIGNET_SECRET_EXPIRED = 3,
  SIGNET_SECRET_REVOKED = 4,
  SIGNET_SECRET_NOT_REVOKED = 5,
  SIGNET_SECRET_ERROR = -1,
} SignetSecretResult;

typedef enum {
  SIGNET_SECRET_STATUS_ACTIVE = 0,
  SIGNET_SECRET_STATUS_EXPIRED,
  SIGNET_SECRET_STATUS_REVOKED,
} SignetSecretStatus;

/* Payload-free lifecycle metadata. None of these fields contain ciphertext,
 * nonces, payload hashes, payload lengths, or decrypted secret material. */
typedef struct {
  char *id;
  char *agent_id;
  SignetSecretType secret_type;
  char *label;
  char *policy_id;
  char *provenance;
  char *created_by;
  int64_t created_at;
  int64_t rotated_at;
  int64_t expires_at;
  int64_t revoked_at;
  int version;
  int active_version;
  SignetSecretStatus status;
} SignetSecretMetadata;

/* Map secret type to/from string. */
/**
 * signet_secret_type_to_string:
 * @t: t
 *
 * Map secret type to/from string.
 *
 * Returns: (transfer none) (nullable): a borrowed pointer owned by the callee
 *
 * Since: 1.0
 */
const char *signet_secret_type_to_string(SignetSecretType t);
/**
 * signet_secret_type_from_string:
 * @s: (not nullable): a #SignetNip46Server
 *
 * signet secret type from string.
 *
 * Returns: (transfer none) (nullable): a static string, or %NULL when unknown
 *
 * Since: 1.0
 */
SignetSecretType signet_secret_type_from_string(const char *s);

/* Strict parser for untrusted lifecycle requests. */
bool signet_secret_type_parse(const char *s, SignetSecretType *out_type);

/* Stable identifier derived only from owner, type, and label. */
int signet_secret_id_generate(const char *agent_id,
                              SignetSecretType secret_type,
                              const char *label,
                              char out_id[70]);

/* Insert-only lifecycle creation/import. The owner pubkey is resolved from the
 * agents table; caller-supplied pubkeys are never trusted. */
SignetSecretResult signet_store_create_secret(
    struct SignetStore *store,
    const char *agent_id,
    SignetSecretType secret_type,
    const char *label,
    const uint8_t *payload,
    size_t payload_len,
    const char *policy_id,
    int64_t expires_at,
    const char *provenance,
    const char *created_by,
    int64_t now,
    SignetSecretMetadata *out_metadata);

/* Metadata-only administrative inspection/listing. agent_id may be NULL for
 * a provisioner listing all credentials. */
SignetSecretResult signet_store_get_secret_metadata(
    struct SignetStore *store,
    const char *id,
    int64_t now,
    SignetSecretMetadata *out_metadata);
SignetSecretResult signet_store_list_secret_metadata(
    struct SignetStore *store,
    const char *agent_id,
    int64_t now,
    SignetSecretMetadata **out_metadata,
    size_t *out_count);

/* Expiry/revocation-aware retrieval. No payload is decrypted when unavailable. */
SignetSecretResult signet_store_get_secret_at(
    struct SignetStore *store,
    const char *id,
    int64_t now,
    SignetSecretRecord *out_record);

/* Lifecycle mutations. Rotation preserves the previous encrypted version in
 * secret_versions within the same transaction. */
SignetSecretResult signet_store_rotate_secret_ex(
    struct SignetStore *store,
    const char *id,
    const uint8_t *new_payload,
    size_t new_payload_len,
    bool replace_expires_at,
    int64_t expires_at,
    int64_t now,
    SignetSecretMetadata *out_metadata);
SignetSecretResult signet_store_revoke_secret(
    struct SignetStore *store,
    const char *id,
    int64_t now,
    SignetSecretMetadata *out_metadata);
SignetSecretResult signet_store_delete_revoked_secret(
    struct SignetStore *store,
    const char *id);
int signet_store_secret_history_count(struct SignetStore *store,
                                      const char *id);

const char *signet_secret_status_to_string(SignetSecretStatus status);
void signet_secret_metadata_clear(SignetSecretMetadata *metadata);
void signet_secret_metadata_list_free(SignetSecretMetadata *metadata,
                                      size_t count);

/* Legacy explicit-ID insert. Payload is envelope-encrypted before writing.
 * Returns 0 on success, 1 when id exists, -1 on error. Never overwrites. */
/**
 * signet_store_put_secret:
 * @store: (nullable): a #SignetStore
 * @id: (not nullable): id
 * @agent_id: (not nullable): agent identifier
 * @agent_pubkey: (not nullable): agent pubkey
 * @secret_type: secret type
 * @label: (not nullable): label
 * @payload: (not nullable): payload
 * @payload_len: length of @payload in bytes
 * @policy_id: (not nullable): policy id
 * @now: current Unix time in seconds
 *
 * Store a new secret. Payload is envelope-encrypted before writing. Returns 0 on success, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_put_secret(struct SignetStore *store,
                            const char *id,
                            const char *agent_id,
                            const char *agent_pubkey,
                            SignetSecretType secret_type,
                            const char *label,
                            const uint8_t *payload,
                            size_t payload_len,
                            const char *policy_id,
                            int64_t now);

/* Retrieve and decrypt an active, unexpired secret.
 * Returns 0 on success, 1 if not found/unavailable, -1 on error. */
/**
 * signet_store_get_secret:
 * @store: (nullable): a #SignetStore
 * @id: (not nullable): id
 * @out_record: (out) (not nullable): return location for record
 *
 * Retrieve and decrypt a secret. Returns 0 on success, 1 if not found, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_get_secret(struct SignetStore *store,
                            const char *id,
                            SignetSecretRecord *out_record);

/* Delete a secret.
 * Returns 0 on success, 1 if not found, -1 on error. */
/**
 * signet_store_delete_secret:
 * @store: (nullable): a #SignetStore
 * @id: (not nullable): id
 *
 * Delete a secret. Returns 0 on success, 1 if not found, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_delete_secret(struct SignetStore *store, const char *id);

/* List secret IDs and labels for a given agent.
 * Returns 0 on success, -1 on error. */
/**
 * signet_store_list_secrets:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @out_ids: (out) (transfer full) (not nullable) (array): return location for ids
 * @out_labels: (out) (transfer full) (not nullable) (array): return location for labels
 * @out_count: (out) (not nullable): return location for the number of elements
 *
 * List secret IDs and labels for a given agent. Returns 0 on success, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_list_secrets(struct SignetStore *store,
                              const char *agent_id,
                              char ***out_ids,
                              char ***out_labels,
                              size_t *out_count);

/* Rotate a secret: insert new version, update active_version.
 * Old version remains in secret_versions table.
 * Returns 0 on success, -1 on error. */
/**
 * signet_store_rotate_secret:
 * @store: (nullable): a #SignetStore
 * @id: (not nullable): id
 * @new_payload: (not nullable): new payload
 * @new_payload_len: length of @new_payload in bytes
 * @now: current Unix time in seconds
 *
 * Rotate a secret: insert new version, update active_version. Old version remains in secret_versions table. Returns 0 on success, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_rotate_secret(struct SignetStore *store,
                               const char *id,
                               const uint8_t *new_payload,
                               size_t new_payload_len,
                               int64_t now);

/* Free a secret record (wipes payload). Safe on NULL. */
/**
 * signet_secret_record_clear:
 * @rec: (nullable): rec
 *
 * Free a secret record (wipes payload). Safe on NULL.
 *
 * Since: 1.0
 */
void signet_secret_record_clear(SignetSecretRecord *rec);

/* Free a list of IDs/labels. Safe on NULL. */
/**
 * signet_store_free_secret_list:
 * @ids: (not nullable) (array): ids
 * @labels: (not nullable) (array): labels
 * @count: count
 *
 * Free a list of IDs/labels. Safe on NULL.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Since: 1.0
 */
void signet_store_free_secret_list(char **ids, char **labels, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_SECRETS_H */
