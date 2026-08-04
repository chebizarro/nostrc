/* SPDX-License-Identifier: MIT
 *
 * git_credential.h - Git credential helper protocol engine for Signet.
 *
 * Implements the `git credential` helper protocol (key=value lines on stdin,
 * key=value lines on stdout) backed by a pluggable credential retriever.
 * The installed binary (signet-git-credential) plugs in a D-Bus GetToken
 * retriever, so the PAT flows: encrypted store -> signetd (capability +
 * ownership + audit) -> credential protocol stream -> git. The token is
 * NEVER passed via argv, environment, or a materialized config file, and is
 * never written anywhere except the credential protocol stream.
 *
 * Configure with:
 *   git config credential.helper '!signet-git-credential'
 *
 * The credential id is resolved from SIGNET_GIT_CREDENTIAL_ID, or derived
 * deterministically from SIGNET_AGENT_ID and the requested host via
 * signet_git_credential_derive_id() (label "git:<host>", type api_token) —
 * rotation-safe because the id is stable across rotations while retrieval
 * always yields the active version.
 */

#ifndef SIGNET_GIT_CREDENTIAL_H
#define SIGNET_GIT_CREDENTIAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

/**
 * SignetGitCredentialQuery:
 * @protocol: (nullable): "https" etc., from git.
 * @host: (nullable): remote host, from git.
 * @path: (nullable): repository path when credential.useHttpPath is set.
 * @username: (nullable): username hint from git/remote URL.
 *
 * Parsed attributes of a `git credential` request.
 *
 * Since: 1.3
 */
typedef struct {
  char *protocol;
  char *host;
  char *path;
  char *username;
} SignetGitCredentialQuery;

/**
 * SignetGitCredentialLookup:
 * @query: (not nullable): parsed request attributes.
 * @user_data: callback state.
 * @out_username: (out): username to present (g_free'd by caller).
 * @out_password: (out): secret to present (wiped + g_free'd by caller).
 *
 * Retrieve the credential for @query. Implementations MUST NOT print or log
 * the secret; the protocol engine writes it to the credential stream only.
 *
 * Returns: 0 on success, nonzero when no credential is available
 *
 * Since: 1.3
 */
typedef int (*SignetGitCredentialLookup)(const SignetGitCredentialQuery *query,
                                         void *user_data,
                                         char **out_username,
                                         char **out_password);

/**
 * signet_git_credential_serve:
 * @action: (not nullable): "get", "store", or "erase" (git passes one).
 * @in: (not nullable): credential protocol input stream.
 * @out: (not nullable): credential protocol output stream.
 * @lookup: (nullable): retriever; required for "get".
 * @user_data: passed to @lookup.
 *
 * Run one credential helper exchange. "store" and "erase" consume the input
 * and succeed as no-ops (Signet credentials are mutated only via the
 * encrypted ContextVM management channel, never by git). For "get", the
 * retrieved secret is written ONLY to @out and wiped afterwards.
 *
 * Returns: 0 on success (including a quiet "no credential" on get), -1 on
 * protocol/internal error
 *
 * Since: 1.3
 */
int signet_git_credential_serve(const char *action,
                                FILE *in,
                                FILE *out,
                                SignetGitCredentialLookup lookup,
                                void *user_data);

/**
 * signet_git_credential_derive_id:
 * @agent_id: (not nullable): owning agent.
 * @host: (not nullable): git remote host (e.g. "github.com").
 * @out_id: (out): stable credential id.
 *
 * Derive the deterministic credential id for an agent's git PAT for @host:
 * signet_secret_id_generate(agent_id, api_token, "git:<host>").
 *
 * Returns: 0 on success, -1 on error
 *
 * Since: 1.3
 */
int signet_git_credential_derive_id(const char *agent_id,
                                    const char *host,
                                    char out_id[70]);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_GIT_CREDENTIAL_H */
