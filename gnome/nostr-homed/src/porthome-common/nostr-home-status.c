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
#include <unistd.h>

static void usage(FILE *f) {
    fprintf(f,
        "Usage: nostr-home-status [--json] [--field <path>]\n"
        "                          [--quiet-hours-set HH:MM-HH:MM|off]\n"
        "                          [--path] [--help]\n");
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

int main(int argc, char **argv) {
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
