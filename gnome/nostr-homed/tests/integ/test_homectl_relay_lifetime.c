/* test_homectl_relay_lifetime.c -- focused roaming regression for
 * beads nostrc-nxpb.7 (WarmCache relay-list use-after-free).
 *
 * Compiles nostr-homectl.c into the test binary (with the daemon `main`
 * renamed via -Dmain=nh_homectl_main_unused), provides in-test stubs for
 * every external dependency of nh_warm_cache (cache/manifest/secrets/relay
 * fetches, nip19 decode), overrides the D-Bus signer hook so the code path
 * proceeds without a session bus, and instruments the fetch helpers so we
 * can prove the same relay-array pointer is passed to
 * nh_fetch_latest_secrets_json AFTER nh_fetch_latest_manifest_json returns.
 *
 * The critical scenario is `relays_owned == 1`: the pre-fix nh_warm_cache
 * freed the owned relay array immediately after the manifest fetch and then
 * passed the freed array to nh_fetch_latest_secrets_json. Under ASan this
 * shows up as a heap-use-after-free during the secrets-fetch pointer walk.
 * Post-fix the array stays live until after the secrets step, and this test
 * passes clean.
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

/* ---- Test-visible witnesses ---- */
static const char **g_manifest_relays_seen = NULL;
static size_t       g_manifest_count_seen  = 0;
static const char **g_secrets_relays_seen  = NULL;
static size_t       g_secrets_count_seen   = 0;
static char         g_first_relay_copy[128] = "";
static int          g_saw_secrets_touch    = 0;

/* ---- Stubs for external deps of nh_warm_cache ---- */

/* nip19 decode: fake a 32-byte pubkey from a fixed npub value. */
int nostr_nip19_decode_npub(const char *npub, uint8_t out[32]){
  (void)npub;
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)i;
  return 0;
}

/* Cache: pretend the DB always fails to open so nh_warm_cache stays in the
 * memory-only relay/manifest/secrets path. Keeps the test hermetic. */
int nh_cache_open_configured(nh_cache *c, const char *p){ (void)c; (void)p; return -1; }
int nh_cache_open(nh_cache *c, const char *p){ (void)c; (void)p; return -1; }
void nh_cache_close(nh_cache *c){ (void)c; }
int nh_cache_set_setting(nh_cache *c, const char *k, const char *v){ (void)c; (void)k; (void)v; return 0; }
int nh_cache_get_setting(nh_cache *c, const char *k, char *o, size_t l){ (void)c; (void)k; if (o && l) o[0]=0; return -1; }
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

/* Manifest: return a minimal valid parse. */
int nh_manifest_parse_json(const char *json, nh_manifest *out){
  (void)json;
  if (out) memset(out, 0, sizeof(*out));
  return 0;
}
void nh_manifest_free(nh_manifest *m){ (void)m; }

/* Secrets: no-ops so the tmpfs write path can't touch /run. */
int nh_secrets_mount_tmpfs(const char *p){ (void)p; return 0; }
int nh_secrets_decrypt_via_signer(const char *ct, char **pt){
  (void)ct; if (pt) *pt = NULL; return -1;
}

/* Relay fetches ------------------------------------------------------- */

/* Hand nh_warm_cache a HEAP-OWNED relay array so relays_owned becomes 1 --
 * this is the branch where the pre-fix bug freed then re-used the array. */
int nh_fetch_profile_relays(const char **relays, size_t n, const char *author,
                            char ***out_relays, size_t *out_count){
  (void)relays; (void)n; (void)author;
  /* nh_warm_cache treats out_relays as `char**` (mutable strings). Each
   * element must be individually free()-able. */
  const size_t k = 3;
  char **arr = (char **)calloc(k, sizeof(char *));
  arr[0] = strdup("wss://owned-a.example");
  arr[1] = strdup("wss://owned-b.example");
  arr[2] = strdup("wss://owned-c.example");
  if (out_relays) *out_relays = arr;
  if (out_count)  *out_count  = k;
  return 0;
}

int nh_fetch_latest_manifest_json(const char **relays, size_t n, const char *author,
                                  const char *ns, char **out){
  (void)author; (void)ns;
  g_manifest_relays_seen = relays;
  g_manifest_count_seen  = n;
  assert(relays != NULL);
  assert(n > 0);
  /* Snapshot the first URL so we can compare identity later. */
  strncpy(g_first_relay_copy, relays[0], sizeof g_first_relay_copy - 1);
  g_first_relay_copy[sizeof g_first_relay_copy - 1] = '\0';
  if (out){ *out = strdup("{\"kind\":\"manifest\"}"); }
  return 0;
}

int nh_fetch_latest_secrets_json(const char **relays, size_t n, const char *author,
                                 const char *ns, char **out){
  (void)author; (void)ns;
  g_secrets_relays_seen = relays;
  g_secrets_count_seen  = n;

  /* CRITICAL: touching relays[i] here is the UAF pre-fix. Post-fix these
   * pointers must still be valid. ASan traps read-after-free below if the
   * bug is present. */
  assert(relays != NULL);
  assert(n == g_manifest_count_seen);
  for (size_t i = 0; i < n; i++){
    /* Dereference the *string* -- ASan catches use-after-free here for both
     * the outer array (relays) and each inner string, since the pre-fix
     * ordering freed both. */
    volatile size_t len = strlen(relays[i]);
    (void)len;
  }
  /* Confirm the outer array identity survived the manifest step. */
  assert(relays == g_manifest_relays_seen);
  /* Confirm the first URL text is intact. */
  assert(strcmp(relays[0], g_first_relay_copy) == 0);
  g_saw_secrets_touch = 1;
  if (out) *out = NULL;
  return -1; /* nothing to decrypt; short-circuit the rest of the do{}while */
}

/* ---- Test hook overrides ---- */
static int fake_dbus_get_signer_npub(char **out){
  if (!out) return -1;
  /* nh_warm_cache calls g_free() on this string. On glib platforms g_free
   * is malloc-free-compatible for strings allocated with malloc/strdup. */
  *out = strdup("npub1testtesttesttesttesttesttesttesttesttesttesttesttestuv6qz");
  return *out ? 0 : -1;
}

int main(void){
  nh_hook_dbus_get_signer_npub = fake_dbus_get_signer_npub;

  int rc = nh_warm_cache("personal");
  fprintf(stderr, "nh_warm_cache rc = %d\n", rc);
  /* Manifest fetch succeeded so return code is 0. */
  assert(rc == 0);

  /* Both hooks were reached. */
  assert(g_manifest_relays_seen != NULL);
  assert(g_secrets_relays_seen  != NULL);
  assert(g_manifest_count_seen  == g_secrets_count_seen);
  assert(g_saw_secrets_touch    == 1);

  /* Same array identity in both calls -- this is what proves the bug's
   * absence: nh_warm_cache did not free-and-then-reuse between the calls. */
  assert(g_manifest_relays_seen == g_secrets_relays_seen);

  fprintf(stderr, "test_homectl_relay_lifetime: OK\n");
  return 0;
}
