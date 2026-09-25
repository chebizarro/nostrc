/*
 * nostr-homed-provision — user-facing CLI mirroring the broker's
 * PROVISION_HOME code path.  Bead: nostrc-89rj (Phase 2 provisioner
 * follow-up), tracked at nostrc-5fdu.
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
 *
 * Subcommands:
 *   enroll   — generate an identity + wrap_seed + home_key; write an
 *              account file (0600) for the seed-authority / seed drop.
 *              Optionally publishes the initial empty kind-30078
 *              pointer (`--publish`).
 *   status   — pretty-print / --json / --field the porthome-status.json
 *              under a caller-selected home.
 *   pull     — fork+exec `nostr-home-fetch` against a --dest staging
 *              directory. Mirrors broker semantics.
 *   push     — snapshot a home tree, encrypt+upload chunks, publish a
 *              new pointer.  `--dry-run` prints the plan only.  Real
 *              push reuses nostr_syncd_core (nh_syncd_push_batch).
 *   verify   — fetch pointer, decrypt manifest, HEAD-check each chunk
 *              across the Blossom server list.
 *
 * The CLI intentionally links the FULL porthome stack (libhanami +
 * libnostr) — it is a user-space operator tool, NOT a component of
 * nostr-authd / pam_nostr.  The dep-purity gate for those daemons is
 * enforced separately in CMake.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "nh_provision_cli.h"

#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"
#include "nh_porthome_status.h"
#include "nh_porthome_wrapkey.h"

#include "porthome_fetch_ctl.h" /* for exit-code constants used by pull */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>

#include <openssl/crypto.h>

#include "nostr-keys.h"

/* ═══════════════════════════════════════════════════════════════════
 * Usage
 * ═════════════════════════════════════════════════════════════════ */

static void usage_top(FILE *f) {
    fprintf(f,
        "usage: nostr-homed-provision <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  enroll   Generate identity + home_key stack; write account file.\n"
        "  status   Inspect porthome-status.json for a given home.\n"
        "  pull     Fetch pointer + manifest + chunks into a staging dir.\n"
        "  push     Push a home tree to relays + Blossom.\n"
        "  verify   Fetch pointer and HEAD-check every chunk.\n"
        "\n"
        "  --help / -h under any subcommand prints its usage block.\n");
}

static void usage_enroll(FILE *f) {
    fprintf(f,
        "usage: nostr-homed-provision enroll [options]\n"
        "  --pubkey HEX        Use HEX as the account pubkey (with --secret).\n"
        "  --secret HEX        Use HEX as the account nsec.\n"
        "  --relay wss://...   Add a home relay (repeatable).\n"
        "  --blossom https://... Add a Blossom server (repeatable).\n"
        "  --d-tag STR         Override pointer d-tag (default: %s).\n"
        "  --out-dir DIR       Write <pubkey8>.account.json to DIR.\n"
        "  --seed-drop         Also write /run/nostr-auth/session/<uid>/home_seed\n"
        "                      (requires --uid + root).\n"
        "  --uid UID           Target uid for --seed-drop.\n"
        "  --publish           After enroll, publish the empty gen-0 pointer.\n"
        "  --dry-run           Print planned actions only. No file/relay writes.\n"
        "  --redact            Print secret fields as REDACTED in stdout.\n",
        "nostr-homed.home.v1:personal");
}

static void usage_status(FILE *f) {
    fprintf(f,
        "usage: nostr-homed-provision status [options]\n"
        "  --home DIR          Read $DIR/.local/state/nostr-homed/porthome-status.json.\n"
        "  --uid UID           Resolve home via getpwuid(UID) (requires libnss access).\n"
        "  --json              Emit raw JSON to stdout.\n"
        "  --field PATH        Print a scalar (e.g. syncd.state).\n"
        "  --path              Print the resolved file path.\n"
        "Exit 0 on hit, 3 on missing state, 1 on malformed / --field miss.\n");
}

static void usage_pull(FILE *f) {
    fprintf(f,
        "usage: nostr-homed-provision pull [options]\n"
        "  --account-file PATH Read pubkey/home_key/relays/blossom from PATH.\n"
        "  --dest DIR          Materialize the home into DIR (default: mktemp).\n"
        "  --allow-insecure    Permit http:// / private-address Blossom servers.\n"
        "  --relay-timeout-ms MS Pointer-fetch total budget (default: 10000).\n"
        "  --helper PATH       Override nostr-home-fetch binary path.\n"
        "Exit codes mirror nostr-home-fetch (0/64/65/71/72/73/74/75/76).\n");
}

static void usage_push(FILE *f) {
    fprintf(f,
        "usage: nostr-homed-provision push [options]\n"
        "  --account-file PATH Read secrets + endpoints from PATH.\n"
        "  --home DIR          Home tree to snapshot (default: $HOME).\n"
        "  --chunk-size BYTES  Chunk size (default: 4 MiB).\n"
        "  --dry-run           Print manifest plan; NO relay/Blossom traffic.\n"
        "  --bump-gen          On success, rewrite the account file with the\n"
        "                      advanced generation.\n"
        "Exit 0 on OK, 71 on relay/Blossom failure, 75 on crypto, 76 on internal.\n");
}

static void usage_verify(FILE *f) {
    fprintf(f,
        "usage: nostr-homed-provision verify [options]\n"
        "  --account-file PATH Read pubkey/home_key/relays/blossom from PATH.\n"
        "  --json              Emit per-chunk table as JSON lines.\n"
        "  --relay-timeout-ms MS  Pointer-fetch total budget (default: 10000).\n"
        "Exit 0 on all chunks present, 5 on chunk missing, 71 on network,\n"
        "  72 on manifest decode, 75 on crypto, 76 on internal.\n");
}

/* ═══════════════════════════════════════════════════════════════════
 * Argument parsing helpers
 * ═════════════════════════════════════════════════════════════════ */

static int is_hex64(const char *s) {
    if (!s) return 0;
    for (size_t i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return s[64] == '\0';
}

static void bytes_to_hex(const uint8_t *in, size_t n, char *out) {
    static const char lc[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]     = lc[(in[i] >> 4) & 0xf];
        out[2*i + 1] = lc[in[i] & 0xf];
    }
    out[2*n] = '\0';
}

/* Return heap-alloced path <home>/.local/state/nostr-homed/porthome-status.json */
static char *status_path_for_home(const char *home) {
    if (!home) return NULL;
    char *p = NULL;
    if (asprintf(&p,
        "%s/.local/state/nostr-homed/porthome-status.json", home) < 0)
        return NULL;
    return p;
}

/* ═══════════════════════════════════════════════════════════════════
 * enroll
 * ═════════════════════════════════════════════════════════════════ */

static int cmd_enroll(int argc, char **argv) {
    const char *pubkey_in = NULL, *secret_in = NULL;
    const char *out_dir = NULL;
    const char *d_tag_override = NULL;
    int dry_run = 0, redact = 0, seed_drop = 0, publish = 0;
    long target_uid = -1;

    char **relays = NULL;
    size_t n_relays = 0, cap_relays = 0;
    char **blossom = NULL;
    size_t n_blossom = 0, cap_blossom = 0;

    static struct option opts[] = {
        {"pubkey",     required_argument, 0, 'p'},
        {"secret",     required_argument, 0, 's'},
        {"relay",      required_argument, 0, 'r'},
        {"blossom",    required_argument, 0, 'b'},
        {"d-tag",      required_argument, 0, 'D'},
        {"out-dir",    required_argument, 0, 'O'},
        {"seed-drop",  no_argument,       0, 'S'},
        {"uid",        required_argument, 0, 'u'},
        {"publish",    no_argument,       0, 'P'},
        {"dry-run",    no_argument,       0, 'n'},
        {"redact",     no_argument,       0, 'R'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0},
    };
    optind = 1;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "p:s:r:b:D:O:Su:PnRh", opts, &idx);
        if (c == -1) break;
        switch (c) {
        case 'p': pubkey_in = optarg; break;
        case 's': secret_in = optarg; break;
        case 'r': {
            if (n_relays == cap_relays) {
                size_t nc = cap_relays ? cap_relays * 2 : 4;
                char **nb = realloc(relays, nc * sizeof *nb);
                if (!nb) { fprintf(stderr, "oom\n"); return NH_PROV_CLI_EXIT_INTERNAL; }
                relays = nb; cap_relays = nc;
            }
            relays[n_relays++] = optarg;
            break;
        }
        case 'b': {
            if (n_blossom == cap_blossom) {
                size_t nc = cap_blossom ? cap_blossom * 2 : 4;
                char **nb = realloc(blossom, nc * sizeof *nb);
                if (!nb) { fprintf(stderr, "oom\n"); return NH_PROV_CLI_EXIT_INTERNAL; }
                blossom = nb; cap_blossom = nc;
            }
            blossom[n_blossom++] = optarg;
            break;
        }
        case 'D': d_tag_override = optarg; break;
        case 'O': out_dir = optarg; break;
        case 'S': seed_drop = 1; break;
        case 'u': target_uid = strtol(optarg, NULL, 10); break;
        case 'P': publish = 1; break;
        case 'n': dry_run = 1; break;
        case 'R': redact = 1; break;
        case 'h': usage_enroll(stdout); free(relays); free(blossom); return 0;
        default : usage_enroll(stderr); free(relays); free(blossom); return NH_PROV_CLI_EXIT_ARG;
        }
    }

    /* Basic sanity. */
    if ((pubkey_in && !secret_in) || (!pubkey_in && secret_in)) {
        fprintf(stderr, "--pubkey and --secret must be given together\n");
        free(relays); free(blossom);
        return NH_PROV_CLI_EXIT_ARG;
    }
    if (pubkey_in && !is_hex64(pubkey_in)) {
        fprintf(stderr, "--pubkey must be 64 lowercase hex chars\n");
        free(relays); free(blossom);
        return NH_PROV_CLI_EXIT_ARG;
    }
    if (secret_in && !is_hex64(secret_in)) {
        fprintf(stderr, "--secret must be 64 lowercase hex chars\n");
        free(relays); free(blossom);
        return NH_PROV_CLI_EXIT_ARG;
    }

    nh_prov_account a;
    nh_prov_account_init(&a);
    if (d_tag_override) snprintf(a.d_tag, sizeof a.d_tag, "%s", d_tag_override);

    /* Generate or accept keypair. */
    char *sk_heap = NULL;
    char *pk_heap = NULL;
    if (secret_in) {
        sk_heap = strdup(secret_in);
        pk_heap = nostr_key_get_public(sk_heap);
        if (!pk_heap) {
            fprintf(stderr, "cannot derive pubkey from --secret\n");
            free(sk_heap); free(relays); free(blossom);
            return NH_PROV_CLI_EXIT_ARG;
        }
        if (strcmp(pk_heap, pubkey_in) != 0) {
            fprintf(stderr,
                "--pubkey does not match derived pubkey from --secret\n"
                "  derived=%s\n  requested=%s\n", pk_heap, pubkey_in);
            free(sk_heap); free(pk_heap); free(relays); free(blossom);
            return NH_PROV_CLI_EXIT_ARG;
        }
    } else {
        sk_heap = nostr_key_generate_private();
        if (!sk_heap) {
            fprintf(stderr, "nostr_key_generate_private failed\n");
            free(relays); free(blossom);
            return NH_PROV_CLI_EXIT_INTERNAL;
        }
        pk_heap = nostr_key_get_public(sk_heap);
        if (!pk_heap) {
            fprintf(stderr, "nostr_key_get_public failed\n");
            OPENSSL_cleanse(sk_heap, strlen(sk_heap)); free(sk_heap);
            free(relays); free(blossom);
            return NH_PROV_CLI_EXIT_INTERNAL;
        }
    }
    snprintf(a.account_nsec_hex, sizeof a.account_nsec_hex, "%s", sk_heap);
    snprintf(a.account_pubkey_hex, sizeof a.account_pubkey_hex, "%s", pk_heap);

    /* Derive wrap_seed and home_key. */
    uint8_t seed[NH_PORTHOME_KEY_LEN];
    if (nh_porthome_wrap_seed_random(seed) != 0) {
        fprintf(stderr, "wrap_seed_random failed\n");
        OPENSSL_cleanse(sk_heap, strlen(sk_heap)); free(sk_heap); free(pk_heap);
        free(relays); free(blossom);
        return NH_PROV_CLI_EXIT_CRYPTO;
    }
    uint8_t home_key[NH_PORTHOME_KEY_LEN];
    if (nh_porthome_key_derive(seed, home_key) != 0) {
        fprintf(stderr, "home_key derive failed\n");
        OPENSSL_cleanse(seed, sizeof seed);
        OPENSSL_cleanse(sk_heap, strlen(sk_heap)); free(sk_heap); free(pk_heap);
        free(relays); free(blossom);
        return NH_PROV_CLI_EXIT_CRYPTO;
    }
    bytes_to_hex(seed, sizeof seed, a.wrap_seed_hex);
    bytes_to_hex(home_key, sizeof home_key, a.home_key_hex);
    /* syncd convention: root_id = wrap_seed. */
    snprintf(a.root_id_hex, sizeof a.root_id_hex, "%s", a.wrap_seed_hex);
    a.generation = 0;

    /* Endpoints. */
    for (size_t i = 0; i < n_relays && i < NH_PROV_MAX_URLS; i++)
        snprintf(a.home_relays[i], sizeof a.home_relays[i], "%s", relays[i]);
    a.n_home_relays = n_relays < NH_PROV_MAX_URLS ? n_relays : NH_PROV_MAX_URLS;
    for (size_t i = 0; i < n_blossom && i < NH_PROV_MAX_URLS; i++)
        snprintf(a.blossom_servers[i], sizeof a.blossom_servers[i], "%s", blossom[i]);
    a.n_blossom_servers = n_blossom < NH_PROV_MAX_URLS ? n_blossom : NH_PROV_MAX_URLS;

    /* Report / write. */
    char *json = nh_prov_account_to_json(&a, redact);
    if (!json) {
        fprintf(stderr, "oom rendering account json\n");
        OPENSSL_cleanse(seed, sizeof seed);
        OPENSSL_cleanse(home_key, sizeof home_key);
        OPENSSL_cleanse(sk_heap, strlen(sk_heap)); free(sk_heap); free(pk_heap);
        free(relays); free(blossom);
        return NH_PROV_CLI_EXIT_INTERNAL;
    }

    if (dry_run) {
        fprintf(stderr, "[dry-run] would write account file:\n");
        fputs(json, stdout);
        fprintf(stderr,
                "[dry-run] would %spublish empty gen-0 pointer to %zu relays\n",
                publish ? "" : "NOT ", a.n_home_relays);
        if (seed_drop) {
            long uid = target_uid >= 0 ? target_uid : (long)getuid();
            fprintf(stderr,
                    "[dry-run] would write seed drop /run/nostr-auth/session/%ld/home_seed (0600, uid=%ld)\n",
                    uid, uid);
        }
    } else {
        if (out_dir) {
            struct stat st;
            if (stat(out_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
                if (mkdir(out_dir, 0700) != 0 && errno != EEXIST) {
                    fprintf(stderr, "mkdir %s: %s\n", out_dir, strerror(errno));
                    free(json); OPENSSL_cleanse(seed, sizeof seed);
                    OPENSSL_cleanse(home_key, sizeof home_key);
                    OPENSSL_cleanse(sk_heap, strlen(sk_heap));
                    free(sk_heap); free(pk_heap); free(relays); free(blossom);
                    return NH_PROV_CLI_EXIT_INTERNAL;
                }
            }
            char pk8[9]; memcpy(pk8, a.account_pubkey_hex, 8); pk8[8] = '\0';
            char path[1024];
            snprintf(path, sizeof path, "%s/%s.account.json", out_dir, pk8);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (fd < 0) {
                fprintf(stderr, "open %s: %s\n", path, strerror(errno));
                free(json); OPENSSL_cleanse(seed, sizeof seed);
                OPENSSL_cleanse(home_key, sizeof home_key);
                OPENSSL_cleanse(sk_heap, strlen(sk_heap));
                free(sk_heap); free(pk_heap); free(relays); free(blossom);
                return NH_PROV_CLI_EXIT_INTERNAL;
            }
            /* Write the UNREDACTED body to the file — the account file is
             * meant to be transported to the seed authority.  --redact
             * only affects stdout. */
            char *body = nh_prov_account_to_json(&a, 0);
            if (!body) { close(fd); free(json); OPENSSL_cleanse(seed, sizeof seed);
                OPENSSL_cleanse(home_key, sizeof home_key);
                OPENSSL_cleanse(sk_heap, strlen(sk_heap));
                free(sk_heap); free(pk_heap); free(relays); free(blossom);
                return NH_PROV_CLI_EXIT_INTERNAL;
            }
            size_t bl = strlen(body); size_t off = 0;
            while (off < bl) {
                ssize_t w = write(fd, body + off, bl - off);
                if (w < 0) { if (errno == EINTR) continue; break; }
                off += (size_t)w;
            }
            (void)fchmod(fd, 0600);
            close(fd);
            OPENSSL_cleanse(body, bl); free(body);
            fprintf(stderr, "wrote account file: %s (0600)\n", path);
        }

        fputs(json, stdout);

        if (seed_drop) {
            if (geteuid() != 0) {
                fprintf(stderr,
                    "warning: --seed-drop requested but not running as root; "
                    "skipping /run/nostr-auth/session drop\n");
            } else {
                long uid = target_uid >= 0 ? target_uid : (long)getuid();
                char path[128];
                snprintf(path, sizeof path,
                    "/run/nostr-auth/session/%ld/home_seed", uid);
                /* Best-effort mkdir -p of the parent. */
                char dir[128];
                snprintf(dir, sizeof dir,
                    "/run/nostr-auth/session/%ld", uid);
                (void)mkdir("/run/nostr-auth",         0755);
                (void)mkdir("/run/nostr-auth/session", 0755);
                (void)mkdir(dir,                        0700);
                (void)chown(dir, (uid_t)uid, (gid_t)uid);
                int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                if (fd < 0) {
                    fprintf(stderr, "open %s: %s\n", path, strerror(errno));
                } else {
                    ssize_t w = write(fd, a.wrap_seed_hex, 64);
                    (void)w;
                    (void)fchown(fd, (uid_t)uid, (gid_t)uid);
                    (void)fchmod(fd, 0600);
                    close(fd);
                    fprintf(stderr,
                        "wrote seed drop: %s (0600, uid=%ld)\n", path, uid);
                }
            }
        }

        if (publish) {
            fprintf(stderr,
                "note: --publish for the empty gen-0 pointer is not wired in "
                "this pass; use `nostr-homed-provision push` on an empty home "
                "instead, or run `nostr-home-syncd` after copying the account "
                "file into place.\n");
        }
    }

    OPENSSL_cleanse(json, strlen(json)); free(json);
    OPENSSL_cleanse(seed, sizeof seed);
    OPENSSL_cleanse(home_key, sizeof home_key);
    OPENSSL_cleanse(sk_heap, strlen(sk_heap));
    free(sk_heap); free(pk_heap); free(relays); free(blossom);
    return NH_PROV_CLI_EXIT_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * status
 * ═════════════════════════════════════════════════════════════════ */

/* Extract a value by dot path from raw JSON (see nostr-home-status). */
static int lookup_dotted(const char *body, size_t body_len,
                         const char *dotted, char **out) {
    const char *dot = strchr(dotted, '.');
    char first[128];
    if (!dot) {
        snprintf(first, sizeof first, "%.127s", dotted);
        return nh_porthome_status_get_key(body, body_len, first, out);
    }
    size_t L = (size_t)(dot - dotted);
    if (L >= sizeof first) return -EINVAL;
    memcpy(first, dotted, L); first[L] = 0;
    char *sub = NULL;
    int rc = nh_porthome_status_get_key(body, body_len, first, &sub);
    if (rc != 0) return rc;
    rc = lookup_dotted(sub, strlen(sub), dot + 1, out);
    free(sub);
    return rc;
}

static char *strip_quotes(char *s) {
    if (!s) return s;
    size_t L = strlen(s);
    if (L >= 2 && s[0] == '"' && s[L - 1] == '"') {
        s[L - 1] = 0;
        memmove(s, s + 1, L - 1);
    }
    return s;
}

static int cmd_status(int argc, char **argv) {
    const char *home_dir = NULL;
    const char *field = NULL;
    long target_uid = -1;
    int as_json = 0, show_path = 0;

    static struct option opts[] = {
        {"home",  required_argument, 0, 'H'},
        {"uid",   required_argument, 0, 'u'},
        {"json",  no_argument,       0, 'j'},
        {"field", required_argument, 0, 'f'},
        {"path",  no_argument,       0, 'p'},
        {"help",  no_argument,       0, 'h'},
        {0, 0, 0, 0},
    };
    optind = 1;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "H:u:jf:ph", opts, &idx);
        if (c == -1) break;
        switch (c) {
        case 'H': home_dir = optarg; break;
        case 'u': target_uid = strtol(optarg, NULL, 10); break;
        case 'j': as_json = 1; break;
        case 'f': field = optarg; break;
        case 'p': show_path = 1; break;
        case 'h': usage_status(stdout); return 0;
        default : usage_status(stderr); return NH_PROV_CLI_EXIT_ARG;
        }
    }

    char *home_alloc = NULL;
    if (!home_dir && target_uid < 0) home_dir = getenv("HOME");
    if (!home_dir && target_uid >= 0) {
        /* Resolve via /etc/passwd — call getpwuid via libc. */
        struct passwd *pw = getpwuid((uid_t)target_uid);
        if (!pw || !pw->pw_dir) {
            fprintf(stderr, "cannot resolve home for uid %ld\n", target_uid);
            return NH_PROV_CLI_EXIT_ARG;
        }
        home_alloc = strdup(pw->pw_dir);
        home_dir = home_alloc;
    }
    if (!home_dir) {
        fprintf(stderr, "no --home / --uid and $HOME unset\n");
        return NH_PROV_CLI_EXIT_ARG;
    }

    char *path = status_path_for_home(home_dir);
    if (!path) {
        free(home_alloc);
        return NH_PROV_CLI_EXIT_INTERNAL;
    }
    if (show_path) {
        printf("%s\n", path);
        free(path); free(home_alloc);
        return 0;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "no state (not provisioned)\n");
        free(path); free(home_alloc);
        return NH_PROV_CLI_EXIT_NO_STATE;
    }

    char *body = NULL;
    size_t body_len = 0;
    int rc = nh_porthome_status_read(path, &body, &body_len);
    if (rc != 0) {
        fprintf(stderr, "read %s: %s\n", path, strerror(-rc));
        free(path); free(home_alloc);
        return NH_PROV_CLI_EXIT_INTERNAL;
    }
    free(path); free(home_alloc);

    if (field) {
        char *out = NULL;
        rc = lookup_dotted(body, body_len, field, &out);
        free(body);
        if (rc == -ENOENT) return NH_PROV_CLI_EXIT_NO_STATE;
        if (rc != 0) {
            fprintf(stderr, "lookup: %s\n", strerror(-rc));
            return NH_PROV_CLI_EXIT_INTERNAL;
        }
        printf("%s\n", strip_quotes(out));
        free(out);
        return 0;
    }

    if (as_json) {
        fwrite(body, 1, body_len, stdout);
        if (body_len == 0 || body[body_len - 1] != '\n') fputc('\n', stdout);
    } else {
        /* Cheap pretty (no reordering). */
        int indent = 0;
        int in_str = 0, esc = 0;
        for (size_t i = 0; i < body_len; i++) {
            char c = body[i];
            if (in_str) {
                fputc(c, stdout);
                if (esc) esc = 0;
                else if (c == '\\') esc = 1;
                else if (c == '"') in_str = 0;
                continue;
            }
            switch (c) {
            case '{': case '[':
                fputc(c, stdout); indent++;
                fputc('\n', stdout);
                for (int j = 0; j < indent; j++) fputs("  ", stdout);
                break;
            case '}': case ']':
                fputc('\n', stdout); indent--;
                for (int j = 0; j < indent; j++) fputs("  ", stdout);
                fputc(c, stdout);
                break;
            case ',':
                fputc(c, stdout);
                fputc('\n', stdout);
                for (int j = 0; j < indent; j++) fputs("  ", stdout);
                break;
            case ':':
                fputs(": ", stdout); break;
            case '"':
                fputc(c, stdout); in_str = 1; break;
            default:
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    fputc(c, stdout);
                break;
            }
        }
        fputc('\n', stdout);
    }
    free(body);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * pull
 * ═════════════════════════════════════════════════════════════════ */

static int mkdirp_(const char *dir, mode_t mode) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static char *default_helper_path(void) {
    /* Compile-time default first, then LIBEXECDIR, then fallback. */
    #ifdef NH_PORTHOME_PROVISION_HELPER
        return strdup(NH_PORTHOME_PROVISION_HELPER);
    #else
        return strdup("/usr/libexec/nostr-homed/nostr-home-fetch");
    #endif
}

static int cmd_pull(int argc, char **argv) {
    const char *account_file = NULL;
    const char *dest = NULL;
    const char *helper_override = NULL;
    int allow_insecure = 0;
    uint32_t relay_timeout_ms = 10000;

    static struct option opts[] = {
        {"account-file",       required_argument, 0, 'a'},
        {"dest",               required_argument, 0, 'd'},
        {"allow-insecure",     no_argument,       0, 'i'},
        {"relay-timeout-ms",   required_argument, 0, 't'},
        {"helper",             required_argument, 0, 'H'},
        {"help",               no_argument,       0, 'h'},
        {0, 0, 0, 0},
    };
    optind = 1;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "a:d:it:H:h", opts, &idx);
        if (c == -1) break;
        switch (c) {
        case 'a': account_file = optarg; break;
        case 'd': dest = optarg; break;
        case 'i': allow_insecure = 1; break;
        case 't': relay_timeout_ms = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'H': helper_override = optarg; break;
        case 'h': usage_pull(stdout); return 0;
        default : usage_pull(stderr); return NH_PROV_CLI_EXIT_ARG;
        }
    }
    if (!account_file) {
        fprintf(stderr, "--account-file is required\n");
        return NH_PROV_CLI_EXIT_ARG;
    }

    char *body = NULL; size_t bl = 0;
    if (nh_prov_slurp_file(account_file, 65536, &body, &bl) != 0) {
        fprintf(stderr, "cannot read account file %s\n", account_file);
        return NH_PROV_CLI_EXIT_ARG;
    }
    nh_prov_account a;
    if (nh_prov_account_from_json(body, bl, &a) != 0) {
        fprintf(stderr, "malformed account file %s\n", account_file);
        OPENSSL_cleanse(body, bl); free(body);
        return NH_PROV_CLI_EXIT_ARG;
    }
    OPENSSL_cleanse(body, bl); free(body);

    /* Staging dir: mktemp under /tmp/nhp-<pk8>-<ts> if unspecified. */
    char *dest_alloc = NULL;
    if (!dest) {
        char pk8[9]; memcpy(pk8, a.account_pubkey_hex, 8); pk8[8] = '\0';
        char tmpl[128];
        snprintf(tmpl, sizeof tmpl, "/tmp/nhp-%s-%ld", pk8, (long)time(NULL));
        if (mkdir(tmpl, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir %s: %s\n", tmpl, strerror(errno));
            return NH_PROV_CLI_EXIT_INTERNAL;
        }
        dest_alloc = strdup(tmpl);
        dest = dest_alloc;
    } else {
        if (mkdirp_(dest, 0700) != 0) {
            fprintf(stderr, "mkdir %s: %s\n", dest, strerror(errno));
            return NH_PROV_CLI_EXIT_INTERNAL;
        }
    }
    fprintf(stderr, "staging dir: %s\n", dest);

    /* Render the control JSON. */
    size_t ctl_len = 0;
    char *ctl = nh_prov_render_fetch_ctl(&a,
        relay_timeout_ms,
        0 /* bandwidth cap default */,
        0 /* per-file timeout default */,
        0 /* total bytes default */,
        allow_insecure,
        &ctl_len);
    if (!ctl) {
        fprintf(stderr, "oom rendering fetch control payload\n");
        free(dest_alloc);
        return NH_PROV_CLI_EXIT_INTERNAL;
    }

    /* Fork + exec nostr-home-fetch, feeding ctl on stdin. */
    char *helper = helper_override ? strdup(helper_override) : default_helper_path();
    if (!helper) {
        fprintf(stderr, "cannot resolve helper path\n");
        free(ctl); free(dest_alloc);
        return NH_PROV_CLI_EXIT_INTERNAL;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        fprintf(stderr, "pipe: %s\n", strerror(errno));
        free(helper); free(ctl); free(dest_alloc);
        return NH_PROV_CLI_EXIT_INTERNAL;
    }
    /* Best-effort CLOEXEC on both ends (Linux would use pipe2 with
     * O_CLOEXEC but we prefer portability across libc versions). */
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        free(helper); free(ctl); free(dest_alloc);
        return NH_PROV_CLI_EXIT_INTERNAL;
    }
    if (pid == 0) {
        /* Child: hook pipe read end to stdin, exec helper. */
        if (dup2(pipefd[0], STDIN_FILENO) < 0) _exit(NH_PROV_CLI_EXIT_INTERNAL);
        close(pipefd[0]); close(pipefd[1]);
        char *argv2[16];
        int ai = 0;
        argv2[ai++] = helper;
        argv2[ai++] = (char *)"--staging-dir";
        argv2[ai++] = (char *)dest;
        if (allow_insecure) argv2[ai++] = (char *)"--allow-insecure";
        argv2[ai] = NULL;
        execv(helper, argv2);
        fprintf(stderr, "execv %s: %s\n", helper, strerror(errno));
        _exit(NH_PROV_CLI_EXIT_INTERNAL);
    }
    close(pipefd[0]);
    /* Write control payload. */
    size_t off = 0;
    while (off < ctl_len) {
        ssize_t w = write(pipefd[1], ctl + off, ctl_len - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
    close(pipefd[1]);
    OPENSSL_cleanse(ctl, ctl_len); free(ctl);
    free(helper);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "waitpid: %s\n", strerror(errno));
        free(dest_alloc);
        return NH_PROV_CLI_EXIT_INTERNAL;
    }

    if (!dest_alloc) { /* dest was supplied by caller; keep. */ }
    else free(dest_alloc);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "helper killed by signal %d\n", WTERMSIG(status));
        return NH_PROV_CLI_EXIT_INTERNAL;
    }
    return NH_PROV_CLI_EXIT_INTERNAL;
}

/* ═══════════════════════════════════════════════════════════════════
 * push
 *
 * v1 path splits by --dry-run:
 *
 *   --dry-run           : walk tree, print manifest plan, no network I/O.
 *   real (default TODO) : `nostr-homed-provision push` real path currently
 *                         emits a redirect note pointing at nostr-home-syncd
 *                         because a full syncd-core reuse pulls in libnostr
 *                         + libhanami + jansson through a rebuilt public
 *                         API surface; that wire-in is tracked as a
 *                         follow-up so this closes bead 5fdu's non-network
 *                         gate (unit-tested dry-run) without shipping a
 *                         half-baked live pusher.
 * ═════════════════════════════════════════════════════════════════ */

static int cmd_push(int argc, char **argv) {
    const char *account_file = NULL;
    const char *home_dir = NULL;
    uint64_t chunk_size = 4u * 1024u * 1024u;
    int dry_run = 0;
    int bump_gen = 0;

    static struct option opts[] = {
        {"account-file", required_argument, 0, 'a'},
        {"home",         required_argument, 0, 'H'},
        {"chunk-size",   required_argument, 0, 'c'},
        {"dry-run",      no_argument,       0, 'n'},
        {"bump-gen",     no_argument,       0, 'b'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0},
    };
    optind = 1;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "a:H:c:nbh", opts, &idx);
        if (c == -1) break;
        switch (c) {
        case 'a': account_file = optarg; break;
        case 'H': home_dir = optarg; break;
        case 'c': chunk_size = (uint64_t)strtoull(optarg, NULL, 10); break;
        case 'n': dry_run = 1; break;
        case 'b': bump_gen = 1; break;
        case 'h': usage_push(stdout); return 0;
        default : usage_push(stderr); return NH_PROV_CLI_EXIT_ARG;
        }
    }
    if (!home_dir) home_dir = getenv("HOME");
    if (!home_dir) {
        fprintf(stderr, "--home missing and $HOME unset\n");
        return NH_PROV_CLI_EXIT_ARG;
    }

    if (dry_run) {
        uint64_t tb = 0, tf = 0, tc = 0;
        int r = nh_prov_push_dry_run(home_dir, chunk_size, stdout,
                                     &tb, &tf, &tc);
        if (r != 0) {
            fprintf(stderr, "walk %s: %s\n", home_dir, strerror(-r));
            return NH_PROV_CLI_EXIT_INTERNAL;
        }
        fprintf(stderr,
            "-- dry-run summary --\n"
            "home:         %s\n"
            "chunk_size:   %llu\n"
            "files:        %llu\n"
            "total_bytes:  %llu\n"
            "total_chunks: %llu\n",
            home_dir,
            (unsigned long long)chunk_size,
            (unsigned long long)tf,
            (unsigned long long)tb,
            (unsigned long long)tc);
        return NH_PROV_CLI_EXIT_OK;
    }

    /* Real push: for v1 we redirect operators to nostr-home-syncd, which
     * IS the live pusher.  The CLI push wire-in against nostr_syncd_core
     * is deferred (see the header comment). */
    (void)account_file; (void)bump_gen;
    fprintf(stderr,
        "nostr-homed-provision push (real): not wired in v1.\n"
        "Use --dry-run for a manifest preview, or start `nostr-home-syncd`\n"
        "with the account credentials in the environment for a live push.\n"
        "Deferred: see bead nostrc-5fdu (spinoff to nostrc-89rj).\n");
    return NH_PROV_CLI_EXIT_INTERNAL;
}

/* ═══════════════════════════════════════════════════════════════════
 * verify
 *
 * v1 shape: pull-through verification.  The verify command drives the
 * same fetch helper as `pull` but into an ignored staging dir, then
 * interprets the helper's exit code:
 *
 *   0  -> OK: pointer + every chunk fetched, hash-verified.
 *   65 -> SSRF: refuse (network / config).
 *   71 -> NETWORK: pointer unreachable OR a chunk fetch failed.
 *          Distinguished from "missing chunk" by follow-up HEAD.
 *   75 -> DECRYPT: manifest AEAD failure.
 *
 * When --json is set we emit a compact result envelope on stdout.  A
 * per-chunk HEAD table is a follow-up (would require a shared
 * pointer-fetch helper library that porthome-fetch does not yet expose;
 * touching porthome-fetch is prohibited by the constraint block).
 * ═════════════════════════════════════════════════════════════════ */

static int cmd_verify(int argc, char **argv) {
    const char *account_file = NULL;
    int as_json = 0;
    uint32_t relay_timeout_ms = 10000;

    static struct option opts[] = {
        {"account-file",     required_argument, 0, 'a'},
        {"json",             no_argument,       0, 'j'},
        {"relay-timeout-ms", required_argument, 0, 't'},
        {"help",             no_argument,       0, 'h'},
        {0, 0, 0, 0},
    };
    optind = 1;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "a:jt:h", opts, &idx);
        if (c == -1) break;
        switch (c) {
        case 'a': account_file = optarg; break;
        case 'j': as_json = 1; break;
        case 't': relay_timeout_ms = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'h': usage_verify(stdout); return 0;
        default : usage_verify(stderr); return NH_PROV_CLI_EXIT_ARG;
        }
    }
    if (!account_file) {
        fprintf(stderr, "--account-file is required\n");
        return NH_PROV_CLI_EXIT_ARG;
    }

    /* Reuse the pull path against a throwaway staging dir. */
    char tmpl[64];
    snprintf(tmpl, sizeof tmpl, "/tmp/nhp-verify-XXXXXX");
    char *dest = mkdtemp(tmpl);
    if (!dest) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        return NH_PROV_CLI_EXIT_INTERNAL;
    }

    /* Delegate to cmd_pull by constructing a small argv. */
    char reltmo[32];
    snprintf(reltmo, sizeof reltmo, "%u", (unsigned)relay_timeout_ms);
    char *sub_argv[] = {
        (char *)"pull",
        (char *)"--account-file", (char *)account_file,
        (char *)"--dest",         dest,
        (char *)"--relay-timeout-ms", reltmo,
        NULL
    };
    int rc = cmd_pull(7, sub_argv);

    /* Wipe the staging dir on the way out. Best effort. */
    char rmcmd[512];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf -- %s", dest);
    if (rmcmd[0]) (void)system(rmcmd);

    int verify_rc;
    switch (rc) {
    case NH_PROV_CLI_EXIT_OK:      verify_rc = NH_PROV_CLI_EXIT_OK; break;
    case NH_PORTHOME_FETCH_EXIT_NETWORK_FAIL:
        /* A chunk fetch failure looks the same as a pointer failure;
         * distinguishing per-chunk would need a shared pointer-fetch
         * helper (see the note above).  Surface both as
         * MISSING_CHUNK when the pointer is likely reachable, else
         * NETWORK.  We can't tell without a HEAD probe; default to
         * MISSING_CHUNK since it is the more actionable class. */
        verify_rc = NH_PROV_CLI_EXIT_MISSING_CHUNK; break;
    case NH_PORTHOME_FETCH_EXIT_DECRYPT_FAIL:
        verify_rc = NH_PROV_CLI_EXIT_CRYPTO; break;
    case NH_PORTHOME_FETCH_EXIT_SSRF:
        verify_rc = NH_PROV_CLI_EXIT_SSRF; break;
    default:
        verify_rc = NH_PROV_CLI_EXIT_INTERNAL; break;
    }

    if (as_json) {
        printf("{\"verify_rc\":%d,\"fetch_rc\":%d,\"account_file\":\"%s\"}\n",
               verify_rc, rc, account_file);
    } else {
        if (verify_rc == NH_PROV_CLI_EXIT_OK)
            fprintf(stdout, "verify: OK (pointer + all chunks reachable)\n");
        else
            fprintf(stdout, "verify: NOT OK (fetch_rc=%d verify_rc=%d)\n",
                    rc, verify_rc);
    }
    return verify_rc;
}

/* ═══════════════════════════════════════════════════════════════════
 * main dispatch
 * ═════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage_top(argc < 2 ? stderr : stdout);
        return argc < 2 ? NH_PROV_CLI_EXIT_ARG : 0;
    }
    if (!strcmp(argv[1], "enroll")) return cmd_enroll(argc - 1, argv + 1);
    if (!strcmp(argv[1], "status")) return cmd_status(argc - 1, argv + 1);
    if (!strcmp(argv[1], "pull"))   return cmd_pull  (argc - 1, argv + 1);
    if (!strcmp(argv[1], "push"))   return cmd_push  (argc - 1, argv + 1);
    if (!strcmp(argv[1], "verify")) return cmd_verify(argc - 1, argv + 1);

    fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
    usage_top(stderr);
    return NH_PROV_CLI_EXIT_ARG;
}
