/* SPDX-License-Identifier: MIT
 *
 * nh_syncd_conflicts.c — see nh_syncd_conflicts.h (nostrc-h10m.6).
 */
#define _GNU_SOURCE
#include "nh_syncd_conflicts.h"

#include "nh_porthome_status.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define JOURNAL_NAME       "home-conflicts.json"
#define JOURNAL_MAX_ROWS   4096u
#define REQUEST_MAX_BYTES  4096
#define TOKEN_ABSENT       "absent"

/* ───────────────────────── shared helpers ───────────────────────── */

static const char *const choice_slugs[] = {NULL, "keep-current", "use-saved", "keep-both"};

const char *nh_conflict_choice_slug(nh_conflict_choice c) {
    return (c >= NH_CONFLICT_KEEP_CURRENT && c <= NH_CONFLICT_KEEP_BOTH) ? choice_slugs[c] : "";
}

int nh_conflict_choice_parse(const char *slug, nh_conflict_choice *out) {
    for (int c = NH_CONFLICT_KEEP_CURRENT; slug && c <= NH_CONFLICT_KEEP_BOTH; ++c)
        if (!strcmp(slug, choice_slugs[c])) {
            *out = (nh_conflict_choice)c;
            return 0;
        }
    return -1;
}

static int is_hex(const char *s, size_t n) {
    if (!s || strlen(s) != n) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

static int random_hex(char *out, size_t bytes) {
    unsigned char raw[32];
    if (bytes > sizeof raw) return -1;
    size_t got = 0;
    while (got < bytes) {
        ssize_t n = getrandom(raw + got, bytes - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 15];
    }
    out[2 * bytes] = '\0';
    return 0;
}

static int safe_rel(const char *rel) {
    if (!rel || !*rel || *rel == '/' || strlen(rel) >= PATH_MAX) return 0;
    for (const char *p = rel; *p;) {
        size_t n = strcspn(p, "/");
        if (n == 0 || n > NAME_MAX || (n == 1 && p[0] == '.') || (n == 2 && !memcmp(p, "..", 2)))
            return 0;
        p += n;
        if (*p == '/') ++p;
    }
    return rel[strlen(rel) - 1] != '/';
}

/* Open the parent directory of `rel` beneath home_fd without following
 * symlinks. *leaf points into rel. Returns an fd or -1 (errno set). */
static int open_parent(int home_fd, const char *rel, const char **leaf) {
    if (!safe_rel(rel)) {
        errno = EINVAL;
        return -1;
    }
    const char *slash = strrchr(rel, '/');
    *leaf = slash ? slash + 1 : rel;
    int cur = fcntl(home_fd, F_DUPFD_CLOEXEC, 0);
    const char *p = rel;
    while (cur >= 0 && slash && p < slash) {
        size_t n = strcspn(p, "/");
        char name[NAME_MAX + 1];
        memcpy(name, p, n);
        name[n] = '\0';
        int next = openat(cur, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close(cur);
        cur = next;
        errno = saved;
        p += n + 1;
    }
    return cur;
}

static int lstat_rel(int home_fd, const char *rel, struct stat *st) {
    const char *leaf;
    int dir = open_parent(home_fd, rel, &leaf);
    if (dir < 0) return -1;
    int rc = fstatat(dir, leaf, st, AT_SYMLINK_NOFOLLOW);
    int saved = errno;
    close(dir);
    errno = saved;
    return rc;
}

/* Unlink a non-directory. Missing is success. */
static int unlink_rel(int home_fd, const char *rel) {
    const char *leaf;
    int dir = open_parent(home_fd, rel, &leaf);
    if (dir < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    int rc = 0;
    if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISDIR(st.st_mode)) {
            errno = EISDIR;
            rc = -1;
        } else if (unlinkat(dir, leaf, 0) != 0 || fsync(dir) != 0) {
            rc = -1;
        }
    } else if (errno != ENOENT) {
        rc = -1;
    }
    int saved = errno;
    close(dir);
    errno = saved;
    return rc;
}

/* Rename within HOME. `noreplace` refuses an existing destination. */
static int rename_rel(int home_fd, const char *from, const char *to, int noreplace) {
    const char *from_leaf, *to_leaf;
    int a = open_parent(home_fd, from, &from_leaf);
    if (a < 0) return -1;
    int b = open_parent(home_fd, to, &to_leaf);
    if (b < 0) {
        int saved = errno;
        close(a);
        errno = saved;
        return -1;
    }
    int rc = renameat2(a, from_leaf, b, to_leaf, noreplace ? RENAME_NOREPLACE : 0);
    if (rc == 0 && (fsync(a) != 0 || fsync(b) != 0)) rc = -1;
    int saved = errno;
    close(a);
    close(b);
    errno = saved;
    return rc;
}

void nh_syncd_state_version_token(const nh_syncd_state *s, const char *path, char out[65]) {
    const nh_syncd_entry *e = s && path ? nh_syncd_state_find(s, path) : NULL;
    if (!e) {
        snprintf(out, 65, "%s", TOKEN_ABSENT);
        return;
    }
    /* Content identity only: kind, size, hash, chunk addresses and link
     * target. mtime is excluded so a re-capture of identical bytes does
     * not invalidate a request the user is about to send. */
    char head[96];
    snprintf(head, sizeof head, "v1|%d|%llu|", (int)nh_syncd_entry_kind(e),
             (unsigned long long)nh_syncd_entry_size(e));
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    unsigned char hash[32];
    unsigned len = 0;
    int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(md, head, strlen(head)) == 1;
    const char *h = nh_syncd_entry_content_hash_hex(e);
    if (ok && h) ok = EVP_DigestUpdate(md, h, strlen(h) + 1) == 1;
    for (size_t i = 0; ok && i < nh_syncd_entry_chunk_count(e); ++i) {
        const char *c = nh_syncd_entry_chunk_at(e, i);
        if (c) ok = EVP_DigestUpdate(md, c, strlen(c) + 1) == 1;
    }
    const char *t = nh_syncd_entry_symlink_target(e);
    if (ok && t) ok = EVP_DigestUpdate(md, t, strlen(t) + 1) == 1;
    if (ok) ok = EVP_DigestFinal_ex(md, hash, &len) == 1 && len == 32;
    EVP_MD_CTX_free(md);
    if (!ok) {
        snprintf(out, 65, "%s", "unknown");
        return;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = hex[hash[i] >> 4];
        out[2 * i + 1] = hex[hash[i] & 15];
    }
    out[64] = '\0';
}

int nh_conflict_sibling_path(int home_fd, const nh_syncd_state *s, const char *path,
                             const char *label, char **out) {
    *out = NULL;
    if (!safe_rel(path) || !label) return NH_SYNCD_ERR_ARG;
    const char *slash = strrchr(path, '/');
    size_t dir_len = slash ? (size_t)(slash - path) + 1 : 0;
    const char *leaf = path + dir_len;
    const char *dot = strrchr(leaf, '.');
    /* A leading dot is a hidden name, not an extension; long tails are
     * not extensions either ("archive.tar" yes, "notes.from-tuesday" no). */
    if (!dot || dot == leaf || strlen(dot) > 10) dot = leaf + strlen(leaf);
    size_t stem_len = (size_t)(dot - leaf);
    for (unsigned n = 1; n < 100; ++n) {
        char suffix[96];
        if (n == 1)
            snprintf(suffix, sizeof suffix, " (conflicted copy %s)", label);
        else
            snprintf(suffix, sizeof suffix, " (conflicted copy %s %u)", label, n);
        size_t keep = stem_len;
        if (keep + strlen(suffix) + strlen(dot) > NAME_MAX) {
            if (strlen(suffix) + strlen(dot) >= NAME_MAX) return NH_SYNCD_ERR_PATH;
            keep = NAME_MAX - strlen(suffix) - strlen(dot);
        }
        char *candidate = NULL;
        if (asprintf(&candidate, "%.*s%.*s%s%s", (int)dir_len, path, (int)keep, leaf, suffix,
                     dot) < 0)
            return NH_SYNCD_ERR_OOM;
        struct stat st;
        int taken = (s && nh_syncd_state_find(s, candidate)) ||
                    lstat_rel(home_fd, candidate, &st) == 0 || errno != ENOENT;
        if (!taken) {
            *out = candidate;
            return 0;
        }
        free(candidate);
    }
    return NH_SYNCD_ERR_PATH;
}

/* ─────────────────────── personal-home journal ─────────────────────── */

static char *journal_path(const char *state_dir) {
    char *p = NULL;
    return asprintf(&p, "%s/%s", state_dir, JOURNAL_NAME) < 0 ? NULL : p;
}

static json_t *journal_load(const char *state_dir, int *err) {
    *err = 0;
    char *path = journal_path(state_dir);
    if (!path) {
        *err = NH_SYNCD_ERR_OOM;
        return NULL;
    }
    json_t *doc = NULL;
    struct stat st;
    if (stat(path, &st) != 0 && errno == ENOENT) {
        doc = json_pack("{s:i,s:[]}", "version", 1, "conflicts");
    } else {
        doc = json_load_file(path, JSON_REJECT_DUPLICATES, NULL);
        if (!json_is_object(doc) || json_integer_value(json_object_get(doc, "version")) != 1 ||
            !json_is_array(json_object_get(doc, "conflicts"))) {
            json_decref(doc);
            doc = NULL;
            *err = NH_SYNCD_ERR_JSON;
        }
    }
    free(path);
    if (!doc && !*err) *err = NH_SYNCD_ERR_OOM;
    return doc;
}

static int journal_save(const char *state_dir, json_t *doc) {
    char *path = journal_path(state_dir), *tmp = NULL;
    if (!path || asprintf(&tmp, "%s.tmp.%ld", path, (long)getpid()) < 0) {
        free(path);
        return NH_SYNCD_ERR_OOM;
    }
    int rc = NH_SYNCD_ERR_IO;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        if (json_dumpfd(doc, fd, JSON_COMPACT) == 0 && fsync(fd) == 0) rc = 0;
        close(fd);
    }
    if (rc == 0 && rename(tmp, path) != 0) rc = NH_SYNCD_ERR_IO;
    if (rc != 0) unlink(tmp);
    if (rc == 0) {
        int dfd = open(state_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            (void)fsync(dfd);
            close(dfd);
        }
    }
    free(tmp);
    free(path);
    return rc;
}

static const char *kind_slug(nh_home_conflict_kind k) {
    switch (k) {
    case NH_HOME_CONFLICT_EDIT: return "edit";
    case NH_HOME_CONFLICT_DELETED_REMOTELY: return "deleted-remotely";
    case NH_HOME_CONFLICT_DELETED_LOCALLY: return "deleted-locally";
    }
    return "";
}

static nh_home_conflict_kind kind_parse(const char *s) {
    if (s && !strcmp(s, "edit")) return NH_HOME_CONFLICT_EDIT;
    if (s && !strcmp(s, "deleted-remotely")) return NH_HOME_CONFLICT_DELETED_REMOTELY;
    if (s && !strcmp(s, "deleted-locally")) return NH_HOME_CONFLICT_DELETED_LOCALLY;
    return 0;
}

static const char *resolution_state(const json_t *row) {
    return json_string_value(json_object_get(json_object_get(row, "resolution"), "state"));
}

/* Drop the oldest resolved rows above the cap; unresolved rows are never
 * discarded (their files are still in HOME). */
static void journal_prune(json_t *rows) {
    for (size_t i = 0; json_array_size(rows) > JOURNAL_MAX_ROWS && i < json_array_size(rows);) {
        const char *st = resolution_state(json_array_get(rows, i));
        if (st && !strcmp(st, "resolved"))
            json_array_remove(rows, i);
        else
            ++i;
    }
}

int nh_syncd_home_conflicts_record(const char *state_dir, const nh_home_conflict_record *rows,
                                   size_t n, uint64_t generation, int64_t now) {
    if (!state_dir || (n && !rows)) return NH_SYNCD_ERR_ARG;
    if (n == 0) return NH_SYNCD_OK;
    int err = 0;
    json_t *doc = journal_load(state_dir, &err);
    if (!doc) return err;
    json_t *list = json_object_get(doc, "conflicts");
    int rc = NH_SYNCD_OK;
    for (size_t i = 0; rc == NH_SYNCD_OK && i < n; ++i) {
        const nh_home_conflict_record *r = &rows[i];
        char id[33];
        if (!safe_rel(r->path) || !safe_rel(r->saved) || !kind_parse(kind_slug(r->kind))) {
            rc = NH_SYNCD_ERR_ARG;
            break;
        }
        if (random_hex(id, 16) != 0) {
            rc = NH_SYNCD_ERR_IO;
            break;
        }
        json_t *row = json_pack("{s:s,s:s,s:s,s:s,s:I,s:I}", "id", id, "path", r->path, "kind",
                                kind_slug(r->kind), "saved", r->saved, "generation",
                                (json_int_t)generation, "detected_at", (json_int_t)now);
        if (row && r->kind == NH_HOME_CONFLICT_EDIT &&
            json_object_set_new(row, "saved_side", json_string(r->saved_is_local ? "local" : "remote"))) {
            json_decref(row);
            row = NULL;
        }
        if (!row || json_array_append_new(list, row)) rc = NH_SYNCD_ERR_OOM;
    }
    if (rc == NH_SYNCD_OK) {
        journal_prune(list);
        rc = journal_save(state_dir, doc);
    }
    json_decref(doc);
    return rc;
}

/* Which side holds this device's version, for the CLI's keep-mine /
 * take-theirs aliases: "current", "saved" or NULL when unknown. */
static const char *home_mine(const json_t *row) {
    switch (kind_parse(json_string_value(json_object_get(row, "kind")))) {
    case NH_HOME_CONFLICT_EDIT: {
        const char *side = json_string_value(json_object_get(row, "saved_side"));
        if (side && !strcmp(side, "local")) return "saved";
        if (side && !strcmp(side, "remote")) return "current";
        return NULL;
    }
    case NH_HOME_CONFLICT_DELETED_REMOTELY: return "current"; /* local edit kept   */
    case NH_HOME_CONFLICT_DELETED_LOCALLY: return "saved";    /* local deletion lost */
    }
    return NULL;
}

int nh_syncd_home_conflicts_project(const char *state_dir, int home_fd, const nh_syncd_state *s,
                                    char **out) {
    *out = NULL;
    if (!state_dir) return NH_SYNCD_ERR_ARG;
    int err = 0;
    json_t *doc = journal_load(state_dir, &err);
    if (!doc) return err;
    json_t *array = json_array();
    size_t i;
    json_t *row;
    int rc = array ? NH_SYNCD_OK : NH_SYNCD_ERR_OOM;
    json_array_foreach(json_object_get(doc, "conflicts"), i, row) {
        if (rc) break;
        const char *st = resolution_state(row);
        if (st && !strcmp(st, "resolved")) continue;
        const char *path = json_string_value(json_object_get(row, "path"));
        const char *saved = json_string_value(json_object_get(row, "saved"));
        const char *id = json_string_value(json_object_get(row, "id"));
        nh_home_conflict_kind kind = kind_parse(json_string_value(json_object_get(row, "kind")));
        if (!path || !saved || !id || !kind) continue; /* malformed: not resolvable */
        char token[65];
        nh_syncd_state_version_token(s, path, token);
        struct stat cur;
        bool current = home_fd >= 0 ? lstat_rel(home_fd, path, &cur) == 0
                                    : strcmp(token, TOKEN_ABSENT) != 0;
        const char *mine = home_mine(row);
        json_t *choices = kind == NH_HOME_CONFLICT_EDIT
                              ? json_pack("[s,s,s]", "keep-current", "use-saved", "keep-both")
                              : json_pack("[s,s]", "keep-current", "use-saved");
        json_t *gen = json_object_get(row, "generation");
        json_t *at = json_object_get(row, "detected_at");
        json_t *p = json_pack("{s:s,s:s,s:s,s:s,s:b,s:b,s:s,s:o,s:s,s:O,s:O}", "source", "home",
                              "id", id, "path", path, "kind", kind_slug(kind), "present",
                              kind == NH_HOME_CONFLICT_EDIT, "current_present", current,
                              "state", st ? "pending" : "active",
                              "choices", choices, "expect", token, "generation",
                              gen ? gen : json_null(), "detected_at", at ? at : json_null());
        if (p && json_object_set_new(p, kind == NH_HOME_CONFLICT_EDIT ? "destination" : "marker",
                                     json_string(saved)))
            rc = NH_SYNCD_ERR_OOM;
        if (p && !rc && json_object_set_new(p, "mine", mine ? json_string(mine) : json_null()))
            rc = NH_SYNCD_ERR_OOM;
        if (!p || rc || json_array_append_new(array, p)) rc = NH_SYNCD_ERR_OOM;
    }
    if (!rc) {
        *out = json_dumps(array, JSON_COMPACT);
        if (!*out) rc = NH_SYNCD_ERR_OOM;
    }
    json_decref(array);
    json_decref(doc);
    return rc;
}

static json_t *find_row(json_t *doc, const char *id) {
    size_t i;
    json_t *row;
    json_array_foreach(json_object_get(doc, "conflicts"), i, row) {
        const char *rid = json_string_value(json_object_get(row, "id"));
        if (rid && !strcmp(rid, id)) return row;
    }
    return NULL;
}

static int touch(json_t *touched, const nh_syncd_state *s, const char *path, const char *kind) {
    char token[65];
    nh_syncd_state_version_token(s, path, token);
    json_t *t = json_pack("{s:s,s:s,s:s}", "path", path, "kind", kind, "before", token);
    return !t || json_array_append_new(touched, t) ? NH_SYNCD_ERR_OOM : 0;
}

static int touched_published(const json_t *res, const nh_syncd_state *s);

static nh_syncd_change_kind change_kind(const char *kind) {
    if (!strcmp(kind, "delete")) return NH_SYNCD_CHANGE_DELETE;
    if (!strcmp(kind, "create")) return NH_SYNCD_CHANGE_CREATE;
    return NH_SYNCD_CHANGE_MODIFY;
}

/* Perform the HOME edits for a recorded intent. Idempotent so a crash
 * between persisting the intent and finishing the edits can be retried. */
static int apply_intent(int home_fd, const json_t *row, nh_conflict_choice choice,
                        const char *sibling, int resuming, const char **why) {
    nh_home_conflict_kind kind = kind_parse(json_string_value(json_object_get(row, "kind")));
    const char *path = json_string_value(json_object_get(row, "path"));
    const char *saved = json_string_value(json_object_get(row, "saved"));
    struct stat st;
    if (kind == NH_HOME_CONFLICT_EDIT) {
        if (choice == NH_CONFLICT_KEEP_CURRENT) {
            if (unlink_rel(home_fd, saved) == 0) return 0;
        } else {
            const char *to = choice == NH_CONFLICT_KEEP_BOTH ? sibling : path;
            if (lstat_rel(home_fd, saved, &st) != 0) {
                /* Only a resumed request may find its own move already done;
                 * a fresh request with no saved copy cannot use it. */
                if (resuming && errno == ENOENT && lstat_rel(home_fd, to, &st) == 0) return 0;
                *why = "saved-copy-missing";
                return NH_SYNCD_ERR_PATH;
            }
            if (rename_rel(home_fd, saved, to, choice == NH_CONFLICT_KEEP_BOTH) == 0) return 0;
            if (errno == EISDIR || errno == ENOTEMPTY || errno == ENOTDIR) {
                *why = "unsupported";
                return NH_SYNCD_ERR_PATH;
            }
        }
    } else {
        if (choice == NH_CONFLICT_USE_SAVED && unlink_rel(home_fd, path) != 0) {
            *why = errno == EISDIR ? "unsupported" : "io";
            return errno == EISDIR ? NH_SYNCD_ERR_PATH : NH_SYNCD_ERR_IO;
        }
        if (unlink_rel(home_fd, saved) == 0) return 0;
    }
    *why = "io";
    return NH_SYNCD_ERR_IO;
}

int nh_syncd_home_conflicts_resolve(const char *state_dir, int home_fd, nh_syncd_state *s,
                                    nh_syncd_batcher *batcher, const char *id,
                                    nh_conflict_choice choice, const char *expect,
                                    const char *request, const char **why) {
    static const char *unused;
    if (!why) why = &unused;
    *why = "invalid-request";
    if (!state_dir || home_fd < 0 || !is_hex(id, 32) || !is_hex(request, 32) ||
        !nh_conflict_choice_slug(choice)[0])
        return NH_SYNCD_ERR_ARG;
    int err = 0;
    json_t *doc = journal_load(state_dir, &err);
    if (!doc) {
        *why = "io";
        return err;
    }
    int rc = NH_SYNCD_OK;
    json_t *row = find_row(doc, id);
    json_t *res = row ? json_object_get(row, "resolution") : NULL;
    nh_home_conflict_kind kind = row ? kind_parse(json_string_value(json_object_get(row, "kind"))) : 0;
    const char *path = row ? json_string_value(json_object_get(row, "path")) : NULL;
    const char *saved = row ? json_string_value(json_object_get(row, "saved")) : NULL;
    if (!row || !kind || !safe_rel(path) || !safe_rel(saved)) {
        *why = "not-found";
        rc = NH_SYNCD_ERR_ARG;
        goto out;
    }
    if (res) {
        const char *req = json_string_value(json_object_get(res, "request"));
        const char *st = json_string_value(json_object_get(res, "state"));
        int resolved = st && !strcmp(st, "resolved");
        if (req && !strcmp(req, request)) {
            /* Same request again (crash or duplicate delivery): finish the
             * HOME edits if they were interrupted, never re-plan them. */
            nh_conflict_choice prior;
            if (resolved) goto out;
            if (nh_conflict_choice_parse(json_string_value(json_object_get(res, "choice")), &prior)) {
                *why = "io";
                rc = NH_SYNCD_ERR_JSON;
                goto out;
            }
            rc = apply_intent(home_fd, row, prior,
                              json_string_value(json_object_get(res, "sibling")), 1, why);
            if (!rc) rc = NH_CONFLICT_QUEUED;
            goto out;
        }
        *why = resolved ? "already-resolved" : "busy";
        rc = NH_SYNCD_ERR_ARG;
        goto out;
    }
    if (choice == NH_CONFLICT_KEEP_BOTH && kind != NH_HOME_CONFLICT_EDIT) {
        *why = "unsupported-choice";
        rc = NH_SYNCD_ERR_ARG;
        goto out;
    }
    char token[65];
    nh_syncd_state_version_token(s, path, token);
    if (expect && *expect && strcmp(expect, token) != 0) {
        *why = "stale";
        rc = NH_SYNCD_ERR_ARG;
        goto out;
    }

    /* Plan + persist the intent before touching HOME. */
    json_t *touched = json_array();
    char *sibling = NULL;
    rc = touched ? 0 : NH_SYNCD_ERR_OOM;
    if (!rc && kind == NH_HOME_CONFLICT_EDIT) {
        rc = touch(touched, s, saved, "delete");
        if (!rc && choice == NH_CONFLICT_USE_SAVED) rc = touch(touched, s, path, "modify");
        if (!rc && choice == NH_CONFLICT_KEEP_BOTH) {
            char day[16] = "";
            time_t at = (time_t)json_integer_value(json_object_get(row, "detected_at"));
            struct tm tm;
            if (at > 0 && gmtime_r(&at, &tm)) strftime(day, sizeof day, "%Y-%m-%d", &tm);
            rc = nh_conflict_sibling_path(home_fd, s, path, day[0] ? day : "saved", &sibling);
            if (!rc) rc = touch(touched, s, sibling, "create");
        }
    } else if (!rc) {
        if (choice == NH_CONFLICT_USE_SAVED) rc = touch(touched, s, path, "delete");
        if (!rc) rc = touch(touched, s, saved, "delete");
    }
    json_t *intent = NULL;
    if (!rc) {
        intent = json_pack("{s:s,s:s,s:s,s:I,s:O}", "request", request, "choice",
                           nh_conflict_choice_slug(choice), "state", "pending", "at",
                           (json_int_t)time(NULL), "touched", touched);
        if (!intent || (sibling && json_object_set_new(intent, "sibling", json_string(sibling))) ||
            json_object_set(row, "resolution", intent))
            rc = NH_SYNCD_ERR_OOM;
    }
    if (!rc) rc = journal_save(state_dir, doc);
    if (!rc) {
        rc = apply_intent(home_fd, row, choice, sibling, 0, why);
        if (rc) {
            /* Nothing was published: withdraw the intent so the user can retry. */
            json_object_del(row, "resolution");
            (void)journal_save(state_dir, doc);
        }
    } else if (!*why || !strcmp(*why, "invalid-request")) {
        *why = "io";
    }
    if (!rc) {
        size_t i;
        json_t *t;
        json_array_foreach(touched, i, t)
            if (batcher)
                (void)nh_syncd_batcher_push(batcher, json_string_value(json_object_get(t, "path")),
                                            change_kind(json_string_value(json_object_get(t, "kind"))));
        rc = NH_CONFLICT_QUEUED;
        /* Nothing the snapshot ever published was touched (e.g. dropping a
         * copy that never synced): there is no generation to wait for. */
        if (touched_published(intent, s)) {
            if (json_object_set_new(intent, "state", json_string("resolved")) ||
                journal_save(state_dir, doc) != NH_SYNCD_OK) {
                *why = "io";
                rc = NH_SYNCD_ERR_IO;
            } else {
                rc = NH_SYNCD_OK;
            }
        }
    }
    json_decref(intent);
    json_decref(touched);
    free(sibling);
out:
    json_decref(doc);
    return rc;
}

static int touched_published(const json_t *res, const nh_syncd_state *s) {
    size_t i;
    json_t *t;
    json_array_foreach(json_object_get(res, "touched"), i, t) {
        const char *path = json_string_value(json_object_get(t, "path"));
        const char *kind = json_string_value(json_object_get(t, "kind"));
        const char *before = json_string_value(json_object_get(t, "before"));
        if (!path || !kind || !before) return 0;
        char now[65];
        nh_syncd_state_version_token(s, path, now);
        if (!strcmp(kind, "delete") ? strcmp(now, TOKEN_ABSENT) != 0
                                    : (!strcmp(now, TOKEN_ABSENT) || !strcmp(now, before)))
            return 0;
    }
    return 1;
}

int nh_syncd_home_conflicts_commit_pending(const char *state_dir, const nh_syncd_state *s,
                                           nh_home_conflict_done_fn done, void *ud) {
    if (!state_dir) return NH_SYNCD_ERR_ARG;
    int err = 0;
    json_t *doc = journal_load(state_dir, &err);
    if (!doc) return err;
    int changed = 0;
    size_t i;
    json_t *row;
    json_array_foreach(json_object_get(doc, "conflicts"), i, row) {
        json_t *res = json_object_get(row, "resolution");
        const char *st = json_string_value(json_object_get(res, "state"));
        if (!st || strcmp(st, "pending") || !touched_published(res, s)) continue;
        if (json_object_set_new(res, "state", json_string("resolved")) ||
            json_object_set_new(res, "at", json_integer((json_int_t)time(NULL)))) {
            json_decref(doc);
            return NH_SYNCD_ERR_OOM;
        }
        changed = 1;
        if (done)
            done(ud, json_string_value(json_object_get(res, "request")),
                 json_string_value(json_object_get(row, "id")),
                 json_string_value(json_object_get(res, "choice")));
    }
    int rc = changed ? journal_save(state_dir, doc) : NH_SYNCD_OK;
    json_decref(doc);
    return rc;
}

/* ─────────────────────────── request inbox ─────────────────────────── */

struct nh_syncd_requests {
    int dir_fd;
    int ifd;
};

char *nh_syncd_requests_default_dir(void) {
    char *status = nh_porthome_status_default_path();
    if (!status) return NULL;
    char *slash = strrchr(status, '/');
    char *out = NULL;
    if (slash) {
        *slash = '\0';
        if (asprintf(&out, "%s/requests", status) < 0) out = NULL;
    }
    free(status);
    return out;
}

int nh_syncd_requests_open(const char *dir, nh_syncd_requests **out) {
    *out = NULL;
    if (!dir || !*dir) return NH_SYNCD_ERR_ARG;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return NH_SYNCD_ERR_IO;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (dfd < 0 || fstat(dfd, &st) != 0 || st.st_uid != getuid()) {
        if (dfd >= 0) close(dfd);
        return NH_SYNCD_ERR_IO;
    }
    if ((st.st_mode & 077) != 0) (void)fchmod(dfd, 0700);
    nh_syncd_requests *r = calloc(1, sizeof *r);
    if (!r) {
        close(dfd);
        return NH_SYNCD_ERR_OOM;
    }
    r->dir_fd = dfd;
    r->ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (r->ifd >= 0 &&
        inotify_add_watch(r->ifd, dir, IN_MOVED_TO | IN_CLOSE_WRITE | IN_ONLYDIR | IN_DONT_FOLLOW) < 0) {
        close(r->ifd);
        r->ifd = -1;
    }
    *out = r;
    return NH_SYNCD_OK;
}

int nh_syncd_requests_fd(const nh_syncd_requests *r) { return r ? r->ifd : -1; }

/* "<32 lowercase hex>.json"; the CLI's ".<id>.tmp" staging names never match. */
static int request_name(const char *name) {
    if (strlen(name) != 37 || strcmp(name + 32, ".json")) return 0;
    for (int i = 0; i < 32; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return 1;
}

static int parse_request(const char *name, const char *buf, size_t len, nh_syncd_request *out) {
    json_error_t e;
    json_t *j = json_loadb(buf, len, JSON_REJECT_DUPLICATES, &e);
    int ok = json_is_object(j) && json_object_size(j) == 6;
    const char *op = json_string_value(json_object_get(j, "op"));
    const char *req = json_string_value(json_object_get(j, "request"));
    const char *conflict = json_string_value(json_object_get(j, "conflict"));
    const char *choice = json_string_value(json_object_get(j, "choice"));
    const char *expect = json_string_value(json_object_get(j, "expect"));
    ok = ok && json_integer_value(json_object_get(j, "schema")) == 1 &&
         json_is_integer(json_object_get(j, "schema")) && op && !strcmp(op, "resolve-conflict") &&
         is_hex(req, 32) && !strncmp(req, name, 32) && conflict &&
         (is_hex(conflict, 32) || is_hex(conflict, 64)) && choice &&
         !nh_conflict_choice_parse(choice, &out->choice) && expect && strlen(expect) <= 64;
    if (ok) {
        snprintf(out->request, sizeof out->request, "%s", req);
        snprintf(out->conflict, sizeof out->conflict, "%s", conflict);
        snprintf(out->expect, sizeof out->expect, "%s", expect);
    }
    json_decref(j);
    return ok ? 0 : -1;
}

int nh_syncd_requests_next(nh_syncd_requests *r, nh_syncd_request *out, char bad_request[33]) {
    bad_request[0] = '\0';
    if (!r) return 0;
    if (r->ifd >= 0) {
        char drain[4096];
        while (read(r->ifd, drain, sizeof drain) > 0) {
        }
    }
    int dup_fd = fcntl(r->dir_fd, F_DUPFD_CLOEXEC, 0);
    DIR *d = dup_fd >= 0 ? fdopendir(dup_fd) : NULL;
    if (!d) {
        if (dup_fd >= 0) close(dup_fd);
        return NH_SYNCD_ERR_IO;
    }
    rewinddir(d);
    char best[64] = "";
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
        if (request_name(de->d_name) && (!best[0] || strcmp(de->d_name, best) < 0))
            memcpy(best, de->d_name, 38); /* request_name() pinned the length to 37 */
    closedir(d);
    if (!best[0]) return 0;

    int fd = openat(r->dir_fd, best, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    char buf[REQUEST_MAX_BYTES + 1];
    ssize_t n = -1;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
        st.st_size <= REQUEST_MAX_BYTES)
        n = read(fd, buf, sizeof buf);
    if (fd >= 0) close(fd);
    memset(out, 0, sizeof *out);
    if (n < 0 || n > REQUEST_MAX_BYTES || parse_request(best, buf, (size_t)n, out) != 0) {
        snprintf(bad_request, 33, "%.32s", best);
        if (!is_hex(bad_request, 32)) bad_request[0] = '\0';
        (void)unlinkat(r->dir_fd, best, 0);
        return 2;
    }
    return 1;
}

void nh_syncd_requests_done(nh_syncd_requests *r, const char *request) {
    if (!r || !is_hex(request, 32)) return;
    char name[40];
    snprintf(name, sizeof name, "%s.json", request);
    (void)unlinkat(r->dir_fd, name, 0);
}

void nh_syncd_requests_close(nh_syncd_requests *r) {
    if (!r) return;
    if (r->ifd >= 0) close(r->ifd);
    close(r->dir_fd);
    free(r);
}
