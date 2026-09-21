/* test_homectl_open_session_failures.c -- focused roaming regression for
 * beads nostrc-nxpb.8 (OpenSession false success after systemctl / mount /
 * mkdir failures).
 *
 * Injects nonzero results at each distinct step in nh_open_session and
 * asserts:
 *   (a) nh_open_session returns non-zero (failure surfaced), and
 *   (b) "mounted" is never recorded in the cache when the mount is not
 *       actually live.
 *
 * The three distinct failure sites are:
 *   1. mkdir_p_impl returns -1        (couldn't create /home/<user>)
 *   2. systemctl_start_impl returns -1 (spawn failure OR non-zero exit)
 *   3. is_mountpoint_impl returns 0    (unit "started" but mount not live)
 *
 * Each is exercised independently and cross-checked against the cache-write
 * record via a lightweight in-test cache stub. The happy path is also
 * exercised as a sanity check that the corrected code path still records
 * "mounted".
 */
#include "nostr_homectl.h"
#include "nostr_cache.h"
#include "nostr_manifest.h"
#include "nostr_secrets.h"
#include "relay_fetch.h"
#include "nostr_homectl_test_seam.h"
#include "nostr/nip19/nip19.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- cache stub: records status.<user> writes so tests can assert ---- */
static char g_last_status[64]     = "";
static char g_last_status_user[64] = "";
static int  g_saw_mount_write     = 0;
static int  g_saw_pid_write       = 0;

static void reset_witnesses(void){
  g_last_status[0]     = '\0';
  g_last_status_user[0] = '\0';
  g_saw_mount_write    = 0;
  g_saw_pid_write      = 0;
}

int nh_cache_open_configured(nh_cache *c, const char *p){ (void)c; (void)p; return 0; }
int nh_cache_open(nh_cache *c, const char *p){ (void)c; (void)p; return 0; }
void nh_cache_close(nh_cache *c){ (void)c; }
int nh_cache_set_setting(nh_cache *c, const char *k, const char *v){
  (void)c;
  if (!k || !v) return 0;
  if (strncmp(k, "status.", 7) == 0){
    strncpy(g_last_status_user, k + 7, sizeof g_last_status_user - 1);
    g_last_status_user[sizeof g_last_status_user - 1] = '\0';
    strncpy(g_last_status, v, sizeof g_last_status - 1);
    g_last_status[sizeof g_last_status - 1] = '\0';
  } else if (strncmp(k, "mount.", 6) == 0){
    g_saw_mount_write = 1;
  } else if (strncmp(k, "pid.", 4) == 0){
    g_saw_pid_write = 1;
  }
  return 0;
}
int nh_cache_get_setting(nh_cache *c, const char *k, char *o, size_t l){
  (void)c;
  if (o && l) o[0] = '\0';
  if (k && strcmp(k, "warmcache") == 0 && o && l >= 2){
    /* Pretend the cache is already warm so nh_open_session does not call
     * nh_warm_cache and drag in the whole roaming stack. */
    o[0] = '1'; o[1] = '\0';
    return 0;
  }
  return -1;
}
int nh_cache_set_uid_policy(nh_cache *c, uint32_t b, uint32_t r){ (void)c; (void)b; (void)r; return 0; }
uint32_t nh_cache_map_npub_to_uid(const nh_cache *c, const char *n){ (void)c; (void)n; return 100000; }
int nh_cache_lookup_name(nh_cache *c, const char *n, unsigned *u, unsigned *g, char *h, size_t hl){
  (void)c; (void)n; (void)u; (void)g; (void)h; (void)hl; return -1;
}
int nh_cache_lookup_uid(nh_cache *c, unsigned u, char *n, size_t nl, unsigned *g, char *h, size_t hl){
  (void)c; (void)u; (void)n; (void)nl; (void)g; (void)h; (void)hl; return -1;
}
int nh_cache_upsert_user(nh_cache *c, unsigned u, const char *n, const char *un, unsigned g, const char *h){
  (void)c; (void)u; (void)n; (void)un; (void)g; (void)h; return 0;
}
int nh_cache_group_lookup_name(nh_cache *c, const char *n, unsigned *g){ (void)c; (void)n; (void)g; return -1; }
int nh_cache_group_lookup_gid(nh_cache *c, unsigned g, char *n, size_t nl){ (void)c; (void)g; (void)n; (void)nl; return -1; }
int nh_cache_ensure_primary_group(nh_cache *c, const char *n, unsigned g){ (void)c; (void)n; (void)g; return 0; }

/* ---- Manifest / secrets / relay: never reached in these tests because we
 * short-circuit warmcache above. Provide empty stubs to satisfy the link. */
int nostr_nip19_decode_npub(const char *n, uint8_t o[32]){ (void)n; memset(o, 0, 32); return 0; }
int nh_manifest_parse_json(const char *j, nh_manifest *o){ (void)j; if (o) memset(o, 0, sizeof *o); return 0; }
void nh_manifest_free(nh_manifest *m){ (void)m; }
int nh_secrets_mount_tmpfs(const char *p){ (void)p; return 0; }
int nh_secrets_decrypt_via_signer(const char *ct, char **pt){ (void)ct; if (pt) *pt = NULL; return -1; }
int nh_fetch_profile_relays(const char **r, size_t n, const char *a, char ***o, size_t *c){
  (void)r; (void)n; (void)a; if (o) *o = NULL; if (c) *c = 0; return -1;
}
int nh_fetch_latest_manifest_json(const char **r, size_t n, const char *a, const char *ns, char **o){
  (void)r; (void)n; (void)a; (void)ns; if (o) *o = NULL; return -1;
}
int nh_fetch_latest_secrets_json(const char **r, size_t n, const char *a, const char *ns, char **o){
  (void)r; (void)n; (void)a; (void)ns; if (o) *o = NULL; return -1;
}

/* ---- Hook injection knobs ---- */
static int g_mkdir_rc      = 0;   /* what mkdir_p returns */
static int g_systemctl_rc  = 0;   /* what systemctl_start returns */
static int g_mount_ready   = 1;   /* what is_mountpoint returns */
static int g_mkdir_calls   = 0;
static int g_systemctl_calls = 0;
static int g_mountpoint_calls = 0;

static int hook_mkdir(const char *p, int m){ (void)p; (void)m; g_mkdir_calls++; return g_mkdir_rc; }
static int hook_systemctl_start(const char *u){ (void)u; g_systemctl_calls++; return g_systemctl_rc; }
static int hook_mountpoint(const char *p){ (void)p; g_mountpoint_calls++; return g_mount_ready; }

static void install_hooks(void){
  nh_hook_mkdir_p         = hook_mkdir;
  nh_hook_systemctl_start = hook_systemctl_start;
  nh_hook_is_mountpoint   = hook_mountpoint;
}

/* ---- Scenarios ---- */
static void scenario_happy_path(void){
  reset_witnesses();
  g_mkdir_rc = 0; g_systemctl_rc = 0; g_mount_ready = 1;
  g_mkdir_calls = g_systemctl_calls = g_mountpoint_calls = 0;
  int rc = nh_open_session("alice");
  fprintf(stderr, "[happy] rc=%d status=%s user=%s mount_write=%d pid_write=%d\n",
          rc, g_last_status, g_last_status_user, g_saw_mount_write, g_saw_pid_write);
  assert(rc == 0);
  assert(strcmp(g_last_status_user, "alice") == 0);
  assert(strcmp(g_last_status, "mounted") == 0);
  assert(g_saw_mount_write == 1);
  assert(g_saw_pid_write == 1);
  assert(g_mkdir_calls == 1);
  assert(g_systemctl_calls == 1);
  assert(g_mountpoint_calls == 1);
}

static void scenario_mkdir_fails(void){
  reset_witnesses();
  g_mkdir_rc = -1; g_systemctl_rc = 0; g_mount_ready = 1;
  g_mkdir_calls = g_systemctl_calls = g_mountpoint_calls = 0;
  int rc = nh_open_session("bob");
  fprintf(stderr, "[mkdir-fail] rc=%d status=%s user=%s mount_write=%d pid_write=%d systemctl_calls=%d\n",
          rc, g_last_status, g_last_status_user, g_saw_mount_write, g_saw_pid_write,
          g_systemctl_calls);
  /* (a) failure surfaces */
  assert(rc != 0);
  /* (b) "mounted" never recorded, and mount/pid fields not written */
  assert(strcmp(g_last_status, "mounted") != 0);
  assert(g_saw_mount_write == 0);
  assert(g_saw_pid_write == 0);
  /* systemctl must be skipped when there's nowhere to mount */
  assert(g_systemctl_calls == 0);
  /* status should be an honest "failed" marker */
  assert(strcmp(g_last_status_user, "bob") == 0);
  assert(strcmp(g_last_status, "failed") == 0);
}

static void scenario_systemctl_fails(void){
  reset_witnesses();
  g_mkdir_rc = 0; g_systemctl_rc = -1; g_mount_ready = 1;
  g_mkdir_calls = g_systemctl_calls = g_mountpoint_calls = 0;
  int rc = nh_open_session("carol");
  fprintf(stderr, "[systemctl-fail] rc=%d status=%s mount_write=%d pid_write=%d mp_calls=%d\n",
          rc, g_last_status, g_saw_mount_write, g_saw_pid_write, g_mountpoint_calls);
  assert(rc != 0);
  assert(strcmp(g_last_status, "mounted") != 0);
  assert(g_saw_mount_write == 0);
  assert(g_saw_pid_write == 0);
  /* is_mountpoint must not have been consulted -- systemctl failure short-
   * circuits the "verify mount is live" step so we can't accidentally
   * grade a stale mountpoint as success. */
  assert(g_mountpoint_calls == 0);
  assert(strcmp(g_last_status_user, "carol") == 0);
  assert(strcmp(g_last_status, "failed") == 0);
}

static void scenario_mount_not_live(void){
  reset_witnesses();
  g_mkdir_rc = 0; g_systemctl_rc = 0; g_mount_ready = 0;
  g_mkdir_calls = g_systemctl_calls = g_mountpoint_calls = 0;
  int rc = nh_open_session("dave");
  fprintf(stderr, "[mount-not-live] rc=%d status=%s mount_write=%d pid_write=%d\n",
          rc, g_last_status, g_saw_mount_write, g_saw_pid_write);
  assert(rc != 0);
  assert(strcmp(g_last_status, "mounted") != 0);
  assert(g_saw_mount_write == 0);
  assert(g_saw_pid_write == 0);
  assert(strcmp(g_last_status_user, "dave") == 0);
  assert(strcmp(g_last_status, "failed") == 0);
}

int main(void){
  install_hooks();
  scenario_happy_path();
  scenario_mkdir_fails();
  scenario_systemctl_fails();
  scenario_mount_not_live();
  fprintf(stderr, "test_homectl_open_session_failures: OK\n");
  return 0;
}
