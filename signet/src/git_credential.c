/* SPDX-License-Identifier: MIT
 *
 * git_credential.c - Git credential helper protocol engine.
 *
 * The engine only moves the secret between the retriever callback and the
 * credential protocol output stream; it never logs, copies to argv/env, or
 * persists it. Buffers holding the secret are wiped before free.
 */

#include "signet/git_credential.h"
#include "signet/store_secrets.h"

#include <string.h>

#include <glib.h>
#include <sodium.h>

static void signet_git_query_clear(SignetGitCredentialQuery *q) {
  if (!q) return;
  g_free(q->protocol);
  g_free(q->host);
  g_free(q->path);
  g_free(q->username);
  memset(q, 0, sizeof(*q));
}

/* Parse "key=value" lines until a blank line or EOF. Unknown keys are
 * ignored (forward compatibility with newer git attribute sets). */
static int signet_git_parse_query(FILE *in, SignetGitCredentialQuery *q) {
  char line[4096];
  while (fgets(line, sizeof(line), in)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len == 0) break; /* blank line terminates the request */
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    const char *key = line;
    const char *value = eq + 1;
    if (strcmp(key, "protocol") == 0) {
      g_free(q->protocol);
      q->protocol = g_strdup(value);
    } else if (strcmp(key, "host") == 0) {
      g_free(q->host);
      q->host = g_strdup(value);
    } else if (strcmp(key, "path") == 0) {
      g_free(q->path);
      q->path = g_strdup(value);
    } else if (strcmp(key, "username") == 0) {
      g_free(q->username);
      q->username = g_strdup(value);
    }
    /* Wipe the line: values may include a secret on store actions. */
    sodium_memzero(line, sizeof(line));
  }
  return 0;
}

int signet_git_credential_serve(const char *action,
                                FILE *in,
                                FILE *out,
                                SignetGitCredentialLookup lookup,
                                void *user_data) {
  if (!action || !in || !out) return -1;

  SignetGitCredentialQuery q;
  memset(&q, 0, sizeof(q));
  if (signet_git_parse_query(in, &q) != 0) {
    signet_git_query_clear(&q);
    return -1;
  }

  /* store/erase: consume and succeed. Credential mutations happen only over
   * the encrypted ContextVM management channel; git cannot write or delete
   * Signet-held credentials. */
  if (strcmp(action, "get") != 0) {
    signet_git_query_clear(&q);
    return 0;
  }

  if (!lookup) {
    signet_git_query_clear(&q);
    return -1;
  }

  char *username = NULL;
  char *password = NULL;
  int rc = lookup(&q, user_data, &username, &password);
  signet_git_query_clear(&q);
  if (rc != 0 || !password || !password[0]) {
    /* Quiet miss: emit nothing so git falls through to its next helper or
     * prompts. Never distinguish deny/absent to the caller. */
    if (password) {
      sodium_memzero(password, strlen(password));
      g_free(password);
    }
    g_free(username);
    return 0;
  }

  /* The ONLY place the secret is ever written: the credential protocol
   * stream. */
  fprintf(out, "username=%s\n", username && username[0] ? username : "x-access-token");
  fprintf(out, "password=%s\n", password);
  fflush(out);

  sodium_memzero(password, strlen(password));
  g_free(password);
  g_free(username);
  return 0;
}

int signet_git_credential_derive_id(const char *agent_id,
                                    const char *host,
                                    char out_id[70]) {
  if (!agent_id || !agent_id[0] || !host || !host[0] || !out_id) return -1;
  char *label = g_strdup_printf("git:%s", host);
  int rc = signet_secret_id_generate(agent_id, SIGNET_SECRET_API_TOKEN,
                                     label, out_id);
  g_free(label);
  return rc;
}
