/* SPDX-License-Identifier: MIT
 *
 * vault_client.h - HashiCorp Vault KV v2 client for Signet.
 *
 * This module is responsible for Vault HTTP interactions (KV v2 data endpoints).
 * Phase 1: API + stub implementation; later phases implement real libcurl calls.
 */

#ifndef SIGNET_VAULT_CLIENT_H
#define SIGNET_VAULT_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct SignetVaultClient SignetVaultClient;

typedef struct {
  const char *base_url;        /* e.g. https://vault:8200 */
  const char *token;           /* Vault token (sensitive); do not log. */
  const char *ca_bundle_path;  /* optional */
  const char *namespace_name;  /* optional: X-Vault-Namespace */
  uint32_t timeout_ms;         /* request timeout */
} SignetVaultClientConfig;

typedef struct {
  long http_status;
  char *body;      /* heap string (may be NULL) */
  char *error_msg; /* heap string (may be NULL) */
} SignetVaultResponse;

/* Create Vault client. Returns NULL on failure. */
SignetVaultClient *signet_vault_client_new(const SignetVaultClientConfig *cfg);

/* Free Vault client. Safe on NULL. */
void signet_vault_client_free(SignetVaultClient *c);

/* Clear/free response buffers. Safe on NULL/empty. */
void signet_vault_response_clear(SignetVaultResponse *r);

/* KV v2: Read secret at mount/path.
 * GET /v1/<mount>/data/<path> */
bool signet_vault_kv2_read(SignetVaultClient *c,
                           const char *mount,
                           const char *path,
                           SignetVaultResponse *out);

/* KV v2: Write secret (data object) at mount/path.
 * POST /v1/<mount>/data/<path>
 * Body: {"data": <data_json_object>}
 *
 * data_json_object must be a JSON object string like: {"k":"v"}.
 */
bool signet_vault_kv2_write(SignetVaultClient *c,
                            const char *mount,
                            const char *path,
                            const char *data_json_object,
                            SignetVaultResponse *out);

/* KV v2: Delete latest version at mount/path.
 * DELETE /v1/<mount>/data/<path> */
bool signet_vault_kv2_delete_latest(SignetVaultClient *c,
                                    const char *mount,
                                    const char *path,
                                    SignetVaultResponse *out);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_VAULT_CLIENT_H */