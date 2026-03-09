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

typedef enum {
  SIGNET_SECRET_NOSTR_NSEC = 0,
  SIGNET_SECRET_SSH_KEY,
  SIGNET_SECRET_API_TOKEN,
  SIGNET_SECRET_CREDENTIAL,
  SIGNET_SECRET_CERTIFICATE,
} SignetSecretType;

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

/* Map secret type to/from string. */
const char *signet_secret_type_to_string(SignetSecretType t);
SignetSecretType signet_secret_type_from_string(const char *s);

/* Store a new secret. Payload is envelope-encrypted before writing.
 * Returns 0 on success, -1 on error. */
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

/* Retrieve and decrypt a secret.
 * Returns 0 on success, 1 if not found, -1 on error. */
int signet_store_get_secret(struct SignetStore *store,
                            const char *id,
                            SignetSecretRecord *out_record);

/* Delete a secret.
 * Returns 0 on success, 1 if not found, -1 on error. */
int signet_store_delete_secret(struct SignetStore *store, const char *id);

/* List secret IDs and labels for a given agent.
 * Returns 0 on success, -1 on error. */
int signet_store_list_secrets(struct SignetStore *store,
                              const char *agent_id,
                              char ***out_ids,
                              char ***out_labels,
                              size_t *out_count);

/* Rotate a secret: insert new version, update active_version.
 * Old version remains in secret_versions table.
 * Returns 0 on success, -1 on error. */
int signet_store_rotate_secret(struct SignetStore *store,
                               const char *id,
                               const uint8_t *new_payload,
                               size_t new_payload_len,
                               int64_t now);

/* Free a secret record (wipes payload). Safe on NULL. */
void signet_secret_record_clear(SignetSecretRecord *rec);

/* Free a list of IDs/labels. Safe on NULL. */
void signet_store_free_secret_list(char **ids, char **labels, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_SECRETS_H */
