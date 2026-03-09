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

typedef struct SignetSshAgent SignetSshAgent;

struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetAuditLogger;

/* UID → agent_id resolver (same interface as dbus_unix). */
typedef char *(*SignetSshUidResolver)(uid_t uid, void *user_data);

typedef struct {
  const char *socket_path;    /* e.g. "/run/signet/ssh-agent.sock" */
  struct SignetKeyStore *keys;
  struct SignetPolicyRegistry *policy;
  struct SignetAuditLogger *audit;
  SignetSshUidResolver uid_resolver;
  void *uid_resolver_data;
} SignetSshAgentConfig;

/* Create SSH agent server. Returns NULL on OOM. */
SignetSshAgent *signet_ssh_agent_new(const SignetSshAgentConfig *cfg);

/* Free SSH agent. Safe on NULL. */
void signet_ssh_agent_free(SignetSshAgent *sa);

/* Start listening on the socket. Returns 0 on success, -1 on failure. */
int signet_ssh_agent_start(SignetSshAgent *sa);

/* Stop listening. Safe to call multiple times. */
void signet_ssh_agent_stop(SignetSshAgent *sa);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_SSH_AGENT_H */
