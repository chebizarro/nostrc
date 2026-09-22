/* profile_refresh.c — full "fetch + download + install" orchestration.
 *
 * Callers: nostr-homed-profile CLI, seeder --fetch-profile step,
 * broker post-login refresh hook. The orchestrator is deliberately
 * synchronous — the broker's caller invokes it via fork/exec in a
 * detached child, so we never block a PAM conversation.
 *
 * On any per-step failure we log to stderr and press on where it makes
 * sense (e.g. picture failed but display name is fine → still install
 * the name). We only return non-OK when NOTHING useful was installed. */
#define _GNU_SOURCE
#include "nostr_profile.h"
#include "relay_fetch.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_CACHE_DIR   "/var/lib/nostr-auth/profile"
#define DEFAULT_IMAGE_USER  "nobody"

static const char *pick(const char *v, const char *fallback) {
  return (v && *v) ? v : fallback;
}

nh_profile_rc nh_profile_refresh(const char *user, const char *pubkey_hex,
                                 const nh_profile_refresh_opts *opts) {
  if (!user || !*user || !pubkey_hex) return NH_PROFILE_ERR_ARG;
  if (strlen(pubkey_hex) != 64) return NH_PROFILE_ERR_ARG;
  if (!opts) return NH_PROFILE_ERR_ARG;
  if (!opts->relays || opts->num_relays == 0) return NH_PROFILE_ERR_ARG;

  const char *cache_dir = pick(opts->cache_dir, DEFAULT_CACHE_DIR);
  const char *drop_user = pick(opts->image_drop_user, DEFAULT_IMAGE_USER);

  /* Throttle: never touch relays or D-Bus more than once per
   * throttle_seconds per account. Fresh-boot: cache mtime == 0, so we
   * always run the first time. */
  if (opts->throttle_seconds > 0) {
    int64_t last = nh_profile_cache_last_refresh(cache_dir, user);
    if (last > 0) {
      int64_t now = (int64_t)time(NULL);
      if (now - last < opts->throttle_seconds) return NH_PROFILE_ERR_RATE;
    }
  }

  /* Fetch verified kind-0. */
  char *event_json = NULL;
  /* relay_fetch's older API takes non-const char** to match libnostr's
   * SimplePool signature; the caller owns the strings and we don't mutate
   * them, so a defensive cast keeps profile's `const char *const *`
   * contract from leaking through. */
  if (nh_fetch_latest_kind0_verified((const char **)opts->relays,
                                     opts->num_relays,
                                     pubkey_hex, &event_json) != 0) {
    return NH_PROFILE_ERR_FETCH;
  }
  nh_profile_metadata meta;
  memset(&meta, 0, sizeof meta);
  nh_profile_rc prc = nh_profile_parse_event_json(event_json, &meta);
  free(event_json);
  if (prc != NH_PROFILE_OK) return prc;
  /* Belt-and-braces: refuse if event pubkey does not equal the requested
   * one. The fetcher's middleware already enforces this, but we recheck
   * here so a future refactor can't silently regress the invariant. */
  if (strcmp(meta.pubkey_hex, pubkey_hex) != 0) return NH_PROFILE_ERR_PARSE;

  /* Persist the sanitised metadata BEFORE the download so the cache
   * mtime advances even if the picture fails. This drives the throttle. */
  (void)nh_profile_cache_write(cache_dir, user, &meta);

  /* Download the picture (if any) into <cache_dir>/<user>.png. */
  char png_path[PATH_MAX];
  png_path[0] = '\0';
  int have_png = 0;
  if (meta.picture_url[0]) {
    int n = snprintf(png_path, sizeof png_path, "%s/%s.png", cache_dir, user);
    if (n > 0 && (size_t)n < sizeof png_path) {
      nh_profile_rc drc = nh_profile_download_image(opts->image_helper_path,
                                                    meta.picture_url,
                                                    drop_user,
                                                    png_path);
      if (drc == NH_PROFILE_OK) have_png = 1;
      else fprintf(stderr, "nostr-homed-profile: image refused/failed (%d)\n", (int)drc);
    }
  }

  /* Install into AccountsService. Prefer display_name → name. */
  const char *real = NULL;
  if (meta.display_name[0]) real = meta.display_name;
  else if (meta.name[0])    real = meta.name;

  nh_profile_rc arc = NH_PROFILE_OK;
  if (!opts->skip_accounts_install) {
    arc = nh_profile_accounts_install(user, have_png ? png_path : NULL, real);
  }

  if (opts->out_metadata) *opts->out_metadata = meta;
  if (opts->out_png_path && opts->out_png_path_cap > 0) {
    if (have_png) {
      size_t n = strlen(png_path);
      if (n < opts->out_png_path_cap) memcpy(opts->out_png_path, png_path, n + 1);
    } else {
      opts->out_png_path[0] = '\0';
    }
  }

  /* Success rule: OK if either the name or the icon reached AccountsService,
   * OR the caller skipped the install step (dry-run). */
  if (opts->skip_accounts_install) return NH_PROFILE_OK;
  if (arc == NH_PROFILE_OK && (have_png || real)) return NH_PROFILE_OK;
  return NH_PROFILE_ERR_ACCOUNTS;
}
