/* SPDX-License-Identifier: MIT
 *
 * ssh_agent.h - OpenSSH agent protocol implementation for Signet v2.
 *
 * Exposes SSH agent protocol on /run/signet/ssh-agent.sock.
 * Auth via SO_PEERCRED UID → agent_id mapping.
 * SSH_AGENTC_REQUEST_IDENTITIES returns only keys authorized per policy.
 * SSH_AGENTC_SIGN_REQUEST signs from mlock'd hot cache.
 * Supports systemd socket activation.
 */

#ifndef SIGNET_SSH_AGENT_H
#define SIGNET_SSH_AGENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * SignetSshAgent:
 * Opaque OpenSSH agent protocol server.
 *
 * Since: 1.0
 */
typedef struct SignetSshAgent SignetSshAgent;

struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetAuditLogger;

/* UID → agent_id resolver (same interface as dbus_unix). */
/**
 * SignetSshUidResolver:
 * @uid: uid
 * @user_data: (not nullable): user data
 *
 * Callback that maps a Unix uid to an agent identifier for SSH-agent access.
 *
 * Returns: (transfer full) (nullable): resolved agent identifier, or %NULL if unknown
 *
 * Since: 1.0
 */
typedef char *(*SignetSshUidResolver)(uid_t uid, void *user_data);

/**
 * SignetSshAgentConfig:
 * @socket_path: e.g. "/run/signet/ssh-agent.sock".
 * @keys: borrowed key-store dependency.
 * @policy: borrowed policy dependency.
 * @audit: borrowed audit logger dependency.
 * @uid_resolver: uid resolver value.
 * @uid_resolver_data: uid resolver data value.
 *
 * Configuration for the Signet SSH-agent transport.
 *
 * Since: 1.0
 */
typedef struct {
  const char *socket_path;    /* e.g. "/run/signet/ssh-agent.sock" */
  struct SignetKeyStore *keys;
  struct SignetPolicyRegistry *policy;
  struct SignetAuditLogger *audit;
  SignetSshUidResolver uid_resolver;
  void *uid_resolver_data;
} SignetSshAgentConfig;

/* Create SSH agent server. Returns NULL on OOM. */
/**
 * signet_ssh_agent_new:
 * @cfg: (nullable): configuration to use
 *
 * Create SSH agent server. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetSshAgent *signet_ssh_agent_new(const SignetSshAgentConfig *cfg);

/* Free SSH agent. Safe on NULL. */
/**
 * signet_ssh_agent_free:
 * @sa: (nullable): a #SignetSshAgent
 *
 * Free SSH agent. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_ssh_agent_free(SignetSshAgent *sa);

/* Start listening on the socket. Returns 0 on success, -1 on failure. */
/**
 * signet_ssh_agent_start:
 * @sa: (not nullable): a #SignetSshAgent
 *
 * Start listening on the socket. Returns 0 on success, -1 on failure.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_ssh_agent_start(SignetSshAgent *sa);

/* Stop listening. Safe to call multiple times. */
/**
 * signet_ssh_agent_stop:
 * @sa: (nullable): a #SignetSshAgent
 *
 * Stop listening. Safe to call multiple times.
 *
 * Since: 1.0
 */
void signet_ssh_agent_stop(SignetSshAgent *sa);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_SSH_AGENT_H */
