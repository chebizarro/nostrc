/*
 * nostr-home-status — operator CLI over porthome-status.json.
 *
 * SPDX-License-Identifier: MIT
 *
 * Subcommands:
 *   nostr-home-status                    pretty-print all keys
 *   nostr-home-status --json             raw JSON on stdout
 *   nostr-home-status --field syncd.state
 *                                        print a single scalar and exit
 *   nostr-home-status --quiet-hours-set HH:MM-HH:MM
 *                                        write to $XDG_CONFIG_HOME/
 *                                        nostr-homed/quiet-hours
 *   nostr-home-status --quiet-hours-set off
 *                                        remove the config file
 *   nostr-home-status --path             print the resolved status
 *                                        path
 *
 * Field syntax: dot-separated key path; the first segment is the
 * writer name (syncd / fuse / provisioner), the rest is a sub-key.
 * Nested lookup is best-effort — this is an operator convenience,
 * not a full JSONPath.
 *
 * Bead: nostrc-h10m.1.
 */

#define _GNU_SOURCE
#include "nh_porthome_notify.h"
#include "nh_porthome_status.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void usage(FILE *f) {
    fprintf(f,
        "Usage: nostr-home-status [--json] [--field <path>]\n"
        "                          [--quiet-hours-set HH:MM-HH:MM|off]\n"
        "                          [--path] [--help]\n"
        "       nostr-home-status quota [--json]\n"
        "                          [--set-override BYTES | --clear-override]\n"
        "                          [--reload] [--help]\n");
}

/* Extract a value by dot path from raw JSON. Very small — handles
 * top-level object → nested object → scalar. Returns 0 on hit.
 * The returned string is heap. */
static int lookup_dotted(const char *body, size_t body_len,
                         const char *dotted,
                         char **out) {
    /* Split at first '.' — one level of nesting is enough for the
     * defined schema (writer.subkey). */
    const char *dot = strchr(dotted, '.');
    char first[128];
    if (!dot) {
        snprintf(first, sizeof first, "%.127s", dotted);
        return nh_porthome_status_get_key(body, body_len, first, out);
    }
    size_t L = (size_t)(dot - dotted);
    if (L >= sizeof first) return -EINVAL;
    memcpy(first, dotted, L);
    first[L] = 0;
    char *sub = NULL;
    int rc = nh_porthome_status_get_key(body, body_len, first, &sub);
    if (rc != 0) return rc;
    /* Recurse on the remainder. */
    rc = lookup_dotted(sub, strlen(sub), dot + 1, out);
    free(sub);
    return rc;
}

/* Trim JSON scalar wrappers: "..." → ... ; other scalars pass through. */
static char *strip_quotes(char *s) {
    if (!s) return s;
    size_t L = strlen(s);
    if (L >= 2 && s[0] == '"' && s[L - 1] == '"') {
        s[L - 1] = 0;
        memmove(s, s + 1, L - 1);
    }
    return s;
}

/* Pretty-print a raw JSON blob to stdout with a small indent. Does
 * not re-order keys. Not a full formatter — enough for an operator
 * eyeballing state. */
static void pretty(const char *body) {
    int indent = 0;
    bool in_str = false;
    bool esc    = false;
    for (const char *p = body; *p; ++p) {
        char c = *p;
        if (in_str) {
            fputc(c, stdout);
            if (esc)          esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  in_str = false;
            continue;
        }
        switch (c) {
            case '{':
            case '[':
                fputc(c, stdout);
                ++indent;
                fputc('\n', stdout);
                for (int i = 0; i < indent; ++i) fputs("  ", stdout);
                break;
            case '}':
            case ']':
                fputc('\n', stdout);
                --indent;
                for (int i = 0; i < indent; ++i) fputs("  ", stdout);
                fputc(c, stdout);
                break;
            case ',':
                fputc(c, stdout);
                fputc('\n', stdout);
                for (int i = 0; i < indent; ++i) fputs("  ", stdout);
                break;
            case ':':
                fputs(": ", stdout);
                break;
            case '"':
                fputc(c, stdout);
                in_str = true;
                break;
            default:
                if (!isspace((unsigned char)c)) fputc(c, stdout);
                break;
        }
    }
    fputc('\n', stdout);
}

static int cmd_quiet_hours_set(const char *arg) {
    int s = -1, e = -1;
    if (nh_porthome_notify_parse_hhmm(arg, &s, &e) != 0) {
        fprintf(stderr, "invalid quiet-hours value: %s\n", arg);
        fprintf(stderr, "expected HH:MM-HH:MM or 'off'\n");
        return 2;
    }
    /* Resolve $XDG_CONFIG_HOME/nostr-homed/quiet-hours. */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char dir[512];
    char path[600];
    if (xdg && *xdg) {
        snprintf(dir,  sizeof dir,  "%s/nostr-homed", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            fprintf(stderr, "$HOME unset; cannot resolve config path\n");
            return 1;
        }
        snprintf(dir,  sizeof dir,  "%s/.config/nostr-homed", home);
    }
    snprintf(path, sizeof path, "%s/quiet-hours", dir);
    /* mkdir -p on dir. */
    for (char *p = dir + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
                fprintf(stderr, "mkdir %s: %s\n", dir, strerror(errno));
                return 1;
            }
            *p = '/';
        }
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (s < 0) {
        /* "off" — remove the file. */
        if (unlink(path) != 0 && errno != ENOENT) {
            fprintf(stderr, "unlink %s: %s\n", path, strerror(errno));
            return 1;
        }
        fprintf(stdout, "quiet-hours: cleared\n");
        return 0;
    }
    FILE *f = fopen(path, "we");
    if (!f) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 1;
    }
    fprintf(f, "%02d:%02d-%02d:%02d\n", s / 60, s % 60, e / 60, e % 60);
    /* Set mode via the still-open fd, then close. */
    if (fchmod(fileno(f), 0600) != 0) { /* best effort */ }
    fclose(f);
    fprintf(stdout, "quiet-hours: %02d:%02d-%02d:%02d\n",
            s / 60, s % 60, e / 60, e % 60);
    return 0;
}


/* ─────────────── `nostr-home-status quota` subcommand ─────────────── */

static char *quota_config_dir_(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char buf[512];
    if (xdg && *xdg) {
        snprintf(buf, sizeof buf, "%s/nostr-homed", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        snprintf(buf, sizeof buf, "%s/.config/nostr-homed", home);
    }
    return strdup(buf);
}

static int mkdirp_600_(const char *dir) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Read status → extract a scalar field under status.syncd.<key>.
 * Returns 0 on hit (heap-alloc'd), -ENOENT if absent. */
static int status_scalar_(const char *field, char **out) {
    char *path = nh_porthome_status_default_path();
    if (!path) return -ENOMEM;
    char *body = NULL; size_t body_len = 0;
    int rc = nh_porthome_status_read(path, &body, &body_len);
    free(path);
    if (rc != 0) return rc;
    rc = lookup_dotted(body, body_len, field, out);
    free(body);
    return rc;
}

static void print_quota_json(uint64_t bytes,
                             const char *source,
                             uint64_t used_bytes,
                             uint32_t pinned,
                             uint32_t evict_rate) {
    printf("{\"cache_quota_bytes\":%llu,"
           "\"cache_quota_source\":\"%s\","
           "\"cache_bytes\":%llu,"
           "\"pinned_gen_count\":%u,"
           "\"evict_rate_1h\":%u}\n",
           (unsigned long long)bytes,
           source ? source : "default",
           (unsigned long long)used_bytes,
           (unsigned)pinned,
           (unsigned)evict_rate);
}

static void print_quota_pretty(uint64_t bytes,
                               const char *source,
                               uint64_t used_bytes,
                               uint32_t pinned,
                               uint32_t evict_rate) {
    printf("cache quota:     %llu bytes (source=%s)\n",
           (unsigned long long)bytes, source ? source : "default");
    printf("cache used:      %llu bytes\n", (unsigned long long)used_bytes);
    printf("pinned gens:     %u\n", (unsigned)pinned);
    printf("evict rate/1h:   %u\n", (unsigned)evict_rate);
}

/* Best-effort SIGHUP delivery to the user-scope sync service. Returns
 * 0 if the systemctl reload OR the pkill fallback exited 0; nonzero
 * when both delivery paths failed so callers can surface a hint. */
static int reload_syncd_(void) {
    /* Prefer systemctl --user reload if available. Fall back to sending
     * SIGHUP to any nostr-home-syncd process owned by this uid. */
    int rc = system("systemctl --user reload nostr-home-sync.service"
                    " >/dev/null 2>&1");
    if (rc == 0) return 0;
    /* nostrc-cvqe: surface pkill fallback failure — check system() rc
     * and return nonzero so the CLI exit code reflects it. */
    int prc = system("pkill -HUP -u $(id -u) -x nostr-home-syncd"
                     " >/dev/null 2>&1");
    if (prc == -1 || !WIFEXITED(prc) || WEXITSTATUS(prc) != 0) {
        fprintf(stderr,
                "warning: could not signal nostr-home-syncd "
                "(systemctl reload rc=%d, pkill rc=%d); the new value "
                "will apply on the next syncd restart\n", rc, prc);
        return 1;
    }
    return 0;
}

static int cmd_quota(int argc, char **argv) {
    bool as_json = false;
    bool set_override = false;
    bool clear_override = false;
    bool do_reload = false;
    const char *set_val = NULL;

    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--json"))      { as_json = true;         continue; }
        if (!strcmp(a, "--reload"))    { do_reload = true;       continue; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(a, "--set-override") && i + 1 < argc) {
            set_override = true; set_val = argv[++i]; continue;
        }
        if (!strcmp(a, "--clear-override")) { clear_override = true; continue; }
        fprintf(stderr, "unknown argument: %s\n", a);
        usage(stderr);
        return 2;
    }
    if (set_override && clear_override) {
        fprintf(stderr, "--set-override and --clear-override are mutually exclusive\n");
        return 2;
    }

    /* Mutations. */
    if (set_override) {
        char *end = NULL;
        unsigned long long v = strtoull(set_val, &end, 10);
        if (!end || *end != '\0' || v == 0) {
            fprintf(stderr, "invalid --set-override value: %s (want a decimal integer of bytes)\n",
                    set_val);
            return 2;
        }
        char *dir = quota_config_dir_();
        if (!dir) { fprintf(stderr, "$HOME unset; cannot resolve config path\n"); return 1; }
        if (mkdirp_600_(dir) != 0) { fprintf(stderr, "mkdir %s: %s\n", dir, strerror(errno)); free(dir); return 1; }
        char path[600];
        snprintf(path, sizeof path, "%s/cache-quota", dir);
        free(dir);
        FILE *f = fopen(path, "we");
        if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }
        fprintf(f, "%llu\n", v);
        (void)fchmod(fileno(f), 0600);
        fclose(f);
        printf("quota override: %llu bytes -> %s\n", v, path);
        printf("note: syncd will pick up the new value on next SIGHUP\n");
        if (do_reload && reload_syncd_() != 0) return 1;
        return 0;
    }
    if (clear_override) {
        char *dir = quota_config_dir_();
        if (!dir) { fprintf(stderr, "$HOME unset; cannot resolve config path\n"); return 1; }
        char path[600];
        snprintf(path, sizeof path, "%s/cache-quota", dir);
        free(dir);
        if (unlink(path) != 0 && errno != ENOENT) {
            fprintf(stderr, "unlink %s: %s\n", path, strerror(errno));
            return 1;
        }
        printf("quota override: cleared\n");
        printf("note: syncd will pick up the change on next SIGHUP\n");
        if (do_reload && reload_syncd_() != 0) return 1;
        return 0;
    }

    /* Read-side: pull from status.syncd. */
    char *q_bytes  = NULL, *q_source = NULL, *c_bytes = NULL,
         *pinned  = NULL, *evictr  = NULL;
    (void)status_scalar_("syncd.cache_quota_bytes",  &q_bytes);
    (void)status_scalar_("syncd.cache_quota_source", &q_source);
    (void)status_scalar_("syncd.cache_bytes",        &c_bytes);
    (void)status_scalar_("syncd.pinned_gen_count",   &pinned);
    (void)status_scalar_("syncd.evict_rate_1h",      &evictr);

    uint64_t qb = q_bytes ? strtoull(strip_quotes(q_bytes), NULL, 10) : 0ull;
    uint64_t cb = c_bytes ? strtoull(strip_quotes(c_bytes), NULL, 10) : 0ull;
    uint32_t pn = pinned  ? (uint32_t)strtoul(strip_quotes(pinned), NULL, 10) : 0u;
    uint32_t er = evictr  ? (uint32_t)strtoul(strip_quotes(evictr), NULL, 10) : 0u;
    const char *src = q_source ? strip_quotes(q_source) : "unknown";

    if (as_json) print_quota_json(qb, src, cb, pn, er);
    else          print_quota_pretty(qb, src, cb, pn, er);

    free(q_bytes); free(q_source); free(c_bytes); free(pinned); free(evictr);
    return 0;
}

int main(int argc, char **argv) {
    /* Subcommand dispatch. `quota` gets a dedicated parser
     * so its argv can accept flag pairs like --set-override 42.
     * Everything else routes through the flat --field/--json path. */
    if (argc >= 2 && !strcmp(argv[1], "quota")) {
        return cmd_quota(argc - 2, argv + 2);
    }
    bool as_json = false;
    bool show_path = false;
    const char *field = NULL;
    const char *quiet_set = NULL;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--json"))      { as_json = true;  continue; }
        if (!strcmp(a, "--path"))      { show_path = true; continue; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(a, "--field") && i + 1 < argc) {
            field = argv[++i];
            continue;
        }
        if (!strcmp(a, "--quiet-hours-set") && i + 1 < argc) {
            quiet_set = argv[++i];
            continue;
        }
        fprintf(stderr, "unknown argument: %s\n", a);
        usage(stderr);
        return 2;
    }

    if (quiet_set) return cmd_quiet_hours_set(quiet_set);

    char *path = nh_porthome_status_default_path();
    if (!path) {
        fprintf(stderr, "cannot resolve status path (HOME unset?)\n");
        return 1;
    }
    if (show_path) {
        printf("%s\n", path);
        free(path);
        return 0;
    }
    char *body = NULL; size_t body_len = 0;
    int rc = nh_porthome_status_read(path, &body, &body_len);
    if (rc != 0) {
        fprintf(stderr, "read %s: %s\n", path, strerror(-rc));
        free(path);
        return 1;
    }
    free(path);

    if (field) {
        char *out = NULL;
        rc = lookup_dotted(body, body_len, field, &out);
        free(body);
        if (rc == -ENOENT) {
            /* Missing keys exit non-zero but silent (script friendly). */
            return 3;
        }
        if (rc != 0) {
            fprintf(stderr, "lookup: %s\n", strerror(-rc));
            return 1;
        }
        printf("%s\n", strip_quotes(out));
        free(out);
        return 0;
    }

    if (as_json) {
        fwrite(body, 1, body_len, stdout);
        if (body_len == 0 || body[body_len - 1] != '\n') fputc('\n', stdout);
    } else {
        pretty(body);
    }
    free(body);
    return 0;
}
