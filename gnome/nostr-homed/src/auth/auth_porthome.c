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
} label_ctx;

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
  if (lc->sealed_manifest && lc->sealed_len) {
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

  /* Phase 2 fetch source:
   *   - test hook (present) -> synthetic in-memory manifest + fetcher
   *   - otherwise -> LIMITED (real relay fetch is Phase 2.5+ scope,
   *     tracked in the follow-up; this ensures the LIMITED_MODE
   *     interlock is the default when no manifest is available). */
  if (j->test_sealed_manifest && j->test_sealed_len && j->test_fetch) {
    lc.sealed_manifest = j->test_sealed_manifest;
    lc.sealed_len      = j->test_sealed_len;
    lc.fetch           = (nh_porthome_prov_fetch_fn)j->test_fetch;
    lc.fetch_ctx       = j->test_fetch_ctx;
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

#endif /* NH_AUTH_BROKER_ENABLE_PORTHOME */
