/*
 * nostr-homectl (headless identity-authority admin CLI).
 *
 * A GLib-free / FUSE-free identity/authority admin subset of the nostr-homed
 * fleet, decoupled from the experimental FUSE roaming stack. Depends only on
 * nostr_identity_core (libsqlite3 + libcrypto). Ships as `nostr-homectl` under
 * NOSTR_HOMED_ENABLE_CTL for the headless nostr-login product.
 *
 * Subcommands
 * ---------------------------------------------------------------------------
 *   check-config [path]         Validate an /etc/nss_nostr.conf-shaped file
 *                               (default /etc/nss_nostr.conf). Accepts the
 *                               canonical `projection_path=...` key the NSS
 *                               module actually reads, plus the legacy
 *                               `db_path=` / `uid_base=` / `uid_range=` set
 *                               used by the older cache/roaming stack.
 *
 *   authority-info <dir>        Open the authority.db + nss.db pair rooted at
 *                               <dir> (matching nostr-authd's convention) and
 *                               print authority_id, generation counters and
 *                               schema version.
 *
 *   show <dir> <username>       Look up <username> in the authority store and
 *                               print the account (uid, gid, status, origin,
 *                               pubkey_hex, home, shell, provider bitmap).
 *
 *   publish-projection <dir>    Re-publish the read-only NSS projection
 *                               (nss.db) from the authority.db as an atomic
 *                               snapshot; prints the new projection_generation.
 *
 *   help | --help | -h          Print this usage summary.
 *
 * No D-Bus. No FUSE. No GLib. No session-bus mounts. This CLI does not manage
 * roaming home mounts -- for the FUSE/D-Bus roaming experiment build the
 * separate NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING flavour.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "nostr_identity.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int usage(FILE *out, const char *argv0) {
  fprintf(out,
    "Usage: %s <command> [args]\n"
    "\n"
    "  check-config [path]           validate an /etc/nss_nostr.conf file\n"
    "                                (default: /etc/nss_nostr.conf)\n"
    "  authority-info <dir>          print authority id / generation / schema\n"
    "  show <dir> <username>         look up an account by username\n"
    "  publish-projection <dir>      re-publish the read-only nss.db snapshot\n"
    "  help | --help | -h            this message\n"
    "\n"
    "<dir> is the nostr-authd state directory. authority.db lives at\n"
    "<dir>/authority.db and the read-only NSS projection at <dir>/nss.db.\n",
    argv0);
  return out == stderr ? 2 : 0;
}

/* --------------------------------------------------------------------------
 * check-config
 * ------------------------------------------------------------------------ */

static int nh_check_config(const char *conf_path) {
  printf("checking %s ...\n", conf_path);
  FILE *f = fopen(conf_path, "r");
  if (!f) {
    fprintf(stderr, "ERROR: cannot open %s: %s\n", conf_path, strerror(errno));
    return 1;
  }
  char projection_path[512] = "";
  char db_path[512] = "";
  int have_projection = 0, have_legacy = 0;
  uint32_t uid_base = 100000u, uid_range = 100000u;
  int errors = 0, warnings = 0, line_no = 0;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    line_no++;
    /* strip trailing newline */
    size_t l = strlen(line);
    while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
    if (l == 0 || line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) {
      fprintf(stderr, "WARN: line %d: no '=' found, skipped\n", line_no);
      warnings++;
      continue;
    }
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;
    if (strcmp(key, "projection_path") == 0) {
      snprintf(projection_path, sizeof projection_path, "%s", val);
      have_projection = 1;
    } else if (strcmp(key, "db_path") == 0) {
      snprintf(db_path, sizeof db_path, "%s", val);
      have_legacy = 1;
    } else if (strcmp(key, "uid_base") == 0) {
      uid_base = (uint32_t)strtoul(val, NULL, 10);
      have_legacy = 1;
    } else if (strcmp(key, "uid_range") == 0) {
      uid_range = (uint32_t)strtoul(val, NULL, 10);
      have_legacy = 1;
    } else {
      fprintf(stderr, "WARN: line %d: unknown key '%s'\n", line_no, key);
      warnings++;
    }
  }
  fclose(f);

  if (!have_projection && !have_legacy) {
    fprintf(stderr,
      "ERROR: %s recognises no known keys (expected projection_path= for the\n"
      "       NSS module, or db_path=/uid_base=/uid_range= for the legacy cache)\n",
      conf_path);
    return 1;
  }

  if (have_projection) {
    printf("  projection_path = %s\n", projection_path);
    if (projection_path[0] != '/') {
      fprintf(stderr, "ERROR: projection_path must be an absolute path\n");
      errors++;
    } else {
      char parent[512];
      snprintf(parent, sizeof parent, "%s", projection_path);
      char *slash = strrchr(parent, '/');
      if (slash && slash != parent) {
        *slash = '\0';
        struct stat st;
        if (stat(parent, &st) != 0) {
          fprintf(stderr,
                  "WARN: projection_path parent '%s' does not exist yet\n",
                  parent);
          warnings++;
        } else if (!S_ISDIR(st.st_mode)) {
          fprintf(stderr,
                  "ERROR: projection_path parent '%s' is not a directory\n",
                  parent);
          errors++;
        } else {
          printf("  projection_path parent '%s': OK\n", parent);
        }
      }
      struct stat pst;
      if (stat(projection_path, &pst) == 0 && !S_ISREG(pst.st_mode)) {
        fprintf(stderr,
                "ERROR: projection_path '%s' exists and is not a regular file\n",
                projection_path);
        errors++;
      }
    }
  }

  if (have_legacy) {
    printf("  db_path   = %s\n",
           db_path[0] ? db_path : "(unset)");
    printf("  uid_base  = %u\n", (unsigned)uid_base);
    printf("  uid_range = %u\n", (unsigned)uid_range);
    if (uid_base <= 1000u) {
      fprintf(stderr,
              "ERROR: uid_base %u <= 1000 (collides with system accounts)\n",
              (unsigned)uid_base);
      errors++;
    }
    if (uid_range < 1000u) {
      fprintf(stderr,
              "ERROR: uid_range %u < 1000 (too few UIDs for production)\n",
              (unsigned)uid_range);
      errors++;
    }
    uint64_t max_uid = (uint64_t)uid_base + (uint64_t)uid_range;
    if (max_uid > 4294967295ULL) {
      fprintf(stderr, "ERROR: uid_base + uid_range exceeds uint32 max\n");
      errors++;
    }
    if (uid_base <= 65534u && uid_base + uid_range > 65534u) {
      fprintf(stderr,
              "WARN: UID range [%u, %u) includes 65534 (nobody)\n",
              (unsigned)uid_base, (unsigned)(uid_base + uid_range));
      warnings++;
    }
  }

  if (errors > 0) {
    fprintf(stderr, "\n%d error(s), %d warning(s)\n", errors, warnings);
    return 1;
  }
  if (warnings > 0)
    printf("\nconfig OK (%d warning(s))\n", warnings);
  else
    printf("\nconfig OK\n");
  return 0;
}

/* --------------------------------------------------------------------------
 * store helpers
 * ------------------------------------------------------------------------ */

/* Read-only ownership probe: nostr-homectl never claims ownership of anything,
 * it only inspects. Reporting FREE means "not held by us"; the store treats
 * this as an advisory probe and will not overwrite an established mapping.
 */
static nh_identity_ownership_result probe_free(
    void *ctx, const char *username, uint32_t uid, uint32_t gid) {
  (void)ctx; (void)username; (void)uid; (void)gid;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

static int open_store_at_dir(const char *dir, nh_identity_store **out_store,
                             nh_identity_config *out_config) {
  if (!dir || !*dir) {
    fprintf(stderr, "ERROR: <dir> is required\n");
    return -1;
  }
  nh_identity_config_defaults(out_config);
  snprintf(out_config->authority_path, sizeof out_config->authority_path,
           "%s/authority.db", dir);
  snprintf(out_config->projection_path, sizeof out_config->projection_path,
           "%s/nss.db", dir);
  snprintf(out_config->home_root, sizeof out_config->home_root, "/home");

  nh_identity_store_options options = {0};
  options.config = out_config;
  options.ownership_probe = probe_free;
  options.flags = 0;

  nh_identity_rc rc = nh_identity_store_open(&options, out_store);
  if (rc != NH_IDENTITY_OK) {
    fprintf(stderr,
            "ERROR: cannot open authority at %s: %s (%s)\n",
            out_config->authority_path, nh_identity_rc_name(rc),
            (*out_store) ? nh_identity_store_error_detail(*out_store) : "no detail");
    if (*out_store) nh_identity_store_close(*out_store);
    *out_store = NULL;
    return -1;
  }
  return 0;
}

/* --------------------------------------------------------------------------
 * authority-info
 * ------------------------------------------------------------------------ */

static int nh_authority_info(const char *dir) {
  nh_identity_store *store = NULL;
  nh_identity_config config;
  if (open_store_at_dir(dir, &store, &config) != 0) return 1;
  nh_identity_store_info info;
  memset(&info, 0, sizeof info);
  int rc = 0;
  nh_identity_rc grc = nh_identity_store_get_info(store, &info);
  if (grc != NH_IDENTITY_OK) {
    fprintf(stderr, "ERROR: nh_identity_store_get_info: %s (%s)\n",
            nh_identity_rc_name(grc),
            nh_identity_store_error_detail(store));
    rc = 1;
    goto out;
  }
  printf("authority_path        = %s\n", config.authority_path);
  printf("projection_path       = %s\n", config.projection_path);
  printf("authority_id          = %s\n", info.authority_id);
  printf("authority_generation  = %" PRIu64 "\n", info.authority_generation);
  printf("projection_generation = %" PRIu64 "\n", info.projection_generation);
  printf("schema_version        = %u\n", (unsigned)info.schema_version);
out:
  nh_identity_store_close(store);
  return rc;
}

/* --------------------------------------------------------------------------
 * show
 * ------------------------------------------------------------------------ */

static const char *origin_name(nh_identity_origin o) {
  switch (o) {
    case NH_IDENTITY_ORIGIN_ENROLLED:      return "enrolled";
    case NH_IDENTITY_ORIGIN_LEGACY_IMPORT: return "legacy_import";
    default:                               return "unknown";
  }
}

static int nh_show(const char *dir, const char *username) {
  if (!username || !*username) {
    fprintf(stderr, "ERROR: <username> is required\n");
    return 2;
  }
  nh_identity_store *store = NULL;
  nh_identity_config config;
  if (open_store_at_dir(dir, &store, &config) != 0) return 1;
  nh_identity_account acct;
  memset(&acct, 0, sizeof acct);
  int rc = 0;
  nh_identity_rc lrc = nh_identity_store_lookup_by_name(store, username, &acct);
  if (lrc == NH_IDENTITY_NOT_FOUND) {
    fprintf(stderr, "not_found: %s\n", username);
    rc = 3;
    goto out;
  }
  if (lrc != NH_IDENTITY_OK) {
    fprintf(stderr, "ERROR: lookup_by_name(%s): %s (%s)\n", username,
            nh_identity_rc_name(lrc),
            nh_identity_store_error_detail(store));
    rc = 1;
    goto out;
  }
  printf("username           = %s\n", acct.username);
  printf("account_id         = %s\n", acct.account_id);
  printf("uid                = %u\n", (unsigned)acct.uid);
  printf("gid                = %u\n", (unsigned)acct.gid);
  printf("home               = %s\n", acct.home);
  printf("shell              = %s\n", acct.shell);
  printf("pubkey_hex         = %s\n", acct.pubkey_hex);
  printf("status             = %s\n", nh_identity_status_name(acct.status));
  printf("origin             = %s\n", origin_name(acct.origin));
  printf("key_generation     = %" PRIu64 "\n", acct.key_generation);
  printf("authority_gen      = %" PRIu64 "\n", acct.authority_generation);
  printf("enabled_providers  = 0x%08x\n", (unsigned)acct.enabled_providers);
  printf("projectable        = %s\n", acct.projectable ? "yes" : "no");
out:
  nh_identity_store_close(store);
  return rc;
}

/* --------------------------------------------------------------------------
 * publish-projection
 * ------------------------------------------------------------------------ */

static int nh_publish_projection(const char *dir) {
  nh_identity_store *store = NULL;
  nh_identity_config config;
  if (open_store_at_dir(dir, &store, &config) != 0) return 1;
  uint64_t generation = 0;
  int rc = 0;
  nh_identity_rc prc =
      nh_identity_store_publish_projection(store, &generation);
  if (prc != NH_IDENTITY_OK) {
    fprintf(stderr, "ERROR: publish_projection: %s (%s)\n",
            nh_identity_rc_name(prc),
            nh_identity_store_error_detail(store));
    rc = 1;
    goto out;
  }
  printf("published %s @ generation %" PRIu64 "\n",
         config.projection_path, generation);
out:
  nh_identity_store_close(store);
  return rc;
}

/* --------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------ */

int main(int argc, char **argv) {
  if (argc < 2) return usage(stderr, argv[0]);
  const char *cmd = argv[1];
  if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
    return usage(stdout, argv[0]);
  }
  if (!strcmp(cmd, "check-config")) {
    const char *path = (argc >= 3) ? argv[2] : "/etc/nss_nostr.conf";
    return nh_check_config(path);
  }
  if (!strcmp(cmd, "authority-info")) {
    if (argc < 3) return usage(stderr, argv[0]);
    return nh_authority_info(argv[2]);
  }
  if (!strcmp(cmd, "show")) {
    if (argc < 4) return usage(stderr, argv[0]);
    return nh_show(argv[2], argv[3]);
  }
  if (!strcmp(cmd, "publish-projection")) {
    if (argc < 3) return usage(stderr, argv[0]);
    return nh_publish_projection(argv[2]);
  }
  fprintf(stderr, "ERROR: unknown command '%s'\n\n", cmd);
  return usage(stderr, argv[0]);
}
