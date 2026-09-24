/* auth_porthome.c — broker-side portable-home glue. See auth_porthome.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * Compiled only when NH_AUTH_BROKER_ENABLE_PORTHOME is defined. Without
 * it the broker returns NH_AUTH_RESULT_NOT_SUPPORTED for both
 * PROVISION_HOME and WAIT_HOME and this translation unit is not linked
 * in — so the base nostr-authd link closure never pulls in
 * libnostr_porthome or libhanami. That's the packaging gate that keeps
 * the login runtime headless-pure. */

#define _GNU_SOURCE
#include "auth_porthome.h"

#ifdef NH_AUTH_BROKER_ENABLE_PORTHOME

#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"
#include "nh_porthome_provision.h"
#include "nh_porthome_wrapkey.h"
#include "nh_porthome_sandbox.h"
#include "auth_porthome_fetch.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_types.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <openssl/crypto.h>

/* ────────────────────────────────────────────────────────────────────
 * Job registry
 *
 * A single global registry keyed on account_id. Broker is
 * single-transaction today so contention is nil, but the registry is
 * still guarded by a mutex so a follow-up concurrent broker can share
 * this code path without a rewrite. */

#define REG_CAP 8u

struct nh_auth_porthome_job {
  atomic_int refcount;
  atomic_int state;                 /* nh_auth_porthome_job_state */
  atomic_ullong bytes;
  atomic_ullong total_bytes;
  atomic_uint files;
  atomic_uint total_files;
  atomic_int cancelled;
  pthread_t thread;
  int thread_started;

  /* Snapshot of inputs (deep-copied at start; owned by the job). */
  char account_id[NH_IDENTITY_UUID_CAP];
  char tx_id[NH_AUTH_TRANSACTION_ID_HEX_LEN + 1];
  uint8_t wrap_seed[32];            /* mlock'd via mlock() at start */
  nh_identity_store *store;         /* borrowed */
  nh_auth_porthome_config config;   /* value copy of caps */

  /* Optional test hook payload — supersedes the real fetch path. */
  uint8_t *test_sealed_manifest;
  size_t   test_sealed_len;
  nh_auth_porthome_fetch_hook_fn test_fetch;
  void    *test_fetch_ctx;

  /* Progress artifact path (owned). */
  char *progress_path;
};

static pthread_mutex_t g_reg_mutex = PTHREAD_MUTEX_INITIALIZER;
static nh_auth_porthome_job *g_reg[REG_CAP];
static int g_reg_ready;

static char g_progress_dir[512];
static const uint8_t *g_next_test_sealed;
static size_t         g_next_test_sealed_len;
static nh_auth_porthome_fetch_hook_fn g_next_test_fetch;
static void          *g_next_test_fetch_ctx;

static const char *progress_dir(void) {
  return g_progress_dir[0] ? g_progress_dir : "/run/nostr-auth/greeter";
}

int nh_auth_porthome_registry_init(void) {
  pthread_mutex_lock(&g_reg_mutex);
  for (size_t i = 0; i < REG_CAP; i++) g_reg[i] = NULL;
  g_reg_ready = 1;
  pthread_mutex_unlock(&g_reg_mutex);
  return 0;
}

void nh_auth_porthome_registry_shutdown(void) {
  pthread_mutex_lock(&g_reg_mutex);
  for (size_t i = 0; i < REG_CAP; i++) {
    if (!g_reg[i]) continue;
    atomic_store(&g_reg[i]->cancelled, 1);
    /* Best-effort join; skip if never started or already joined. */
    if (g_reg[i]->thread_started) {
      pthread_mutex_unlock(&g_reg_mutex);
      pthread_join(g_reg[i]->thread, NULL);
      pthread_mutex_lock(&g_reg_mutex);
    }
    free(g_reg[i]->progress_path);
    free(g_reg[i]->test_sealed_manifest);
    OPENSSL_cleanse(g_reg[i]->wrap_seed, sizeof g_reg[i]->wrap_seed);
    free(g_reg[i]);
    g_reg[i] = NULL;
  }
  g_reg_ready = 0;
  pthread_mutex_unlock(&g_reg_mutex);
}

void nh_auth_porthome_set_progress_dir(const char *dir) {
  if (!dir || !dir[0]) { g_progress_dir[0] = '\0'; return; }
  size_t n = strlen(dir);
  if (n >= sizeof g_progress_dir) { g_progress_dir[0] = '\0'; return; }
  memcpy(g_progress_dir, dir, n + 1);
}

void nh_auth_porthome_set_test_hook(const uint8_t *sealed_manifest,
                                    size_t sealed_len,
                                    nh_auth_porthome_fetch_hook_fn fetch,
                                    void *fetch_ctx) {
  g_next_test_sealed = sealed_manifest;
  g_next_test_sealed_len = sealed_len;
  g_next_test_fetch = fetch;
  g_next_test_fetch_ctx = fetch_ctx;
}

static void job_release(nh_auth_porthome_job *j) {
  if (!j) return;
  if (atomic_fetch_sub(&j->refcount, 1) != 1) return;
  /* Last ref: caller must have already joined the thread. */
  free(j->progress_path);
  free(j->test_sealed_manifest);
  OPENSSL_cleanse(j->wrap_seed, sizeof j->wrap_seed);
  free(j);
}

static nh_auth_porthome_job *lookup_locked(const char *account_id) {
  for (size_t i = 0; i < REG_CAP; i++) {
    if (g_reg[i] && strcmp(g_reg[i]->account_id, account_id) == 0)
      return g_reg[i];
  }
  return NULL;
}

nh_auth_porthome_job *nh_auth_porthome_lookup(const char *account_id) {
  if (!account_id) return NULL;
  pthread_mutex_lock(&g_reg_mutex);
  nh_auth_porthome_job *j = lookup_locked(account_id);
  pthread_mutex_unlock(&g_reg_mutex);
  return j;
}

/* ────────────────────────────────────────────────────────────────────
 * Label callback: writes into the staging descriptor.
 * ──────────────────────────────────────────────────────────────────── */

typedef struct label_ctx {
  nh_auth_porthome_job *job;
  const uint8_t *sealed_manifest;
  size_t         sealed_len;
  uint8_t        home_key[32];
  nh_porthome_prov_fetch_fn fetch;
  void *         fetch_ctx;
  /* Real-fetch path (Phase 2.5, bead nostrc-9k4g). When use_helper is
   * true, label_write_into spawns the nostr-home-fetch subprocess
   * against `home_fd` instead of running the in-process materializer.
   * `account_pubkey_hex` / `home_key_hex` / `home_root_id_hex` /
   * `d_tag` are pre-rendered; `relays`/`servers` are borrowed pointers
   * whose lifetime is the enclosing job_run() frame. */
  int          use_helper;
  char         account_pubkey_hex[65];
  char         home_key_hex[65];
  char         home_root_id_hex[65];
  char         d_tag[97];
  const char **relays;
  size_t       relays_count;
  const char **blossom_servers;
  size_t       blossom_servers_count;
  int          allow_insecure;
} label_ctx;

static int helper_progress_cb(void *ctx, uint64_t bytes, uint64_t files,
                              int phase) {
  nh_auth_porthome_job *j = (nh_auth_porthome_job *)ctx;
  if (!j) return 0;
  atomic_store(&j->bytes, bytes);
  atomic_store(&j->files, (uint32_t)(files > 0xffffffffu ? 0xffffffffu : files));
  if (atomic_load(&j->cancelled)) return 1;
  (void)phase;
  return 0;
}

/* Provisioner progress hooks are limited to bytes + files; feed those
 * into the job's atomic counters so WAIT_HOME can report them. Called
 * by nh_porthome_materialize_into_fd via the progress-artifact side
 * channel — but we don't own that channel here (the provisioner writes
 * directly to progress_path). We update our atomics inline from the
 * artifact instead: on completion we set the terminal state; while the
 * job runs the atomics reflect the last observed per-file update.
 *
 * The provisioner writes the artifact synchronously per file, so a
 * poller can re-read it — we just record the last-known state at end
 * of run for the WAIT_HOME response. */

static nh_identity_rc label_write_into(void *ctx, int home_fd) {
  label_ctx *lc = (label_ctx *)ctx;
  if (!lc || home_fd < 0) return NH_IDENTITY_INVALID;
  if (atomic_load(&lc->job->cancelled))
    return NH_IDENTITY_UNSUPPORTED;

  nh_porthome_prov_opts opts = {0};
  /* Materialise as root:root inside the staging descriptor. The
   * identity layer chown()s the whole tree to the account uid/gid
   * when it installs the home (identity_home.c). We do NOT try to
   * uid-map per file — design §D17: uid/gid are machine-local. */
  opts.local_uid = 0;
  opts.local_gid = 0;
  opts.max_bytes_per_load = lc->job->config.bandwidth_bytes_per_load;
  opts.load_timeout_sec   = lc->job->config.load_timeout_sec;
  opts.max_total_bytes    = lc->job->config.max_home_bytes;
  opts.progress_path      = lc->job->progress_path;

  nh_porthome_prov_status pr;
  if (lc->use_helper) {
    /* Real-fetch path: spawn nostr-home-fetch, which fetches the
     * kind-30078 pointer over WSS, verifies signature+pubkey, decodes
     * the sealed CBOR manifest, and materialises chunks into home_fd
     * via the same nh_porthome_materialize_sealed_into_fd path used
     * by the in-process branch. On any recoverable failure the helper
     * exits 71/72/73/74/65 — mapped to LIMITED by the spawner. */
    nh_auth_porthome_fetch_args fa = {0};
    fa.account_pubkey_hex     = lc->account_pubkey_hex;
    fa.home_root_id_hex       = lc->home_root_id_hex;
    fa.home_key_hex           = lc->home_key_hex;
    fa.d_tag                  = lc->d_tag;
    fa.relays                 = lc->relays;
    fa.relays_count           = lc->relays_count;
    fa.blossom_servers        = lc->blossom_servers;
    fa.blossom_servers_count  = lc->blossom_servers_count;
    fa.bandwidth_cap_bytes    = lc->job->config.bandwidth_bytes_per_load;
    fa.per_file_timeout_sec   = lc->job->config.load_timeout_sec;
    fa.max_total_bytes        = lc->job->config.max_home_bytes;
    fa.relay_timeout_ms       = 10000u;
    fa.allow_insecure         = lc->allow_insecure;
    fa.staging_fd             = home_fd;
    fa.staging_dir            = NULL;
    fa.total_timeout_ms       = 300000u;

    nh_auth_porthome_fetch_progress_cb cb = { .fn = helper_progress_cb,
                                              .ctx = lc->job };
    int helper_exit = -1;
    nh_auth_porthome_fetch_result fr =
        nh_auth_porthome_fetch_spawn(&fa, &cb, &helper_exit);
    switch (fr) {
    case NH_PORTHOME_FETCH_RES_OK:
      pr = NH_PORTHOME_PROV_OK;
      break;
    case NH_PORTHOME_FETCH_RES_LIMITED:
    case NH_PORTHOME_FETCH_RES_UNAVAILABLE:
      syslog(LOG_INFO, "porthome fetch helper LIMITED (exit=%d)",
             helper_exit);
      pr = NH_PORTHOME_PROV_LIMITED;
      break;
    case NH_PORTHOME_FETCH_RES_FAILED:
    default:
      syslog(LOG_WARNING, "porthome fetch helper FAILED (exit=%d)",
             helper_exit);
      pr = NH_PORTHOME_PROV_INVARIANT;
      break;
    }
  } else if (lc->sealed_manifest && lc->sealed_len) {
    pr = nh_porthome_materialize_sealed_into_fd(
        home_fd, lc->sealed_manifest, lc->sealed_len,
        lc->home_key, lc->fetch, lc->fetch_ctx, &opts, lc->job->tx_id);
  } else {
    pr = NH_PORTHOME_PROV_LIMITED;
  }

  /* Map provisioner status onto identity_home rc. Anything non-OK is
   * routed through the "labeler failed → ambiguous → REPAIR_REQUIRED"
   * path in identity_home.c so the staging directory is preserved for
   * admin review and the existing home (if any) is NEVER touched.
   *
   * The provisioner's LIMITED and BUDGET are recoverable — we still
   * return non-OK to the identity layer so it aborts the install (a
   * half-materialised home would be worse than none), but we tag the
   * job's terminal state so PAM sees LIMITED_MODE instead of FAILED. */
  if (pr == NH_PORTHOME_PROV_OK) return NH_IDENTITY_OK;
  atomic_store(&lc->job->state,
               (pr == NH_PORTHOME_PROV_LIMITED || pr == NH_PORTHOME_PROV_BUDGET)
                   ? NH_AUTH_PORTHOME_JOB_LIMITED
                   : NH_AUTH_PORTHOME_JOB_FAILED);
  return NH_IDENTITY_UNSUPPORTED;
}

/* Job worker thread: derive home_key, call nh_identity_home_prepare
 * with the label callback (which runs the provisioner). */
static void *job_run(void *arg) {
  nh_auth_porthome_job *j = (nh_auth_porthome_job *)arg;
  atomic_store(&j->state, NH_AUTH_PORTHOME_JOB_RUNNING);

  label_ctx lc = {0};
  lc.job = j;
  if (nh_porthome_key_derive(j->wrap_seed, lc.home_key) != 0) {
    atomic_store(&j->state, NH_AUTH_PORTHOME_JOB_FAILED);
    goto done;
  }

  /* Phase 2.5 fetch source ordering (bead nostrc-9k4g):
   *   1. explicit test hook   -> synthetic in-memory manifest + fetcher
   *   2. real fetch helper    -> spawn nostr-home-fetch subprocess if
   *                              enabled (NH_PORTHOME_FETCH_HELPER not
   *                              "off", helper binary present, config
   *                              or environment supplies relays+servers)
   *   3. otherwise            -> LIMITED (design §5.3 interlock).
   *
   * The env-var relay/server fallback lets the operator flip on the
   * real fetcher without threading auth.conf through auth_broker.c
   * (bead nostrc-ww50 is coordinating the broker-side wiring). */
  const char **relay_ptrs = NULL;
  const char **server_ptrs = NULL;
  size_t relay_n = 0, server_n = 0;
  /* Materialise config arrays into pointer arrays (label_ctx borrows
   * the pointers, so their storage must outlive the call to
   * nh_identity_home_prepare below — we keep them on the stack). */
  const char *relay_stack[16];
  const char *server_stack[16];
  char env_relays[1024];
  char env_servers[1024];
  int allow_insecure_env = 0;

  if (j->config.home_relays_count > 0 && j->config.home_relays) {
    for (size_t i = 0; i < j->config.home_relays_count && relay_n < 16; i++) {
      if (j->config.home_relays[i] && j->config.home_relays[i][0])
        relay_stack[relay_n++] = j->config.home_relays[i];
    }
    relay_ptrs = relay_stack;
  } else {
    const char *e = getenv("NH_PORTHOME_FETCH_RELAYS");
    if (e && e[0]) {
      size_t l = strlen(e);
      if (l < sizeof env_relays) {
        memcpy(env_relays, e, l + 1);
        char *p = env_relays;
        while (p && *p && relay_n < 16) {
          char *comma = strchr(p, ',');
          if (comma) *comma = '\0';
          if (*p) relay_stack[relay_n++] = p;
          p = comma ? comma + 1 : NULL;
        }
        relay_ptrs = relay_stack;
      }
    }
  }
  if (j->config.blossom_servers_count > 0 && j->config.blossom_servers) {
    for (size_t i = 0; i < j->config.blossom_servers_count && server_n < 16; i++) {
      if (j->config.blossom_servers[i] && j->config.blossom_servers[i][0])
        server_stack[server_n++] = j->config.blossom_servers[i];
    }
    server_ptrs = server_stack;
  } else {
    const char *e = getenv("NH_PORTHOME_FETCH_BLOSSOM_SERVERS");
    if (e && e[0]) {
      size_t l = strlen(e);
      if (l < sizeof env_servers) {
        memcpy(env_servers, e, l + 1);
        char *p = env_servers;
        while (p && *p && server_n < 16) {
          char *comma = strchr(p, ',');
          if (comma) *comma = '\0';
          if (*p) server_stack[server_n++] = p;
          p = comma ? comma + 1 : NULL;
        }
        server_ptrs = server_stack;
      }
    }
  }
  {
    const char *e = getenv("NH_PORTHOME_ALLOW_INSECURE");
    if (e && strcmp(e, "1") == 0) allow_insecure_env = 1;
  }

  if (j->test_sealed_manifest && j->test_sealed_len && j->test_fetch) {
    lc.sealed_manifest = j->test_sealed_manifest;
    lc.sealed_len      = j->test_sealed_len;
    lc.fetch           = (nh_porthome_prov_fetch_fn)j->test_fetch;
    lc.fetch_ctx       = j->test_fetch_ctx;
  } else if (relay_n > 0 && server_n > 0) {
    /* Try the real helper. Look it up first — if unavailable, fall
     * through to LIMITED without spawning. */
    const char *hp = nh_auth_porthome_fetch_helper_path();
    const char *off = getenv("NH_PORTHOME_FETCH_HELPER");
    if (!hp || access(hp, X_OK) != 0 || (off && strcmp(off, "off") == 0)) {
      atomic_store(&j->state, NH_AUTH_PORTHOME_JOB_LIMITED);
      goto done;
    }
    /* Derive an account-pubkey and derived home_root_id for the
     * helper's control payload. account_pubkey_hex is stored on the
     * job (populated by nh_auth_porthome_start via the account arg). */
    /* Render home_key hex. */
    nh_porthome_hex64(lc.home_key, lc.home_key_hex);
    /* Derive a stable home_root_id from home_key + tag. This is opaque
     * per-home; the helper does not currently verify it against the
     * decoded manifest but plumbing it now keeps the wire schema
     * ready for Phase 3. */
    {
      uint8_t tmp[32];
      uint8_t buf[32 + 32];
      memcpy(buf, lc.home_key, 32);
      memcpy(buf + 32, "porthome/v1/home-root-id-derive", 32);
      if (nh_porthome_sha256(buf, sizeof buf, tmp) != 0) {
        atomic_store(&j->state, NH_AUTH_PORTHOME_JOB_LIMITED);
        goto done;
      }
      nh_porthome_hex64(tmp, lc.home_root_id_hex);
    }
    /* Account pubkey and d-tag. The account struct is not directly
     * addressable from the job (it was consumed by
     * nh_auth_porthome_start); we look it up now via the store. */
    nh_identity_account acct;
    if (nh_identity_store_lookup_by_id(j->store, j->account_id, &acct)
        == NH_IDENTITY_OK && acct.pubkey_hex[0]) {
      strncpy(lc.account_pubkey_hex, acct.pubkey_hex,
              sizeof lc.account_pubkey_hex - 1);
    } else {
      atomic_store(&j->state, NH_AUTH_PORTHOME_JOB_LIMITED);
      goto done;
    }
    snprintf(lc.d_tag, sizeof lc.d_tag, "nostr-homed.home.v1:personal");
    lc.relays = relay_ptrs;
    lc.relays_count = relay_n;
    lc.blossom_servers = server_ptrs;
    lc.blossom_servers_count = server_n;
    lc.allow_insecure = allow_insecure_env;
    lc.use_helper = 1;
  } else {
    atomic_store(&j->state, NH_AUTH_PORTHOME_JOB_LIMITED);
    goto done;
  }

  /* We need an ENROLL operation state to feed home_prepare. For a
   * fresh (first-login) portable-home the caller must have allocated
   * an operation via nh_identity_operation_begin_enroll ahead of
   * time; we assume `tx_id` is that operation id. If the operation
   * does not resolve or is not PENDING we treat this as LIMITED — the
   * existing home is never touched. */
  nh_identity_operation_state state;
  nh_identity_rc rc = nh_identity_operation_get(j->store, j->tx_id, &state);
  if (rc != NH_IDENTITY_OK ||
      state.type != NH_IDENTITY_OPERATION_ENROLL ||
      (state.outcome != NH_IDENTITY_OUTCOME_PENDING &&
       state.outcome != NH_IDENTITY_OUTCOME_DONE)) {
    atomic_store(&j->state, NH_AUTH_PORTHOME_JOB_LIMITED);
    goto done;
  }

  nh_identity_home_options opts = {0};
  opts.skel_path = NULL;
  opts.labeling_required = true;
  opts.label = label_write_into;
  opts.label_context = &lc;

  nh_identity_operation_state after;
  rc = nh_identity_home_prepare(j->store, j->tx_id, &opts, &after);
  int cur = atomic_load(&j->state);
  if (rc == NH_IDENTITY_OK) {
    atomic_store(&j->state, NH_AUTH_PORTHOME_JOB_OK);
    /* W(3): drop the wrap seed to the per-uid runtime path so the
     * user-session syncd can pick it up without env plumbing. Warn-only;
     * failure keeps the env fallback path viable. */
    nh_identity_account acct2;
    if (nh_identity_store_lookup_by_id(j->store, j->account_id, &acct2)
        == NH_IDENTITY_OK && acct2.uid != 0) {
      (void)nh_auth_broker_porthome_drop_seed_for_uid((uid_t)acct2.uid,
                                                       j->wrap_seed);
    }
  } else if (cur == NH_AUTH_PORTHOME_JOB_RUNNING) {
    /* Labeler didn't tag a terminal state; treat as LIMITED so PAM
     * lets login proceed with the existing/empty home. */
    atomic_store(&j->state, NH_AUTH_PORTHOME_JOB_LIMITED);
  }

done:
  OPENSSL_cleanse(lc.home_key, sizeof lc.home_key);
  return NULL;
}

int nh_auth_porthome_start(const nh_auth_porthome_config *config,
                           const nh_identity_account *account,
                           const uint8_t wrap_seed[32],
                           const char *tx_id,
                           nh_identity_store *store,
                           nh_auth_porthome_job **out_job) {
  if (!config || !account || !wrap_seed || !tx_id || !store || !out_job)
    return -1;
  *out_job = NULL;

  pthread_mutex_lock(&g_reg_mutex);
  if (!g_reg_ready) nh_auth_porthome_registry_init();

  if (lookup_locked(account->account_id)) {
    pthread_mutex_unlock(&g_reg_mutex);
    return -1;  /* IN_PROGRESS — caller returns without queueing */
  }

  size_t slot = REG_CAP;
  for (size_t i = 0; i < REG_CAP; i++) if (!g_reg[i]) { slot = i; break; }
  if (slot == REG_CAP) {
    pthread_mutex_unlock(&g_reg_mutex);
    return -1;
  }

  nh_auth_porthome_job *j = calloc(1, sizeof *j);
  if (!j) { pthread_mutex_unlock(&g_reg_mutex); return -1; }
  atomic_init(&j->refcount, 1);
  atomic_init(&j->state, NH_AUTH_PORTHOME_JOB_PENDING);
  atomic_init(&j->cancelled, 0);
  atomic_init(&j->bytes, 0);
  atomic_init(&j->total_bytes, 0);
  atomic_init(&j->files, 0);
  atomic_init(&j->total_files, 0);
  strncpy(j->account_id, account->account_id, sizeof j->account_id - 1);
  strncpy(j->tx_id, tx_id, sizeof j->tx_id - 1);
  memcpy(j->wrap_seed, wrap_seed, 32);
  (void)mlock(j->wrap_seed, sizeof j->wrap_seed);
  j->store = store;
  j->config = *config;

  /* Consume any pending test-hook injection (single-shot). */
  if (g_next_test_sealed && g_next_test_sealed_len && g_next_test_fetch) {
    uint8_t *copy = malloc(g_next_test_sealed_len);
    if (copy) {
      memcpy(copy, g_next_test_sealed, g_next_test_sealed_len);
      j->test_sealed_manifest = copy;
      j->test_sealed_len = g_next_test_sealed_len;
      j->test_fetch = g_next_test_fetch;
      j->test_fetch_ctx = g_next_test_fetch_ctx;
    }
    g_next_test_sealed = NULL;
    g_next_test_sealed_len = 0;
    g_next_test_fetch = NULL;
    g_next_test_fetch_ctx = NULL;
  }

  /* Progress artifact path: /run/nostr-auth/greeter/<tx>_porthome.json.
   * A follow-up bead will move this under a per-tx subdir. */
  char pp[1024];
  int nn = snprintf(pp, sizeof pp, "%s/%s_porthome.json",
                    progress_dir(), tx_id);
  if (nn > 0 && (size_t)nn < sizeof pp) j->progress_path = strdup(pp);

  g_reg[slot] = j;
  atomic_fetch_add(&j->refcount, 1); /* hold a ref on behalf of registry */
  pthread_mutex_unlock(&g_reg_mutex);

  if (pthread_create(&j->thread, NULL, job_run, j) != 0) {
    /* Roll back registration. */
    pthread_mutex_lock(&g_reg_mutex);
    for (size_t i = 0; i < REG_CAP; i++)
      if (g_reg[i] == j) { g_reg[i] = NULL; break; }
    pthread_mutex_unlock(&g_reg_mutex);
    job_release(j);  /* registry ref */
    job_release(j);  /* creator ref */
    return -1;
  }
  j->thread_started = 1;
  *out_job = j;
  return 0;
}

static void snapshot_progress(nh_auth_porthome_job *j,
                              nh_auth_porthome_progress_hint *out) {
  if (!out) return;
  out->bytes = atomic_load(&j->bytes);
  out->total_bytes = atomic_load(&j->total_bytes);
  out->files = atomic_load(&j->files);
  out->total_files = atomic_load(&j->total_files);
}

nh_auth_porthome_job_state
nh_auth_porthome_wait(nh_auth_porthome_job *j, uint32_t timeout_ms,
                      nh_auth_porthome_progress_hint *hint_out) {
  if (!j) return NH_AUTH_PORTHOME_JOB_FAILED;
  /* Bounded busy-wait with 10 ms tick — cheap for the current single-tx
   * broker; a follow-up can move to a condvar. */
  const uint32_t tick_ms = 10u;
  uint32_t waited = 0;
  for (;;) {
    int st = atomic_load(&j->state);
    if (st == NH_AUTH_PORTHOME_JOB_OK ||
        st == NH_AUTH_PORTHOME_JOB_LIMITED ||
        st == NH_AUTH_PORTHOME_JOB_FAILED) {
      /* Terminal: join the thread and retire the registry slot. */
      if (j->thread_started) {
        pthread_join(j->thread, NULL);
        j->thread_started = 0;
      }
      snapshot_progress(j, hint_out);

      pthread_mutex_lock(&g_reg_mutex);
      for (size_t i = 0; i < REG_CAP; i++)
        if (g_reg[i] == j) { g_reg[i] = NULL; break; }
      pthread_mutex_unlock(&g_reg_mutex);
      /* Release the registry's ref. Caller still holds its own. */
      job_release(j);
      return (nh_auth_porthome_job_state)st;
    }
    if (waited >= timeout_ms) {
      snapshot_progress(j, hint_out);
      return NH_AUTH_PORTHOME_JOB_RUNNING;
    }
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)tick_ms * 1000000L };
    nanosleep(&ts, NULL);
    waited += tick_ms;
  }
}

void nh_auth_porthome_cancel(nh_auth_porthome_job *j) {
  if (!j) return;
  atomic_store(&j->cancelled, 1);
}



/* ────────────────────────────────────────────────────────────────────
 * Broker-owned wrap-seed cache (bead nostrc-ck6i).
 *
 * The provider's post-verify callback deposits a 32-byte wrap_seed
 * keyed on the account_id. The broker's PROVISION_HOME handler takes
 * it back out (single-use) to start a provisioning job. Entries expire
 * after a short TTL (default 300 s) so a stalled login flow cannot
 * leave a wrap_seed sitting in memory forever.
 *
 * The cache pages are mlock'd. Entries are wiped on take/expire. */

#define NH_PORTHOME_WRAP_TTL_SEC 300u
#define NH_PORTHOME_WRAP_CAP     16u

typedef struct wrap_entry {
  char    account_id[NH_IDENTITY_UUID_CAP];
  uint8_t seed[32];
  time_t  deposited_at;
  int     in_use;
} wrap_entry;

static pthread_mutex_t g_wrap_mutex = PTHREAD_MUTEX_INITIALIZER;
static wrap_entry g_wrap[NH_PORTHOME_WRAP_CAP];
static int g_wrap_mlocked;

static void wrap_gc_locked(time_t now) {
  for (size_t i = 0; i < NH_PORTHOME_WRAP_CAP; i++) {
    if (!g_wrap[i].in_use) continue;
    if (now - g_wrap[i].deposited_at > (time_t)NH_PORTHOME_WRAP_TTL_SEC) {
      OPENSSL_cleanse(g_wrap[i].seed, sizeof g_wrap[i].seed);
      memset(g_wrap[i].account_id, 0, sizeof g_wrap[i].account_id);
      g_wrap[i].in_use = 0;
    }
  }
}

int nh_auth_broker_porthome_deposit_wrap_seed(const char *account_id,
                                              const uint8_t seed[32]) {
  if (!account_id || !seed) return -1;
  pthread_mutex_lock(&g_wrap_mutex);
  if (!g_wrap_mlocked) {
    (void)mlock(g_wrap, sizeof g_wrap);
    g_wrap_mlocked = 1;
  }
  time_t now = time(NULL);
  wrap_gc_locked(now);
  /* Refresh any existing entry rather than allocating a new slot. */
  for (size_t i = 0; i < NH_PORTHOME_WRAP_CAP; i++) {
    if (g_wrap[i].in_use && strcmp(g_wrap[i].account_id, account_id) == 0) {
      memcpy(g_wrap[i].seed, seed, 32);
      g_wrap[i].deposited_at = now;
      pthread_mutex_unlock(&g_wrap_mutex);
      return 0;
    }
  }
  for (size_t i = 0; i < NH_PORTHOME_WRAP_CAP; i++) {
    if (!g_wrap[i].in_use) {
      strncpy(g_wrap[i].account_id, account_id, sizeof g_wrap[i].account_id - 1);
      memcpy(g_wrap[i].seed, seed, 32);
      g_wrap[i].deposited_at = now;
      g_wrap[i].in_use = 1;
      pthread_mutex_unlock(&g_wrap_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_wrap_mutex);
  return -1;
}

int nh_auth_broker_porthome_take_wrap_seed(const char *account_id,
                                           uint8_t out_seed[32]) {
  if (!account_id || !out_seed) return -1;
  pthread_mutex_lock(&g_wrap_mutex);
  time_t now = time(NULL);
  wrap_gc_locked(now);
  for (size_t i = 0; i < NH_PORTHOME_WRAP_CAP; i++) {
    if (g_wrap[i].in_use && strcmp(g_wrap[i].account_id, account_id) == 0) {
      memcpy(out_seed, g_wrap[i].seed, 32);
      OPENSSL_cleanse(g_wrap[i].seed, sizeof g_wrap[i].seed);
      memset(g_wrap[i].account_id, 0, sizeof g_wrap[i].account_id);
      g_wrap[i].in_use = 0;
      pthread_mutex_unlock(&g_wrap_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_wrap_mutex);
  return -1;
}

/* ───────────────────────────────────────────────────────────────
 * Bead nostrc-pvha: NIP-46 wrap-key enrollment / unwrap on login.
 *
 * The provider post-verify hook (called from provider_nip46.c and
 * provider_nip46_qr.c) uses the live signer session to either fetch
 * (nip44_decrypt) or mint+persist (nip44_encrypt) the wrap seed, then
 * deposits the seed into the wrap cache above. Everything runs inside
 * the same PAM window that already blocks for sign_event, so the
 * broker's WAIT_HOME budget covers it.
 * ─────────────────────────────────────────────────────────────── */

static pthread_mutex_t g_pvha_mutex = PTHREAD_MUTEX_INITIALIZER;
static nh_identity_store *g_pvha_store;
static int g_pvha_enroll_wrap_key;

void nh_auth_broker_porthome_install(nh_identity_store *store,
                                     int enroll_wrap_key) {
  pthread_mutex_lock(&g_pvha_mutex);
  g_pvha_store = store;
  g_pvha_enroll_wrap_key = enroll_wrap_key ? 1 : 0;
  pthread_mutex_unlock(&g_pvha_mutex);
}

static int hex_lc(uint8_t nibble) {
  return (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
}

static void bytes_to_hex64(const uint8_t in[32], char out[65]) {
  for (int i = 0; i < 32; i++) {
    out[i * 2]     = (char)hex_lc((in[i] >> 4) & 0xf);
    out[i * 2 + 1] = (char)hex_lc(in[i] & 0xf);
  }
  out[64] = '\0';
}

static int try_load_provider_and_ct(const char *account_id,
                                    char **out_provider_id,
                                    uint8_t **out_ct_bytes,
                                    size_t *out_ct_len,
                                    int *out_have_ct) {
  *out_provider_id = NULL;
  *out_ct_bytes = NULL;
  *out_ct_len = 0;
  *out_have_ct = 0;

  nh_identity_provider_record rec;
  memset(&rec, 0, sizeof rec);
  /* Try QR first, then bunker — an account MAY have both types staged,
   * but only one is enabled per (account,type) by the providers index.
   * For the porthome flow either is fine. */
  nh_identity_rc rc = nh_identity_store_provider_get(
      g_pvha_store, account_id, NH_IDENTITY_PROVIDER_NIP46_QR, true, &rec);
  if (rc != NH_IDENTITY_OK) {
    memset(&rec, 0, sizeof rec);
    rc = nh_identity_store_provider_get(
        g_pvha_store, account_id, NH_IDENTITY_PROVIDER_NIP46_BUNKER, true, &rec);
  }
  if (rc != NH_IDENTITY_OK) {
    /* Wipe potential secret_blob bytes even on failure. */
    OPENSSL_cleanse(&rec, sizeof rec);
    return -1;
  }

  *out_provider_id = strdup(rec.provider_id);
  if (!*out_provider_id) {
    OPENSSL_cleanse(&rec, sizeof rec);
    return -1;
  }
  if (rec.wrapped_home_key_len > 0) {
    uint8_t *b = malloc(rec.wrapped_home_key_len);
    if (!b) {
      free(*out_provider_id); *out_provider_id = NULL;
      OPENSSL_cleanse(&rec, sizeof rec);
      return -1;
    }
    memcpy(b, rec.wrapped_home_key, rec.wrapped_home_key_len);
    *out_ct_bytes = b;
    *out_ct_len = rec.wrapped_home_key_len;
    *out_have_ct = 1;
  }
  OPENSSL_cleanse(&rec, sizeof rec);
  return 0;
}

int nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
    void *nip46_session, const char *account_id,
    const char *account_pubkey_hex) {
  if (!nip46_session || !account_id || !account_pubkey_hex ||
      strlen(account_pubkey_hex) != 64)
    return 0; /* nothing to do */

  nh_identity_store *store;
  int enroll_on;
  pthread_mutex_lock(&g_pvha_mutex);
  store = g_pvha_store;
  enroll_on = g_pvha_enroll_wrap_key;
  pthread_mutex_unlock(&g_pvha_mutex);
  if (!store) return 0; /* broker not opted-in */

  char *provider_id = NULL;
  uint8_t *ct_bytes = NULL;
  size_t ct_len = 0;
  int have_ct = 0;
  if (try_load_provider_and_ct(account_id, &provider_id,
                               &ct_bytes, &ct_len, &have_ct) != 0) {
    /* No NIP-46 provider record — nothing to do. Silent no-op so a
     * bunker-only account that hasn't enrolled porthome still logs in
     * cleanly. */
    free(provider_id);
    free(ct_bytes);
    return 0;
  }

  NostrNip46Session *s = (NostrNip46Session *)nip46_session;
  uint8_t seed[32] = {0};
  int seed_valid = 0;

  if (have_ct) {
    /* Wire the ciphertext as a NUL-terminated ASCII string — NIP-44 v2
     * ciphertexts are base64 ASCII, so we stored the bytes verbatim.
     * A stray non-ASCII byte from a corrupted row would fail the
     * bunker's canonical base64 decode; we still guard against
     * embedded NULs by refusing early. */
    for (size_t i = 0; i < ct_len; i++) {
      if (ct_bytes[i] == 0) { free(provider_id); free(ct_bytes); return -1; }
    }
    char *ct_z = malloc(ct_len + 1);
    if (!ct_z) { free(provider_id); free(ct_bytes); return -1; }
    memcpy(ct_z, ct_bytes, ct_len);
    ct_z[ct_len] = '\0';
    char *pt = NULL;
    int rc = nostr_nip46_client_nip44_decrypt_rpc(
        s, account_pubkey_hex, ct_z, &pt);
    OPENSSL_cleanse(ct_z, ct_len);
    free(ct_z);
    if (rc != 0 || !pt) {
      free(pt);
      free(provider_id); free(ct_bytes);
      /* Signer refused / offline / mismatched key. Login already
       * succeeded on the auth axis — return -1 so the caller can
       * decide whether to surface anything. Provision path falls back
       * to NOT_SUPPORTED because no seed lands in the cache. */
      return -1;
    }
    /* Accept 32 raw bytes or 64 lowercase hex. */
    if (nh_porthome_wrap_seed_from_nip44_plaintext(
            (const uint8_t *)pt, strlen(pt), seed) == NH_PORTHOME_OK) {
      seed_valid = 1;
    }
    OPENSSL_cleanse(pt, strlen(pt));
    free(pt);
  } else if (enroll_on) {
    /* Mint a fresh 32-byte seed and ask the signer to nip44_encrypt
     * it (self-encrypt: peer == account_pubkey). The RPC accepts a
     * UTF-8 plaintext; encode the seed as 64 lowercase hex so the JSON
     * transport is safe. The signer's nip44 v2 output is base64 ASCII
     * which we persist verbatim as the wrapped_home_key BLOB. */
    if (nh_porthome_wrap_seed_random(seed) != NH_PORTHOME_OK) {
      free(provider_id); free(ct_bytes);
      return -1;
    }
    char seed_hex[65];
    bytes_to_hex64(seed, seed_hex);
    char *ct_out = NULL;
    int rc = nostr_nip46_client_nip44_encrypt_rpc(
        s, account_pubkey_hex, seed_hex, &ct_out);
    /* Wipe the seed_hex copy immediately — the bytes-of-hex are
     * as sensitive as the seed itself. */
    OPENSSL_cleanse(seed_hex, sizeof seed_hex);
    if (rc != 0 || !ct_out) {
      free(ct_out);
      OPENSSL_cleanse(seed, sizeof seed);
      free(provider_id); free(ct_bytes);
      return -1;
    }
    size_t ct_out_len = strlen(ct_out);
    if (ct_out_len == 0 || ct_out_len > NH_IDENTITY_WRAPPED_HOME_KEY_MAX) {
      OPENSSL_cleanse(ct_out, ct_out_len);
      free(ct_out);
      OPENSSL_cleanse(seed, sizeof seed);
      free(provider_id); free(ct_bytes);
      return -1;
    }
    nh_identity_rc srx = nh_identity_provider_set_wrapped_home_key(
        store, provider_id, (const uint8_t *)ct_out, ct_out_len);
    OPENSSL_cleanse(ct_out, ct_out_len);
    free(ct_out);
    if (srx != NH_IDENTITY_OK) {
      OPENSSL_cleanse(seed, sizeof seed);
      free(provider_id); free(ct_bytes);
      return -1;
    }
    seed_valid = 1;
  }

  int deposit_rc = 0;
  if (seed_valid) {
    deposit_rc = nh_auth_broker_porthome_deposit_wrap_seed(account_id, seed);
  }
  OPENSSL_cleanse(seed, sizeof seed);
  free(provider_id);
  free(ct_bytes);
  return deposit_rc;
}


/* ────────────────────────────────────────────────────────────────────
 * Phase 2.5B fetch-helper sandbox seam (bead nostrc-ww50).
 *
 * The 9k4g agent's helper does the real network work; this glue just
 * spawns the helper under nh_porthome_spawn_sandboxed, waits with a
 * wall-clock deadline, and returns the exit class so the WAIT_HOME
 * handler can still distinguish LIMITED_MODE (network / decode /
 * SSRF-refused) from FAILED (internal / killed).
 *
 * Materialisation stays in the broker as root — the labeler in
 * label_write_into() writes through nh_identity_home_prepare's
 * staging descriptor, which cannot be handed across the exec()
 * boundary without giving the helper write access to /var/lib and
 * defeating the whole point of dropping privileges. That split
 * matches design §7.2 exactly.
 * ──────────────────────────────────────────────────────────────────── */

int nh_auth_broker_porthome_run_fetch(const char *helper_path,
                                      const char *drop_user,
                                      char *const argv_tail[],
                                      int stdin_fd, int stdout_fd,
                                      int stderr_fd,
                                      uint32_t deadline_ms,
                                      int *out_exit,
                                      int *out_signal,
                                      int *out_timed_out) {
  if (out_exit)     *out_exit = -1;
  if (out_signal)   *out_signal = 0;
  if (out_timed_out) *out_timed_out = 0;

  const char *hp = (helper_path && helper_path[0])
                   ? helper_path
                   : NH_AUTH_BROKER_PORTHOME_DEFAULT_HELPER;
  if (hp[0] != '/') return -1;

  /* Build a fresh argv[] on the stack — argv_tail may be NULL. Cap at
   * a small number of tail args; the helper's CLI (see 9k4g) is short. */
  enum { MAX_TAIL = 16 };
  char *argv[MAX_TAIL + 2];
  size_t n = 0;
  argv[n++] = (char *)hp;
  if (argv_tail) {
    for (size_t i = 0; i < MAX_TAIL && argv_tail[i]; i++)
      argv[n++] = argv_tail[i];
  }
  argv[n] = NULL;

  /* Route the drop-user hint through the sandbox test seam so a
   * per-broker config value overrides the sandbox's compile-time
   * default. Cleared after the spawn returns so a subsequent caller
   * that didn't set a drop_user gets defaults back. */
  if (drop_user && drop_user[0])
    nh_porthome_sandbox_set_drop_user(drop_user);

  pid_t pid = 0;
  nh_porthome_sandbox_rc srx = nh_porthome_spawn_sandboxed(
      argv, NULL, stdin_fd, stdout_fd, stderr_fd, deadline_ms, &pid);
  if (drop_user && drop_user[0])
    nh_porthome_sandbox_set_drop_user(NULL);
  if (srx != NH_PORTHOME_SANDBOX_OK) return -1;

  int rc = nh_porthome_sandbox_wait(pid, deadline_ms,
                                    out_exit, out_signal, out_timed_out);
  return rc;
}

int nh_auth_broker_porthome_classify_fetch_exit(int exit_code, int signal) {
  if (signal != 0) return NH_AUTH_PORTHOME_JOB_FAILED;
  if (exit_code == 0) return NH_AUTH_PORTHOME_JOB_OK;
  if (exit_code >= 64 && exit_code <= 69)
    return NH_AUTH_PORTHOME_JOB_LIMITED;
  return NH_AUTH_PORTHOME_JOB_FAILED;
}


/* ────────────────────────────────────────────────────────────────────
 * W(3) — per-user runtime seed drop (bead nostrc-p6qp).
 *
 * The user-session syncd (nostr-home-syncd, runs unprivileged under
 * `systemd --user`) needs the 32-byte wrap seed to derive its home_key.
 * Historically it read NOSTR_HOMED_SYNCD_{NSEC,SEED}_HEX from the env.
 *
 * We add a minimal, disk-only handoff: after a successful home
 * materialisation the broker atomically drops the seed into
 *   /run/nostr-auth/session/<uid>/home_seed
 * mode 0600, owned by root:uid (chown so the target uid can read+unlink).
 * /run is tmpfs on modern systemd; the file never survives a reboot.
 *
 * The user-session syncd reads the file ONCE on start, then unlink(2)s
 * and mlock(2)s the value in memory. Nothing else on disk beyond that
 * ephemeral runtime file.
 *
 * We chose the file drop over the alternative (per-user AF_UNIX socket
 * gated by SO_PEERCRED) because:
 *   - No broker-side protocol change (no wire, no version bump).
 *   - No long-lived accept()ing listener in nostr-authd.
 *   - The tmpfs guarantee already gives us the "no persistence" property.
 *   - Ownership + 0600 gives us the "only that uid can read it" property.
 *
 * On any error the drop is warn-only: the daemon still falls back to the
 * env-var seam so headless / test paths keep working. Never logs the
 * seed value. */

#ifndef NH_AUTH_BROKER_PORTHOME_SESSION_DIR
#define NH_AUTH_BROKER_PORTHOME_SESSION_DIR "/run/nostr-auth/session"
#endif

/* Overrideable by tests (e.g. tmpdir under /tmp). NULL / "" resets to
 * the compile-time default. */
static char g_session_dir[512];
static const char *session_dir(void) {
  return g_session_dir[0] ? g_session_dir : NH_AUTH_BROKER_PORTHOME_SESSION_DIR;
}
void nh_auth_broker_porthome_set_session_dir(const char *dir) {
  if (!dir || !*dir) { g_session_dir[0] = '\0'; return; }
  size_t n = strlen(dir);
  if (n >= sizeof g_session_dir) { g_session_dir[0] = '\0'; return; }
  memcpy(g_session_dir, dir, n + 1);
}

static int hex_lc_char(uint8_t nibble) {
  return (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
}
static void seed_to_hex64(const uint8_t in[32], char out[65]) {
  for (int i = 0; i < 32; i++) {
    out[i*2]     = (char)hex_lc_char((in[i] >> 4) & 0xf);
    out[i*2 + 1] = (char)hex_lc_char(in[i] & 0xf);
  }
  out[64] = '\0';
}

/* Write `hex64` (64 lowercase hex chars, no NUL required in the file)
 * to <udir>/<name> using tmp + fsync + rename + fchown+fchmod.
 * Returns 0 on success, -1 on any failure. Wipes local buffers on exit. */
static int drop_hex_file(const char *udir, const char *name, uid_t uid,
                         const char hex64[65]) {
  char path[800], tmp[840];
  if (snprintf(path, sizeof path, "%s/%s", udir, name) >= (int)sizeof path) return -1;
  if (snprintf(tmp,  sizeof tmp,  "%s.tmp.%d", path, (int)getpid()) >= (int)sizeof tmp) return -1;
  int rc = -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    syslog(LOG_INFO, "porthome/seed-drop: open tmp %s failed errno=%d", tmp, errno);
    return -1;
  }
  size_t off = 0, want = 64;
  while (off < want) {
    ssize_t w = write(fd, hex64 + off, want - off);
    if (w < 0) { if (errno == EINTR) continue; goto out; }
    off += (size_t)w;
  }
  if (fchmod(fd, 0600) != 0) goto out;
  if (fchown(fd, uid, uid) != 0) {
    syslog(LOG_INFO, "porthome/seed-drop: fchown(%s,%u,%u) failed errno=%d",
           name, (unsigned)uid, (unsigned)uid, errno);
    /* keep going — see note above */
  }
  if (fsync(fd) != 0) goto out;
  close(fd); fd = -1;
  if (rename(tmp, path) != 0) {
    syslog(LOG_INFO, "porthome/seed-drop: rename %s failed errno=%d", path, errno);
    goto out;
  }
  syslog(LOG_INFO, "porthome/seed-drop: dropped %s for uid=%u", name, (unsigned)uid);
  rc = 0;
out:
  if (fd >= 0) close(fd);
  if (rc != 0) (void)unlink(tmp);
  return rc;
}

int nh_auth_broker_porthome_drop_seed_for_uid(uid_t uid,
                                              const uint8_t seed[32]) {
  if (!seed) return -1;
  const char *base = session_dir();
  /* mkdir -p the base + per-uid subdir. Ignore EEXIST. */
  if (mkdir(base, 0755) != 0 && errno != EEXIST) {
    syslog(LOG_INFO, "porthome/seed-drop: mkdir(%s) failed errno=%d", base, errno);
    return -1;
  }
  char udir[600];
  if (snprintf(udir, sizeof udir, "%s/%u", base, (unsigned)uid) >= (int)sizeof udir)
    return -1;
  if (mkdir(udir, 0700) != 0 && errno != EEXIST) {
    syslog(LOG_INFO, "porthome/seed-drop: mkdir(%s) failed errno=%d", udir, errno);
    return -1;
  }
  /* Chown the per-uid dir to <uid>:<uid> so only the target user can
   * traverse it. Best effort — if we're not root the chown fails and
   * we still write with root:uid on the file itself. */
  (void)chown(udir, uid, uid);

  char hex[65]; seed_to_hex64(seed, hex);
  /* Two drop files, both read-once-and-unlink by their respective
   * consumers so a session-scope daemon restart cannot recover the
   * seed (broker GET_HOME_SEED is the long-term fix — HFR §10.1,
   * design §D6c, filed as follow-up). Symmetric: same mode / owner /
   * atomic rename. Best-effort — a failure on the second drop must
   * not roll back the first, because the syncd already relies on
   * the primary drop and blocking it would silently break the sync
   * path. Both failures are warn-only; the FUSE mount then falls
   * back to NH_FUSE_SEED_HEX / NH_FUSE_SEED_FILE. */
  int rc_primary = drop_hex_file(udir, "home_seed",      uid, hex);
  int rc_fuse    = drop_hex_file(udir, "home_seed.fuse", uid, hex);
  OPENSSL_cleanse(hex, sizeof hex);
  /* Report success iff the primary syncd drop succeeded. The FUSE
   * drop is warn-only. */
  if (rc_fuse != 0)
    syslog(LOG_INFO, "porthome/seed-drop: home_seed.fuse drop failed (warn-only); FUSE mount will fall back to env");
  return rc_primary;
}

#else  /* !NH_AUTH_BROKER_ENABLE_PORTHOME */

int  nh_auth_porthome_registry_init(void)     { return 0; }
void nh_auth_porthome_registry_shutdown(void) {}
void nh_auth_porthome_set_progress_dir(const char *d) { (void)d; }
void nh_auth_porthome_set_test_hook(const uint8_t *m, size_t l,
                                    nh_auth_porthome_fetch_hook_fn f, void *c) {
  (void)m; (void)l; (void)f; (void)c;
}
nh_auth_porthome_job *nh_auth_porthome_lookup(const char *account_id) {
  (void)account_id; return NULL;
}
int nh_auth_porthome_start(const nh_auth_porthome_config *config,
                           const nh_identity_account *account,
                           const uint8_t wrap_seed[32],
                           const char *tx_id, nh_identity_store *store,
                           nh_auth_porthome_job **out_job) {
  (void)config; (void)account; (void)wrap_seed; (void)tx_id; (void)store;
  if (out_job) *out_job = NULL;
  return -1;
}
nh_auth_porthome_job_state
nh_auth_porthome_wait(nh_auth_porthome_job *job, uint32_t timeout_ms,
                      nh_auth_porthome_progress_hint *hint_out) {
  (void)job; (void)timeout_ms; (void)hint_out;
  return NH_AUTH_PORTHOME_JOB_FAILED;
}
void nh_auth_porthome_cancel(nh_auth_porthome_job *job) { (void)job; }

int nh_auth_broker_porthome_deposit_wrap_seed(const char *a,
                                              const uint8_t s[32]) {
  (void)a; (void)s; return -1;
}
int nh_auth_broker_porthome_take_wrap_seed(const char *a,
                                           uint8_t out[32]) {
  (void)a; (void)out; return -1;
}
void nh_auth_broker_porthome_install(nh_identity_store *store,
                                     int enroll_wrap_key) {
  (void)store; (void)enroll_wrap_key;
}
int nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
    void *nip46_session, const char *account_id,
    const char *account_pubkey_hex) {
  (void)nip46_session; (void)account_id; (void)account_pubkey_hex;
  return 0;
}

int nh_auth_broker_porthome_run_fetch(const char *helper_path,
                                      const char *drop_user,
                                      char *const argv_tail[],
                                      int stdin_fd, int stdout_fd,
                                      int stderr_fd,
                                      uint32_t deadline_ms,
                                      int *out_exit,
                                      int *out_signal,
                                      int *out_timed_out) {
  (void)helper_path; (void)drop_user; (void)argv_tail;
  (void)stdin_fd; (void)stdout_fd; (void)stderr_fd;
  (void)deadline_ms;
  if (out_exit) *out_exit = -1;
  if (out_signal) *out_signal = 0;
  if (out_timed_out) *out_timed_out = 0;
  return -1;
}
int nh_auth_broker_porthome_classify_fetch_exit(int exit_code, int signal) {
  (void)exit_code; (void)signal;
  return (int)NH_AUTH_PORTHOME_JOB_FAILED;
}

int nh_auth_broker_porthome_drop_seed_for_uid(uid_t uid,
                                              const uint8_t seed[32]) {
  (void)uid; (void)seed; return -1;
}
void nh_auth_broker_porthome_set_session_dir(const char *dir) { (void)dir; }

#endif /* NH_AUTH_BROKER_ENABLE_PORTHOME */
