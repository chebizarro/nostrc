/* SPDX-License-Identifier: MIT
 *
 * ssh_agent.c - OpenSSH agent protocol (RFC draft-miller-ssh-agent).
 *
 * Implements the SSH agent wire protocol over a Unix domain socket.
 * Only the required subset:
 *   SSH_AGENTC_REQUEST_IDENTITIES (11)  → SSH_AGENT_IDENTITIES_ANSWER (12)
 *   SSH_AGENTC_SIGN_REQUEST (13)        → SSH_AGENT_SIGN_RESPONSE (14)
 *   Everything else                      → SSH_AGENT_FAILURE (5)
 *
 * Ed25519 keys are derived from the agent's Nostr keypair (same curve).
 * Signing uses libsodium crypto_sign_ed25519_detached on the mlock'd key.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "signet/ssh_agent.h"
#include "signet/key_store.h"
#include "signet/capability.h"
#include "signet/audit_logger.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>

#include <glib.h>
#include <sodium.h>

/* SSH agent protocol constants. */
#define SSH_AGENTC_REQUEST_IDENTITIES 11
#define SSH_AGENT_IDENTITIES_ANSWER   12
#define SSH_AGENTC_SIGN_REQUEST       13
#define SSH_AGENT_SIGN_RESPONSE       14
#define SSH_AGENT_FAILURE             5

/* Ed25519 key blob: "ssh-ed25519" + 32-byte pubkey. */
#define SSH_ED25519_KEYTYPE "ssh-ed25519"
#define SSH_ED25519_KEYTYPE_LEN 11

struct SignetSshAgent {
  char *socket_path;
  SignetKeyStore *keys;
  SignetPolicyRegistry *policy;
  SignetAuditLogger *audit;
  SignetSshUidResolver uid_resolver;
  void *uid_resolver_data;

  int listen_fd;
  GThread *accept_thread;
  volatile gint running;
};

/* ----------------------------- wire helpers ------------------------------- */

static void put_u32(uint8_t *buf, uint32_t val) {
  buf[0] = (val >> 24) & 0xFF;
  buf[1] = (val >> 16) & 0xFF;
  buf[2] = (val >> 8) & 0xFF;
  buf[3] = val & 0xFF;
}

static uint32_t get_u32(const uint8_t *buf) {
  return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
         ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static int read_exact(int fd, void *buf, size_t n) {
  uint8_t *p = (uint8_t *)buf;
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) return -1;
    p += r;
    n -= (size_t)r;
  }
  return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
  const uint8_t *p = (const uint8_t *)buf;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

/* Send an SSH agent message (length-prefixed). */
static int send_msg(int fd, const uint8_t *data, uint32_t len) {
  uint8_t hdr[4];
  put_u32(hdr, len);
  if (write_all(fd, hdr, 4) < 0) return -1;
  return write_all(fd, data, len);
}

static int send_failure(int fd) {
  uint8_t msg[] = { SSH_AGENT_FAILURE };
  return send_msg(fd, msg, 1);
}

/* ----------------------------- key helpers -------------------------------- */

/* Build SSH Ed25519 public key blob. Returns heap-allocated blob. */
static uint8_t *build_ed25519_key_blob(const uint8_t *pk32, size_t *out_len) {
  /* Format: uint32(keytype_len) + keytype + uint32(32) + pk */
  size_t len = 4 + SSH_ED25519_KEYTYPE_LEN + 4 + 32;
  uint8_t *blob = (uint8_t *)g_malloc(len);
  uint8_t *p = blob;

  put_u32(p, SSH_ED25519_KEYTYPE_LEN); p += 4;
  memcpy(p, SSH_ED25519_KEYTYPE, SSH_ED25519_KEYTYPE_LEN); p += SSH_ED25519_KEYTYPE_LEN;
  put_u32(p, 32); p += 4;
  memcpy(p, pk32, 32); p += 32;

  *out_len = len;
  return blob;
}

/* ----------------------------- message handlers --------------------------- */

static int handle_request_identities(SignetSshAgent *sa, int client_fd,
                                      const char *agent_id) {
  /* List keys authorized for this agent. */
  char **ids = NULL;
  size_t count = 0;
  if (signet_key_store_list_agents(sa->keys, &ids, &count) != 0)
    return send_failure(client_fd);

  /* Build identities response. */
  /* For simplicity, expose only the agent's own key (not all fleet keys). */
  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  bool found = signet_key_store_load_agent_key(sa->keys, agent_id, &lk);

  /* Free the list since we only use agent's own key. */
  for (size_t i = 0; i < count; i++) g_free(ids[i]);
  g_free(ids);

  if (!found) {
    /* No key — return empty list. */
    uint8_t msg[5];
    msg[0] = SSH_AGENT_IDENTITIES_ANSWER;
    put_u32(msg + 1, 0);
    return send_msg(client_fd, msg, 5);
  }

  /* Derive ed25519 public key from secret key. */
  uint8_t pk[32];
  crypto_scalarmult_ed25519_base_noclamp(pk, lk.secret_key);
  signet_loaded_key_clear(&lk);

  size_t blob_len = 0;
  uint8_t *blob = build_ed25519_key_blob(pk, &blob_len);
  sodium_memzero(pk, sizeof(pk));

  /* Comment: agent_id. */
  size_t comment_len = strlen(agent_id);

  /* Response: type(1) + nkeys(4) + blob_len(4) + blob + comment_len(4) + comment */
  size_t msg_len = 1 + 4 + 4 + blob_len + 4 + comment_len;
  uint8_t *msg = (uint8_t *)g_malloc(msg_len);
  uint8_t *p = msg;

  *p++ = SSH_AGENT_IDENTITIES_ANSWER;
  put_u32(p, 1); p += 4; /* nkeys = 1 */
  put_u32(p, (uint32_t)blob_len); p += 4;
  memcpy(p, blob, blob_len); p += blob_len;
  put_u32(p, (uint32_t)comment_len); p += 4;
  memcpy(p, agent_id, comment_len);

  int rc = send_msg(client_fd, msg, (uint32_t)msg_len);
  g_free(blob);
  g_free(msg);
  return rc;
}

static int handle_sign_request(SignetSshAgent *sa, int client_fd,
                                 const char *agent_id,
                                 const uint8_t *data, uint32_t data_len) {
  /* Parse: key_blob_len(4) + key_blob + data_len(4) + data + flags(4) */
  if (data_len < 8) return send_failure(client_fd);

  const uint8_t *dp = data;
  uint32_t kb_len = get_u32(dp); dp += 4;
  if (kb_len + 8 > data_len) return send_failure(client_fd);
  dp += kb_len; /* skip key blob */

  uint32_t msg_len = get_u32(dp); dp += 4;
  if ((uint32_t)(dp - data) + msg_len > data_len) return send_failure(client_fd);
  const uint8_t *sign_data = dp;

  /* Capability check. */
  if (sa->policy && !signet_policy_has_capability(sa->policy, agent_id,
                                                     SIGNET_CAP_NOSTR_SIGN))
    return send_failure(client_fd);

  /* Load agent key. */
  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  if (!signet_key_store_load_agent_key(sa->keys, agent_id, &lk))
    return send_failure(client_fd);

  /* Sign with ed25519. Build the full 64-byte ed25519 secret key
   * (sk32 || pk32) that libsodium expects. */
  uint8_t ed_sk[64];
  uint8_t pk[32];
  crypto_scalarmult_ed25519_base_noclamp(pk, lk.secret_key);
  memcpy(ed_sk, lk.secret_key, 32);
  memcpy(ed_sk + 32, pk, 32);
  signet_loaded_key_clear(&lk);

  uint8_t sig[64];
  unsigned long long sig_len_actual;
  int sign_rc = crypto_sign_ed25519_detached(sig, &sig_len_actual,
                                               sign_data, msg_len, ed_sk);
  sodium_memzero(ed_sk, sizeof(ed_sk));
  sodium_memzero(pk, sizeof(pk));

  if (sign_rc != 0)
    return send_failure(client_fd);

  /* Build signature blob:
   * uint32(sigtype_len) + "ssh-ed25519" + uint32(64) + sig */
  size_t sig_blob_len = 4 + SSH_ED25519_KEYTYPE_LEN + 4 + 64;
  uint8_t *sig_blob = (uint8_t *)g_malloc(sig_blob_len);
  uint8_t *sp = sig_blob;
  put_u32(sp, SSH_ED25519_KEYTYPE_LEN); sp += 4;
  memcpy(sp, SSH_ED25519_KEYTYPE, SSH_ED25519_KEYTYPE_LEN); sp += SSH_ED25519_KEYTYPE_LEN;
  put_u32(sp, 64); sp += 4;
  memcpy(sp, sig, 64);

  /* Response: type(1) + sig_blob_len(4) + sig_blob */
  size_t resp_len = 1 + 4 + sig_blob_len;
  uint8_t *resp = (uint8_t *)g_malloc(resp_len);
  resp[0] = SSH_AGENT_SIGN_RESPONSE;
  put_u32(resp + 1, (uint32_t)sig_blob_len);
  memcpy(resp + 5, sig_blob, sig_blob_len);

  int rc = send_msg(client_fd, resp, (uint32_t)resp_len);
  g_free(sig_blob);
  g_free(resp);
  return rc;
}

/* ----------------------------- client handler ----------------------------- */

static void handle_client(SignetSshAgent *sa, int client_fd) {
  /* Resolve agent_id from socket peer UID. */
  uid_t peer_uid = (uid_t)-1;
#if defined(__linux__)
  struct ucred cred;
  socklen_t cred_len = sizeof(cred);
  if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0)
    peer_uid = cred.uid;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  /* macOS/FreeBSD: use getpeereid(). */
  uid_t euid;
  gid_t egid;
  if (getpeereid(client_fd, &euid, &egid) == 0)
    peer_uid = euid;
#endif
  if (peer_uid == (uid_t)-1) {
    close(client_fd);
    return;
  }

  char *agent_id = NULL;
  if (sa->uid_resolver) {
    agent_id = sa->uid_resolver(peer_uid, sa->uid_resolver_data);
  }
  if (!agent_id) {
    close(client_fd);
    return;
  }

  /* Message loop. */
  while (g_atomic_int_get(&sa->running)) {
    /* Read message header: 4-byte length. */
    uint8_t hdr[4];
    if (read_exact(client_fd, hdr, 4) < 0) break;

    uint32_t msg_len = get_u32(hdr);
    if (msg_len == 0 || msg_len > 256 * 1024) break;

    uint8_t *msg = (uint8_t *)g_malloc(msg_len);
    if (read_exact(client_fd, msg, msg_len) < 0) {
      g_free(msg);
      break;
    }

    uint8_t type = msg[0];

    switch (type) {
    case SSH_AGENTC_REQUEST_IDENTITIES:
      handle_request_identities(sa, client_fd, agent_id);
      break;
    case SSH_AGENTC_SIGN_REQUEST:
      handle_sign_request(sa, client_fd, agent_id, msg + 1, msg_len - 1);
      break;
    default:
      send_failure(client_fd);
      break;
    }

    g_free(msg);
  }

  g_free(agent_id);
  close(client_fd);
}

/* ---- per-client thread data ---- */

typedef struct {
  SignetSshAgent *sa;
  int client_fd;
} SshClientThreadData;

static gpointer ssh_agent_client_thread(gpointer data) {
  SshClientThreadData *td = (SshClientThreadData *)data;
  handle_client(td->sa, td->client_fd);
  g_free(td);
  return NULL;
}

/* ----------------------------- accept thread ------------------------------ */

static gpointer ssh_agent_accept_loop(gpointer data) {
  SignetSshAgent *sa = (SignetSshAgent *)data;

  while (g_atomic_int_get(&sa->running)) {
    int client_fd = accept(sa->listen_fd, NULL, NULL);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    /* Handle each client in its own thread for concurrency. */
    SshClientThreadData *td = g_new(SshClientThreadData, 1);
    td->sa = sa;
    td->client_fd = client_fd;
    GThread *t = g_thread_new("signet-ssh-client", ssh_agent_client_thread, td);
    if (t) {
      g_thread_unref(t); /* detach — client thread manages its own lifetime */
    } else {
      close(client_fd);
      g_free(td);
    }
  }

  return NULL;
}

/* ------------------------------ public API -------------------------------- */

SignetSshAgent *signet_ssh_agent_new(const SignetSshAgentConfig *cfg) {
  if (!cfg || !cfg->keys) return NULL;

  SignetSshAgent *sa = g_new0(SignetSshAgent, 1);
  if (!sa) return NULL;

  sa->socket_path = g_strdup(cfg->socket_path
      ? cfg->socket_path : "/run/signet/ssh-agent.sock");
  sa->keys = cfg->keys;
  sa->policy = cfg->policy;
  sa->audit = cfg->audit;
  sa->uid_resolver = cfg->uid_resolver;
  sa->uid_resolver_data = cfg->uid_resolver_data;
  sa->listen_fd = -1;

  return sa;
}

void signet_ssh_agent_free(SignetSshAgent *sa) {
  if (!sa) return;
  signet_ssh_agent_stop(sa);
  g_free(sa->socket_path);
  g_free(sa);
}

int signet_ssh_agent_start(SignetSshAgent *sa) {
  if (!sa) return -1;
  if (sa->listen_fd >= 0) return 0;

  (void)unlink(sa->socket_path);

  sa->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sa->listen_fd < 0) return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sa->socket_path, sizeof(addr.sun_path) - 1);

  if (bind(sa->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sa->listen_fd);
    sa->listen_fd = -1;
    return -1;
  }

  if (listen(sa->listen_fd, 16) < 0) {
    close(sa->listen_fd);
    sa->listen_fd = -1;
    return -1;
  }

  g_atomic_int_set(&sa->running, 1);
  sa->accept_thread = g_thread_new("signet-ssh-agent", ssh_agent_accept_loop, sa);

  return 0;
}

void signet_ssh_agent_stop(SignetSshAgent *sa) {
  if (!sa) return;

  g_atomic_int_set(&sa->running, 0);

  if (sa->listen_fd >= 0) {
    shutdown(sa->listen_fd, SHUT_RDWR);
    close(sa->listen_fd);
    sa->listen_fd = -1;
  }

  if (sa->accept_thread) {
    g_thread_join(sa->accept_thread);
    sa->accept_thread = NULL;
  }

  (void)unlink(sa->socket_path);
}
