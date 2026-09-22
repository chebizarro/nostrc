/* nostr-homed-profile — operator CLI for the profile refresh pipeline.
 *
 * Usage:
 *   nostr-homed-profile refresh <user> [--relay=<wss://...>]... \
 *       [--pubkey=<hex>] [--cache-dir=<path>] [--image-user=<uname>] \
 *       [--image-helper=<abs-path>] [--throttle-seconds=<int>] \
 *       [--skip-accounts-install] [--auth-conf=<path>]
 *
 * Resolves the account's pubkey from the identity store when --pubkey
 * is not supplied. When --relay is not supplied, reads profile_relays
 * from auth.conf (falls back to the compiled-in defaults matching the
 * greeter). Exit status:
 *   0   installed
 *   1   fetch/parse/download/AccountsService failure
 *   2   argument / usage error
 *   3   throttled (last refresh too recent) */
#define _GNU_SOURCE
#include "auth_broker.h"
#include "nostr_identity.h"
#include "nostr_profile.h"

#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_RELAYS 8

/* Default relays baked in for a fresh install (docs/reviews will point
 * at them). Overridden by auth.conf profile_relays or --relay. */
static const char *DEFAULT_PROFILE_RELAYS[] = {
    "wss://relay.damus.io",
    "wss://nos.lol",
};
#define DEFAULT_PROFILE_RELAYS_N (sizeof DEFAULT_PROFILE_RELAYS / sizeof DEFAULT_PROFILE_RELAYS[0])

static nh_identity_ownership_result probe(void *c, const char *n,
                                          uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

static void usage(FILE *o, const char *argv0) {
  fprintf(o,
      "usage: %s refresh <user> [options]\n"
      "\n"
      "  --pubkey=<64-hex>            override the store's account pubkey\n"
      "  --relay=<wss://...>          add a relay (repeatable, max %d)\n"
      "  --cache-dir=<path>           default /var/lib/nostr-auth/profile\n"
      "  --image-user=<uname>         default nobody\n"
      "  --image-helper=<abs-path>    default: search PATH\n"
      "  --throttle-seconds=<int>     default 0 (no throttle)\n"
      "  --skip-accounts-install      dry-run: cache only, no D-Bus write\n"
      "  --auth-conf=<path>           default /etc/nostr-auth/auth.conf\n"
      "  --authority-dir=<path>       default /var/lib/nostr-auth\n",
      argv0, MAX_RELAYS);
}

static int is_hex64(const char *s) {
  if (!s || strlen(s) != 64) return 0;
  for (int i = 0; i < 64; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return 1;
}

static int lookup_account_pubkey(const char *authority_dir, const char *user,
                                 char out_hex[65]) {
  nh_identity_config cfg;
  nh_identity_config_defaults(&cfg);
  snprintf(cfg.authority_path, sizeof cfg.authority_path, "%s/authority.db",
           authority_dir);
  snprintf(cfg.projection_path, sizeof cfg.projection_path, "%s/nss.db",
           authority_dir);
  nh_identity_store_options opts = {0};
  opts.config = &cfg;
  opts.ownership_probe = probe;
  opts.flags = 0;
  nh_identity_store *store = NULL;
  if (nh_identity_store_open(&opts, &store) != NH_IDENTITY_OK) return -1;
  nh_identity_account acct;
  int rc = nh_identity_store_lookup_by_name(store, user, &acct) == NH_IDENTITY_OK
           ? 0 : -1;
  nh_identity_store_close(store);
  if (rc != 0) return -1;
  memcpy(out_hex, acct.pubkey_hex, 64);
  out_hex[64] = '\0';
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3 || strcmp(argv[1], "refresh") != 0) {
    usage(stderr, argv[0]);
    return 2;
  }
  const char *user = argv[2];
  const char *pubkey_override = NULL;
  const char *cache_dir = NULL;
  const char *image_user = NULL;
  const char *image_helper = NULL;
  const char *auth_conf = getenv("NH_AUTH_CONF");
  if (!auth_conf || !*auth_conf) auth_conf = "/etc/nostr-auth/auth.conf";
  const char *authority_dir = "/var/lib/nostr-auth";
  int64_t throttle_seconds = 0;
  bool skip_install = false;

  const char *relays[MAX_RELAYS];
  size_t n_relays = 0;

  for (int i = 3; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] != '-' || a[1] != '-') { usage(stderr, argv[0]); return 2; }
    const char *eq = strchr(a, '=');
    size_t klen = eq ? (size_t)(eq - a) : strlen(a);
    const char *v = eq ? eq + 1 : "";
#define K(name) (klen == strlen(name) && strncmp(a, name, klen) == 0)
    if (K("--pubkey")) pubkey_override = v;
    else if (K("--relay")) {
      if (n_relays >= MAX_RELAYS) { fprintf(stderr, "too many --relay\n"); return 2; }
      relays[n_relays++] = v;
    }
    else if (K("--cache-dir")) cache_dir = v;
    else if (K("--image-user")) image_user = v;
    else if (K("--image-helper")) image_helper = v;
    else if (K("--throttle-seconds")) throttle_seconds = atoll(v);
    else if (K("--skip-accounts-install")) skip_install = true;
    else if (K("--auth-conf")) auth_conf = v;
    else if (K("--authority-dir")) authority_dir = v;
    else { fprintf(stderr, "unknown flag: %s\n", a); usage(stderr, argv[0]); return 2; }
#undef K
  }

  /* If the caller didn't supply relays on argv, take them from auth.conf's
   * profile_relays list (comma-separated). Then fall back to defaults. */
  nh_auth_conf conf;
  memset(&conf, 0, sizeof conf);
  (void)nh_auth_conf_load(auth_conf, &conf);
  if (n_relays == 0 && conf.profile_relays_count > 0) {
    for (size_t i = 0; i < conf.profile_relays_count && n_relays < MAX_RELAYS; i++)
      relays[n_relays++] = conf.profile_relays[i];
  }
  if (n_relays == 0) {
    for (size_t i = 0; i < DEFAULT_PROFILE_RELAYS_N && n_relays < MAX_RELAYS; i++)
      relays[n_relays++] = DEFAULT_PROFILE_RELAYS[i];
  }

  char pubkey_hex[65];
  if (pubkey_override && *pubkey_override) {
    if (!is_hex64(pubkey_override)) { fprintf(stderr, "--pubkey must be 64 lc-hex\n"); return 2; }
    memcpy(pubkey_hex, pubkey_override, 64);
    pubkey_hex[64] = '\0';
  } else {
    if (lookup_account_pubkey(authority_dir, user, pubkey_hex) != 0) {
      fprintf(stderr, "no such account in authority: %s\n", user);
      return 2;
    }
  }

  nh_profile_refresh_opts opts = {0};
  opts.relays = relays;
  opts.num_relays = n_relays;
  opts.cache_dir = cache_dir;
  opts.image_drop_user = image_user;
  opts.image_helper_path = image_helper;
  opts.throttle_seconds = throttle_seconds;
  opts.skip_accounts_install = skip_install;

  nh_profile_metadata got = {0};
  opts.out_metadata = &got;

  nh_profile_rc rc = nh_profile_refresh(user, pubkey_hex, &opts);
  if (rc == NH_PROFILE_ERR_RATE) {
    fprintf(stderr, "throttled — last refresh too recent\n");
    return 3;
  }
  if (rc != NH_PROFILE_OK) {
    fprintf(stderr, "refresh failed rc=%d\n", (int)rc);
    return 1;
  }
  fprintf(stdout, "installed user=%s name=%s display_name=%s picture=%s\n",
          user, got.name, got.display_name,
          got.picture_url[0] ? "yes" : "no");
  return 0;
}
