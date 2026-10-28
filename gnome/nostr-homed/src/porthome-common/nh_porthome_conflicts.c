/*
 * nh_porthome_conflicts.c — see nh_porthome_conflicts.h (nostrc-h10m.6).
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "nh_porthome_conflicts.h"

#include "nh_porthome_status.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

static int hex_len(const char *s) {
    size_t n = 0;
    for (; s && s[n]; ++n)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) return -1;
    return (int)n;
}

static char *dup_str(const json_t *v) {
    const char *s = json_string_value(v);
    return s ? strdup(s) : NULL;
}

static void conflict_free(nh_porthome_conflict *c) {
    free(c->path);
    free(c->saved_path);
    free(c->marker);
    free(c->kind);
    free(c->writer);
    free(c->vault_id);
    free(c->expect);
    memset(c, 0, sizeof *c);
}

static unsigned choices_from(const json_t *array) {
    unsigned mask = 0;
    size_t i;
    json_t *v;
    json_array_foreach(array, i, v) {
        const char *s = json_string_value(v);
        if (!s) continue;
        if (!strcmp(s, "keep-current")) mask |= NH_CONFLICT_CHOICE_KEEP_CURRENT;
        else if (!strcmp(s, "use-saved")) mask |= NH_CONFLICT_CHOICE_USE_SAVED;
        else if (!strcmp(s, "keep-both")) mask |= NH_CONFLICT_CHOICE_KEEP_BOTH;
    }
    return mask;
}

/* 0 on success, 1 to skip a malformed row, -ENOMEM. */
static int row_load(const json_t *row, nh_conflict_source src, nh_porthome_conflict *c) {
    memset(c, 0, sizeof *c);
    const char *id = json_string_value(json_object_get(row, "id"));
    const char *path = json_string_value(json_object_get(row, "path"));
    int idn = hex_len(id);
    if (!json_is_object(row) || !path || !*path ||
        idn != (src == NH_CONFLICT_SOURCE_HOME ? 32 : 64))
        return 1;
    memcpy(c->id, id, (size_t)idn + 1);
    c->source = src;
    c->path = strdup(path);
    c->saved_path = dup_str(json_object_get(row, "destination"));
    c->marker = dup_str(json_object_get(row, "marker"));
    c->kind = dup_str(json_object_get(row, "kind"));
    c->writer = dup_str(json_object_get(row, "writer"));
    c->vault_id = dup_str(json_object_get(row, "vault_id"));
    c->expect = dup_str(json_object_get(row, "expect"));
    if (!c->path || !c->expect) {
        int malformed = c->path && !c->expect;
        conflict_free(c);
        return malformed ? 1 : -ENOMEM;
    }
    c->saved_present = json_is_true(json_object_get(row, "present"));
    /* Absent field = assume something is there (older daemons). */
    c->current_present = !json_is_false(json_object_get(row, "current_present"));
    c->overflow = json_is_true(json_object_get(row, "overflow"));
    c->whole_folder = json_is_true(json_object_get(row, "root_snapshot"));
    const char *st = json_string_value(json_object_get(row, "state"));
    c->pending = st && !strcmp(st, "pending");
    const char *mine = json_string_value(json_object_get(row, "mine"));
    c->mine = !mine ? NH_CONFLICT_MINE_UNKNOWN
              : !strcmp(mine, "current") ? NH_CONFLICT_MINE_CURRENT
              : !strcmp(mine, "saved") ? NH_CONFLICT_MINE_SAVED
                                       : NH_CONFLICT_MINE_UNKNOWN;
    c->choices = choices_from(json_object_get(row, "choices"));
    if (!c->choices) c->choices = NH_CONFLICT_CHOICE_KEEP_CURRENT;
    json_int_t at = json_integer_value(json_object_get(row, "detected_at"));
    c->detected_at = at > 0 ? (int64_t)at : 0;
    return 0;
}

int nh_porthome_conflicts_parse(const char *status_json, size_t len,
                                nh_porthome_conflict_list *out) {
    memset(out, 0, sizeof *out);
    json_t *doc = status_json ? json_loadb(status_json, len, 0, NULL) : NULL;
    if (!json_is_object(doc)) {
        json_decref(doc);
        return -EINVAL;
    }
    json_t *syncd = json_object_get(doc, "syncd");
    json_t *home = json_object_get(syncd, "home_conflicts");
    json_t *vault = json_object_get(syncd, "vault_conflicts");
    size_t cap = json_array_size(home) + json_array_size(vault);
    int rc = 0;
    if (cap) {
        out->items = calloc(cap, sizeof *out->items);
        if (!out->items) rc = -ENOMEM;
    }
    const struct { json_t *rows; nh_conflict_source src; } sources[] = {
        {home, NH_CONFLICT_SOURCE_HOME}, {vault, NH_CONFLICT_SOURCE_VAULT}};
    for (size_t s = 0; !rc && s < 2; ++s) {
        size_t i;
        json_t *row;
        json_array_foreach(sources[s].rows, i, row) {
            int r = row_load(row, sources[s].src, &out->items[out->n]);
            if (r < 0) {
                rc = r;
                break;
            }
            if (r == 0) out->n++;
        }
    }
    json_decref(doc);
    if (rc) nh_porthome_conflicts_clear(out);
    return rc;
}

void nh_porthome_conflicts_clear(nh_porthome_conflict_list *list) {
    if (!list) return;
    for (size_t i = 0; i < list->n; ++i) conflict_free(&list->items[i]);
    free(list->items);
    list->items = NULL;
    list->n = 0;
}

int nh_porthome_conflicts_find(const nh_porthome_conflict_list *list, const char *prefix) {
    int n = hex_len(prefix);
    if (n < 6 || n > 64) return -EINVAL;
    int found = -ENOENT;
    for (size_t i = 0; list && i < list->n; ++i)
        if (!strncmp(list->items[i].id, prefix, (size_t)n)) {
            if (found >= 0) return -EEXIST;
            found = (int)i;
        }
    return found;
}

const char *nh_porthome_conflict_choice_slug(unsigned choice) {
    switch (choice) {
    case NH_CONFLICT_CHOICE_KEEP_CURRENT: return "keep-current";
    case NH_CONFLICT_CHOICE_USE_SAVED: return "use-saved";
    case NH_CONFLICT_CHOICE_KEEP_BOTH: return "keep-both";
    }
    return NULL;
}

int nh_porthome_conflict_choice_from_flag(const nh_porthome_conflict *c, const char *flag,
                                          unsigned *choice, bool *alias) {
    *alias = false;
    if (!flag) return -EINVAL;
    if (!strncmp(flag, "--", 2)) flag += 2;
    if (!strcmp(flag, "keep-current")) *choice = NH_CONFLICT_CHOICE_KEEP_CURRENT;
    else if (!strcmp(flag, "use-saved")) *choice = NH_CONFLICT_CHOICE_USE_SAVED;
    else if (!strcmp(flag, "keep-both")) *choice = NH_CONFLICT_CHOICE_KEEP_BOTH;
    else if (!strcmp(flag, "keep-mine") || !strcmp(flag, "take-theirs")) {
        *alias = true;
        bool keep_mine = !strcmp(flag, "keep-mine");
        if (!c || c->mine == NH_CONFLICT_MINE_UNKNOWN) return -ENOENT;
        bool mine_is_current = c->mine == NH_CONFLICT_MINE_CURRENT;
        *choice = keep_mine == mine_is_current ? NH_CONFLICT_CHOICE_KEEP_CURRENT
                                               : NH_CONFLICT_CHOICE_USE_SAVED;
    } else {
        return -EINVAL;
    }
    return 0;
}

const char *nh_porthome_conflict_choice_label(const nh_porthome_conflict *c, unsigned choice) {
    bool deletion = !c->saved_present && c->current_present;
    bool removed = c->saved_present && !c->current_present;
    switch (choice) {
    case NH_CONFLICT_CHOICE_KEEP_CURRENT:
        if (c->whole_folder) return "Keep the folder as it is now";
        return deletion ? "Keep the file" : removed ? "Leave it removed" : "Keep the current version";
    case NH_CONFLICT_CHOICE_USE_SAVED:
        return deletion ? "Delete the file" : removed ? "Restore the saved version"
                                                      : "Use the saved version";
    case NH_CONFLICT_CHOICE_KEEP_BOTH:
        return "Keep both (add the saved version as a copy)";
    }
    return "";
}

int nh_porthome_conflict_describe(const nh_porthome_conflict *c, char *buf, size_t cap) {
    char who[48] = "another device";
    if (c->source == NH_CONFLICT_SOURCE_VAULT && c->writer)
        snprintf(who, sizeof who, "another member (%.8s)", c->writer);
    const char *saved = c->saved_path ? c->saved_path : "";
    bool mine_current = c->mine == NH_CONFLICT_MINE_CURRENT;
    bool mine_saved = c->mine == NH_CONFLICT_MINE_SAVED;
    int n;
    if (c->whole_folder) {
        n = snprintf(buf, cap,
                     "Changes made here could not be shared because the folder's sharing "
                     "settings changed first. They are kept in %s so you can copy back "
                     "anything you still need.",
                     saved);
    } else if (c->kind && !strcmp(c->kind, "deleted-locally")) {
        n = snprintf(buf, cap,
                     "You deleted this file here, but it was changed on %s, so it was put "
                     "back.",
                     who);
    } else if (c->kind && !strcmp(c->kind, "deleted-remotely")) {
        n = snprintf(buf, cap, "This file was deleted on %s, but it was changed here.", who);
    } else if (!c->saved_present && c->current_present) {
        n = snprintf(buf, cap, "This file was deleted %s, but it was %s.",
                     mine_saved ? "here" : c->source == NH_CONFLICT_SOURCE_VAULT
                                               ? "by another member" : "on another device",
                     mine_current ? "changed here" : "changed in the current version");
    } else if (c->saved_present && !c->current_present) {
        n = snprintf(buf, cap,
                     "This file was added or changed on %s but is not in the current "
                     "version. That version is saved%s%s.%s",
                     who, *saved ? " at " : "", saved,
                     mine_saved ? " The saved version is yours." : "");
    } else {
        n = snprintf(buf, cap,
                     "This file was changed in two places at the same time. The current "
                     "version is in place; the other version is saved%s%s.%s",
                     *saved ? " at " : "", saved,
                     mine_current ? " The current version is yours."
                     : mine_saved ? " The saved version is yours."
                                  : "");
    }
    if (n < 0 || (size_t)n >= cap) {
        if (cap) buf[0] = '\0';
        return -1;
    }
    return n;
}

int nh_porthome_resolution_lookup(const char *status_json, size_t len, const char *request,
                                  char result[16], char reason[32]) {
    json_t *doc = status_json ? json_loadb(status_json, len, 0, NULL) : NULL;
    json_t *list = json_object_get(json_object_get(doc, "syncd"), "resolutions");
    int rc = -ENOENT;
    size_t i;
    json_t *row;
    /* syncd writes the ring most-recent-first. */
    json_array_foreach(list, i, row) {
        const char *req = json_string_value(json_object_get(row, "request"));
        if (!req || !request || strcmp(req, request)) continue;
        const char *res = json_string_value(json_object_get(row, "result"));
        const char *why = json_string_value(json_object_get(row, "reason"));
        snprintf(result, 16, "%s", res ? res : "");
        snprintf(reason, 32, "%s", why ? why : "");
        rc = 0;
        break;
    }
    json_decref(doc);
    return rc;
}

const char *nh_porthome_resolution_reason_text(const char *reason) {
    static const struct { const char *slug, *text; } table[] = {
        {"not-found", "That conflict no longer exists. It may already be resolved."},
        {"already-resolved", "That conflict was already resolved."},
        {"busy", "Sync is busy with this folder right now. Try again in a moment."},
        {"unsupported-choice", "That choice is not available for this conflict."},
        {"stale", "The file changed after the conflict was listed. List the conflicts again "
                  "and choose again."},
        {"offline", "The relays could not be reached. Your choice is applied on this device "
                    "and will be shared when the connection returns."},
        {"saved-copy-missing", "The saved copy is missing, so it cannot be used. You can "
                               "still keep the current version."},
        {"unsupported", "This conflict involves a folder and cannot be resolved automatically. "
                        "Move the files by hand, then keep the current version."},
        {"invalid-request", "The sync service rejected the request as malformed."},
        {"io", "The sync service could not apply the change because of a file system error. "
               "Check the nostr-home-sync log."},
    };
    for (size_t i = 0; reason && i < sizeof table / sizeof table[0]; ++i)
        if (!strcmp(reason, table[i].slug)) return table[i].text;
    return "The sync service could not apply the change.";
}

char *nh_porthome_requests_default_dir(void) {
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

int nh_porthome_request_write(const char *dir, const char *conflict_id, unsigned choice,
                              const char *expect, char request_out[33]) {
    const char *slug = nh_porthome_conflict_choice_slug(choice);
    int idn = hex_len(conflict_id);
    if (!dir || !slug || (idn != 32 && idn != 64) || !expect || strlen(expect) > 64)
        return -EINVAL;
    unsigned char raw[16];
    for (size_t got = 0; got < sizeof raw;) {
        ssize_t n = getrandom(raw + got, sizeof raw - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        got += (size_t)n;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        request_out[2 * i] = hex[raw[i] >> 4];
        request_out[2 * i + 1] = hex[raw[i] & 15];
    }
    request_out[32] = '\0';
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -errno;

    json_t *req = json_pack("{s:i,s:s,s:s,s:s,s:s,s:s}", "schema", 1, "op", "resolve-conflict",
                            "request", request_out, "conflict", conflict_id, "choice", slug,
                            "expect", expect);
    char *body = req ? json_dumps(req, JSON_COMPACT) : NULL;
    json_decref(req);
    if (!body) return -ENOMEM;

    /* Stage under a name syncd ignores, then rename into place so syncd
     * only ever sees a complete file (IN_MOVED_TO). */
    char tmp[4096], final[4096];
    int rc = 0;
    if (snprintf(tmp, sizeof tmp, "%s/.%s.tmp", dir, request_out) >= (int)sizeof tmp ||
        snprintf(final, sizeof final, "%s/%s.json", dir, request_out) >= (int)sizeof final)
        rc = -ENAMETOOLONG;
    int fd = rc ? -1 : open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (!rc && fd < 0) rc = -errno;
    if (!rc) {
        size_t len = strlen(body), off = 0;
        while (!rc && off < len) {
            ssize_t w = write(fd, body + off, len - off);
            if (w < 0 && errno != EINTR) rc = -errno;
            if (w > 0) off += (size_t)w;
        }
        if (!rc && fsync(fd) != 0) rc = -errno;
    }
    if (fd >= 0) close(fd);
    if (!rc && rename(tmp, final) != 0) rc = -errno;
    if (rc && fd >= 0) unlink(tmp);
    free(body);
    return rc;
}
