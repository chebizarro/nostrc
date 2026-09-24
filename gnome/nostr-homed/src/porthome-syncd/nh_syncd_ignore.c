/*
 * nh_syncd_ignore.c — ignore-set matcher per design §6.2.
 *
 * SPDX-License-Identifier: MIT
 *
 * Order of checks (fail-closed):
 *   1. Explicit control-path/state-dir/cache-dir prefixes (fastest to
 *      match; unignorable).
 *   2. xdev — if st_dev != home_dev the entry is on a foreign
 *      filesystem and we refuse to capture (avoids bind mounts, nfs
 *      mounts, /run tmpfs mounted under $HOME, etc.).
 *   3. Special file (socket/FIFO/device/blockdev) — never captured.
 *   4. Static glob set (`~/.cache`, `Trash`, firefox lock, `*.tmp`,
 *      trailing-`~` backups).
 *   5. User globs from ~/.config/nostr-homed/ignore.
 *
 * Paths handed to us are always $HOME-relative, without a leading '/'.
 * A directory match implicitly covers every descendant, and every
 * static rule is anchored (either a bare basename or a slash-form).
 */

#include "nh_syncd.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Never-overridable prefixes; these are policy, not opinion. */
static const char *const STATIC_PREFIX[] = {
    ".cache",
    ".cache/nostr-homed",         /* NH_SYNCD_LOCAL_CACHE_REL, doubled for clarity */
    ".local/share/Trash",
    ".local/state/nostr-homed",   /* our own state dir */
    ".nostr-home-limited",        /* control file */
    NULL,
};

/* Extra static globs (matched with FNM_PATHNAME so `*.tmp` in
 * subdir/foo.tmp still matches — fnmatch(FNM_PATHNAME) makes '*'
 * stop at '/', so we register the pattern both bare and with a
 * `**` prefix — we implement `**` as a repeat-across-slashes
 * traversal in match_glob below). */
static const char *const STATIC_GLOB[] = {
    ".mozilla/firefox/*/lock",
    "**/*.tmp",
    "**/*~",                      /* editor backup files */
    NULL,
};

struct nh_syncd_ignore {
    /* User globs (owned). */
    char   **user_globs;
    size_t   user_globs_n;
    /* Home's st_dev for xdev; 0 => unknown / skip xdev. */
    uint64_t home_dev;
};

static int add_user_glob(nh_syncd_ignore *ig, const char *g) {
    if (!g || !*g) return NH_SYNCD_OK;
    size_t n = ig->user_globs_n;
    char **grown = realloc(ig->user_globs, (n + 1) * sizeof(char *));
    if (!grown) return NH_SYNCD_ERR_OOM;
    ig->user_globs = grown;
    ig->user_globs[n] = strdup(g);
    if (!ig->user_globs[n]) return NH_SYNCD_ERR_OOM;
    ig->user_globs_n = n + 1;
    return NH_SYNCD_OK;
}

int nh_syncd_ignore_add_user_glob(nh_syncd_ignore *ig, const char *glob) {
    if (!ig) return NH_SYNCD_ERR_ARG;
    return add_user_glob(ig, glob);
}
void nh_syncd_ignore_set_home_dev(nh_syncd_ignore *ig, uint64_t dev) {
    if (ig) ig->home_dev = dev;
}

static void free_user_globs(nh_syncd_ignore *ig) {
    for (size_t i = 0; i < ig->user_globs_n; i++) free(ig->user_globs[i]);
    free(ig->user_globs);
    ig->user_globs = NULL;
    ig->user_globs_n = 0;
}

static char *config_ignore_path(const char *home_dir) {
    /* Respects XDG_CONFIG_HOME if set to any non-empty absolute path. */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') {
        size_t n = strlen(xdg) + strlen("/nostr-homed/ignore") + 1;
        char *p = malloc(n);
        if (!p) return NULL;
        snprintf(p, n, "%s/nostr-homed/ignore", xdg);
        return p;
    }
    size_t n = strlen(home_dir) + strlen("/.config/nostr-homed/ignore") + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s/.config/nostr-homed/ignore", home_dir);
    return p;
}

static int load_user_file(nh_syncd_ignore *ig, const char *home_dir) {
    char *path = config_ignore_path(home_dir);
    if (!path) return NH_SYNCD_ERR_OOM;
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return errno == ENOENT ? NH_SYNCD_OK : NH_SYNCD_ERR_IO;
    char *line = NULL; size_t cap = 0;
    ssize_t n;
    int rc = NH_SYNCD_OK;
    while ((n = getline(&line, &cap, f)) != -1) {
        char *p = line;
        /* Strip trailing newline / carriage return. */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) { line[--n] = '\0'; }
        /* Strip leading whitespace. */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        rc = add_user_glob(ig, p);
        if (rc != NH_SYNCD_OK) break;
    }
    free(line);
    fclose(f);
    return rc;
}

int nh_syncd_ignore_new(const char *home_dir, nh_syncd_ignore **out) {
    if (!home_dir || !out) return NH_SYNCD_ERR_ARG;
    nh_syncd_ignore *ig = calloc(1, sizeof *ig);
    if (!ig) return NH_SYNCD_ERR_OOM;
    /* Try to record the home device for xdev checks. Failure to stat
     * $HOME leaves home_dev == 0 which disables the xdev branch — a
     * safe fallback that still applies the other rules. */
    struct stat st;
    if (stat(home_dir, &st) == 0) ig->home_dev = (uint64_t)st.st_dev;
    int rc = load_user_file(ig, home_dir);
    if (rc != NH_SYNCD_OK) { nh_syncd_ignore_free(ig); return rc; }
    *out = ig;
    return NH_SYNCD_OK;
}

int nh_syncd_ignore_reload(nh_syncd_ignore *ig, const char *home_dir) {
    if (!ig || !home_dir) return NH_SYNCD_ERR_ARG;
    free_user_globs(ig);
    return load_user_file(ig, home_dir);
}

void nh_syncd_ignore_free(nh_syncd_ignore *ig) {
    if (!ig) return;
    free_user_globs(ig);
    free(ig);
}

/* Match `path` (already $HOME-relative) against a glob. The double
 * star ("globstar") is treated as "any run of characters including
 * the slash, including the empty run before the following slash":
 * so "globstar-slash-star-dot-tmp" matches both a bare "foo.tmp"
 * and a nested "dir/sub/foo.tmp". Implementation:
 *   - Strip a leading globstar-slash and try the tail against the
 *     path with FNM_PATHNAME cleared (so '*' crosses slashes).
 *   - Otherwise rewrite every doublestar as a single star and match
 *     the same way.
 * (Comment avoids literal star-slash sequences that would close
 * this block comment early.) */
static int match_glob(const char *pattern, const char *path) {
    if (strstr(pattern, "**") == NULL)
        return fnmatch(pattern, path, FNM_PATHNAME) == 0;

    /* Handle the common leading-globstar idiom. */
    if (strncmp(pattern, "**/", 3) == 0) {
        const char *tail = pattern + 3;
        /* Match at the root (bare basename). */
        if (fnmatch(tail, path, 0) == 0) return 1;
        /* Match at any depth: fall through to the generic
         * rewrite-and-match path below. */
    }

    /* Replace every `**` with `*` and match with FNM_PATHNAME off
     * so `*` crosses directory boundaries. */
    size_t n = strlen(pattern);
    char *rewritten = malloc(n + 1);
    if (!rewritten) return 0;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (i + 1 < n && pattern[i] == '*' && pattern[i + 1] == '*') {
            rewritten[j++] = '*';
            i++;
            continue;
        }
        rewritten[j++] = pattern[i];
    }
    rewritten[j] = '\0';
    int r = fnmatch(rewritten, path, 0);
    free(rewritten);
    return r == 0;
}

/* Test whether `rel` equals `prefix` or begins with `prefix/`. */
static int has_prefix_component(const char *rel, const char *prefix) {
    size_t pn = strlen(prefix);
    if (strncmp(rel, prefix, pn) != 0) return 0;
    if (rel[pn] == '\0' || rel[pn] == '/') return 1;
    return 0;
}

nh_syncd_ignore_kind nh_syncd_ignore_check_path(const nh_syncd_ignore *ig,
                                                const char *rel_path)
{
    if (!ig || !rel_path) return NH_SYNCD_IGNORE_PASS;
    if (rel_path[0] == '/' || rel_path[0] == '\0')
        return NH_SYNCD_IGNORE_PASS;

    for (int i = 0; STATIC_PREFIX[i]; i++)
        if (has_prefix_component(rel_path, STATIC_PREFIX[i]))
            return NH_SYNCD_IGNORE_CONTROL;

    for (int i = 0; STATIC_GLOB[i]; i++)
        if (match_glob(STATIC_GLOB[i], rel_path))
            return NH_SYNCD_IGNORE_STATIC;

    for (size_t i = 0; i < ig->user_globs_n; i++)
        if (match_glob(ig->user_globs[i], rel_path))
            return NH_SYNCD_IGNORE_USER;

    return NH_SYNCD_IGNORE_PASS;
}

nh_syncd_ignore_kind nh_syncd_ignore_check(const nh_syncd_ignore *ig,
                                           const char *rel_path,
                                           uint64_t st_dev,
                                           uint32_t st_mode)
{
    if (!ig || !rel_path) return NH_SYNCD_IGNORE_PASS;

    /* Control-file/state-dir prefixes first — the state dir must never
     * be captured no matter what stat says. */
    if (rel_path[0] == '/' || rel_path[0] == '\0')
        return NH_SYNCD_IGNORE_PASS;
    for (int i = 0; STATIC_PREFIX[i]; i++)
        if (has_prefix_component(rel_path, STATIC_PREFIX[i]))
            return NH_SYNCD_IGNORE_CONTROL;

    /* xdev. */
    if (ig->home_dev != 0 && st_dev != 0 && st_dev != ig->home_dev)
        return NH_SYNCD_IGNORE_XDEV;

    /* Special files (sockets/FIFOs/char/block devices). Regular files,
     * dirs, symlinks pass this gate. */
    if (st_mode != 0 && !S_ISREG(st_mode) && !S_ISDIR(st_mode) && !S_ISLNK(st_mode))
        return NH_SYNCD_IGNORE_SPECIAL;

    for (int i = 0; STATIC_GLOB[i]; i++)
        if (match_glob(STATIC_GLOB[i], rel_path))
            return NH_SYNCD_IGNORE_STATIC;

    for (size_t i = 0; i < ig->user_globs_n; i++)
        if (match_glob(ig->user_globs[i], rel_path))
            return NH_SYNCD_IGNORE_USER;

    return NH_SYNCD_IGNORE_PASS;
}
