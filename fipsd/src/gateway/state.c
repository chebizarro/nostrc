#ifdef __linux__
#define _GNU_SOURCE 1
#else
#define _DARWIN_C_SOURCE 1
#endif
#include "gateway/state.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#define FIPSD_GW_TMPFS_MAGIC 0x01021994L
#else
#include <sys/mount.h>
#include <sys/param.h>
#endif

#define MODULE "gateway"

void fipsd_gw_loaded_clear(fipsd_gw_loaded *loaded)
{
    fipsd_gw_pool_state_clear(&loaded->state);
    loaded->kind = FIPSD_GW_LOADED_ABSENT; loaded->reason[0] = 0;
}

int fipsd_gw_is_tmpfs(const char *path, void *unused)
{
    (void)unused;
    struct statfs st;
    if (statfs(path, &st) != 0) return -1;
#ifdef __linux__
    /* f_type's width differs between libcs; compare as the widest type. */
    return (long long)st.f_type == (long long)FIPSD_GW_TMPFS_MAGIC;
#else
    return strcmp(st.f_fstypename, "tmpfs") == 0;
#endif
}

char *fipsd_gw_state_encode(const fipsd_gw_pool_state *s)
{
    json_t *pool = json_string(s->pool);
    char *pool_text = pool ? json_dumps(pool, JSON_ENCODE_ANY) : NULL;
    json_decref(pool);
    if (!pool_text) return NULL;
    size_t cap = strlen(pool_text) + 160 + s->held_len * 24, at = 0;
    char *out = malloc(cap);
    if (!out) { free(pool_text); return NULL; }
    at += (size_t)snprintf(out + at, cap - at, "{\"version\":%u,\"pool\":%s,\"total\":%u,\"from\":%u,\"span\":%u,\"held\":[",
                           s->version, pool_text, s->total, s->from, s->span);
    free(pool_text);
    for (size_t i = 0; i < s->held_len; ++i)
        at += (size_t)snprintf(out + at, cap - at, "%s[%u,%u]", i ? "," : "", s->held[i][0], s->held[i][1]);
    snprintf(out + at, cap - at, "],\"hold_secs\":%llu}", (unsigned long long)s->hold_secs);
    return out;
}

static bool field_uint(const json_t *root, const char *name, uint64_t max, uint64_t *out, char *reason, size_t cap)
{
    const json_t *v = json_object_get(root, name);
    if (!v) { snprintf(reason, cap, "cannot parse the state file: missing field `%s`", name); return false; }
    if (!json_is_integer(v) || json_integer_value(v) < 0 || (uint64_t)json_integer_value(v) > max) {
        snprintf(reason, cap, "cannot parse the state file: field `%s` is not an integer in 0..=%llu", name, (unsigned long long)max);
        return false;
    }
    *out = (uint64_t)json_integer_value(v);
    return true;
}
bool fipsd_gw_state_decode(const char *json, size_t len, fipsd_gw_pool_state *out, char *reason, size_t cap)
{
    memset(out, 0, sizeof *out);
    json_error_t error;
    json_t *root = json_loadb(json, len, JSON_REJECT_DUPLICATES, &error);
    if (!root) { snprintf(reason, cap, "cannot parse the state file: %s at line %d column %d", error.text, error.line, error.column); return false; }
    bool ok = false;
    uint64_t version, total, from, span, hold;
    const json_t *pool = json_object_get(root, "pool"), *held = json_object_get(root, "held");
    if (!json_is_object(root)) { snprintf(reason, cap, "cannot parse the state file: not an object"); goto done; }
    if (!field_uint(root, "version", UINT32_MAX, &version, reason, cap) || !field_uint(root, "total", UINT32_MAX, &total, reason, cap) ||
        !field_uint(root, "from", UINT32_MAX, &from, reason, cap) || !field_uint(root, "span", UINT32_MAX, &span, reason, cap) ||
        !field_uint(root, "hold_secs", UINT64_MAX, &hold, reason, cap)) goto done;
    if (!json_is_string(pool) || json_string_length(pool) >= sizeof out->pool || strlen(json_string_value(pool)) != json_string_length(pool)) {
        snprintf(reason, cap, "cannot parse the state file: field `pool` is not a pool CIDR"); goto done;
    }
    if (!json_is_array(held)) { snprintf(reason, cap, "cannot parse the state file: field `held` is not an array"); goto done; }
    size_t n = json_array_size(held);
    if (n) {
        out->held = calloc(n, sizeof *out->held);
        if (!out->held) { snprintf(reason, cap, "cannot parse the state file: out of memory"); goto done; }
    }
    for (size_t i = 0; i < n; ++i) {
        const json_t *pair = json_array_get(held, i);
        const json_t *a = json_array_get(pair, 0), *b = json_array_get(pair, 1);
        if (!json_is_array(pair) || json_array_size(pair) != 2 || !json_is_integer(a) || !json_is_integer(b) ||
            json_integer_value(a) < 0 || json_integer_value(a) > UINT32_MAX || json_integer_value(b) < 0 || json_integer_value(b) > UINT32_MAX) {
            snprintf(reason, cap, "cannot parse the state file: held entry %zu is not a pair of offsets", i); goto done;
        }
        out->held[i][0] = (uint32_t)json_integer_value(a); out->held[i][1] = (uint32_t)json_integer_value(b);
        out->held_len = i + 1;
    }
    out->version = (uint32_t)version; out->total = (uint32_t)total; out->from = (uint32_t)from; out->span = (uint32_t)span;
    out->hold_secs = hold;
    snprintf(out->pool, sizeof out->pool, "%s", json_string_value(pool));
    ok = true;
done:
    json_decref(root);
    if (!ok) fipsd_gw_pool_state_clear(out);
    return ok;
}

bool fipsd_gw_state_validate(const fipsd_gw_pool_state *s, const char *cidr, uint32_t total, char *reason, size_t cap)
{
    if (s->version != 1) { snprintf(reason, cap, "unknown state version %u", s->version); return false; }
    if (strcmp(s->pool, cidr) || s->total != total) {
        snprintf(reason, cap, "state is for pool %s of %u addresses, not %s of %u", s->pool, s->total, cidr, total);
        return false;
    }
    if (s->from < 1 || s->from > total) { snprintf(reason, cap, "offset %u is outside the pool", s->from); return false; }
    if (s->span > total) { snprintf(reason, cap, "stretch of %u exceeds the pool", s->span); return false; }
    uint32_t previous = 0;
    for (size_t i = 0; i < s->held_len; ++i) {
        uint32_t first = s->held[i][0], last = s->held[i][1];
        if (first <= previous || first > last || last > total) {
            snprintf(reason, cap, "held range %u-%u is out of order or outside the pool", first, last);
            return false;
        }
        previous = last;
    }
    if (s->hold_secs > FIPSD_GW_RESTART_HOLD_MAX_SECS) {
        snprintf(reason, cap, "hold of %llu s exceeds %llu s", (unsigned long long)s->hold_secs,
                 (unsigned long long)FIPSD_GW_RESTART_HOLD_MAX_SECS);
        return false;
    }
    return true;
}

static int join(char *out, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}
static char *read_all(const char *path, size_t *len, int *err)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { *err = errno; return NULL; }
    size_t cap = 4096, at = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (at == cap) {
            /* A real state is a few kilobytes; the bound keeps a hostile file
             * from exhausting memory. */
            if (cap >= (size_t)16 << 20) { free(buf); buf = NULL; *err = EFBIG; break; }
            char *grown = realloc(buf, cap * 2);
            if (!grown) { free(buf); buf = NULL; *err = ENOMEM; break; }
            buf = grown; cap *= 2;
        }
        ssize_t n = read(fd, buf + at, cap - at);
        if (n < 0) { if (errno == EINTR) continue; *err = errno; free(buf); buf = NULL; break; }
        if (n == 0) break;
        at += (size_t)n;
    }
    if (!buf && !*err) *err = ENOMEM;
    close(fd);
    *len = at;
    return buf;
}
void fipsd_gw_state_load(const char *dir, const char *cidr, uint32_t total, fipsd_gw_loaded *out)
{
    memset(out, 0, sizeof *out);
    char path[4096];
    if (join(path, sizeof path, dir, FIPSD_GW_STATE_FILE) != 0) {
        out->kind = FIPSD_GW_LOADED_INVALID; snprintf(out->reason, sizeof out->reason, "state path is too long"); return;
    }
    size_t len = 0; int read_err = 0;
    char *bytes = read_all(path, &len, &read_err);
    if (!bytes && read_err == ENOENT) { out->kind = FIPSD_GW_LOADED_ABSENT; return; }
    /* Whatever the file holds, it is removed before this returns, so it is
     * never trusted twice. */
    if (unlink(path) != 0 && errno != ENOENT) {
        out->kind = FIPSD_GW_LOADED_INVALID;
        snprintf(out->reason, sizeof out->reason, "cannot remove the state file: %s", strerror(errno));
        free(bytes); return;
    }
    if (!bytes) {
        out->kind = FIPSD_GW_LOADED_INVALID;
        snprintf(out->reason, sizeof out->reason, "cannot read the state file: %s", strerror(read_err)); return;
    }
    fipsd_gw_pool_state state;
    bool parsed = fipsd_gw_state_decode(bytes, len, &state, out->reason, sizeof out->reason);
    free(bytes);
    if (!parsed) { out->kind = FIPSD_GW_LOADED_INVALID; return; }
    if (!fipsd_gw_state_validate(&state, cidr, total, out->reason, sizeof out->reason)) {
        fipsd_gw_pool_state_clear(&state); out->kind = FIPSD_GW_LOADED_INVALID; return;
    }
    out->kind = FIPSD_GW_LOADED_RESTORED; out->state = state;
}
void fipsd_gw_state_start_from(const fipsd_gw_loaded *loaded, uint32_t random, fipsd_gw_pool_begin *out)
{
    memset(out, 0, sizeof *out);
    if (loaded->kind == FIPSD_GW_LOADED_RESTORED) { out->restored = true; out->state = loaded->state; }
    else out->offset = random;
}

static int require_tmpfs(const char *path, fipsd_gw_tmpfs_fn on_tmpfs, void *user, char *err, size_t cap)
{
    int rc = on_tmpfs(path, user);
    if (rc < 0) { int e = errno ? errno : EIO; snprintf(err, cap, "%s: %s", path, strerror(e)); return e; }
    if (!rc) { snprintf(err, cap, "%s is not on tmpfs; pool state is kept only on tmpfs", path); return EXDEV; }
    return 0;
}
static int write_state(const char *dir, const fipsd_gw_pool_state *state, fipsd_gw_tmpfs_fn on_tmpfs, void *user,
                       char *err, size_t cap)
{
    char parent[4096];
    snprintf(parent, sizeof parent, "%s", dir);
    char *slash = strrchr(parent, '/');
    if (!slash) { snprintf(err, cap, "%s has no parent directory", dir); return EINVAL; }
    if (slash == parent) slash[1] = 0; else *slash = 0;
    struct stat st;
    if (stat(parent, &st) != 0 || !S_ISDIR(st.st_mode)) { snprintf(err, cap, "%s does not exist", parent); return ENOENT; }
    int rc = require_tmpfs(parent, on_tmpfs, user, err, cap);
    if (rc) return rc;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) { rc = errno; snprintf(err, cap, "cannot create %s: %s", dir, strerror(rc)); return rc; }
    /* A bind mount could put the directory on other storage than its parent. */
    if ((rc = require_tmpfs(dir, on_tmpfs, user, err, cap))) return rc;
    char *bytes = fipsd_gw_state_encode(state);
    if (!bytes) { snprintf(err, cap, "cannot encode the pool state"); return ENOMEM; }
    char tmp[4096], file[4096];
    if (join(tmp, sizeof tmp, dir, FIPSD_GW_STATE_TMP) != 0 || join(file, sizeof file, dir, FIPSD_GW_STATE_FILE) != 0) {
        free(bytes); snprintf(err, cap, "state path is too long"); return ENAMETOOLONG;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { rc = errno; free(bytes); snprintf(err, cap, "cannot open %s: %s", tmp, strerror(rc)); return rc; }
    size_t len = strlen(bytes), at = 0;
    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);
        if (n < 0) { if (errno == EINTR) continue; rc = errno; break; }
        at += (size_t)n;
    }
    free(bytes);
    if (close(fd) != 0 && !rc) rc = errno;
    if (rc) { snprintf(err, cap, "cannot write %s: %s", tmp, strerror(rc)); return rc; }
    if (rename(tmp, file) != 0) { rc = errno; snprintf(err, cap, "cannot rename %s: %s", tmp, strerror(rc)); return rc; }
    return 0;
}
int fipsd_gw_state_save(const char *dir, const fipsd_gw_pool_state *state, fipsd_gw_tmpfs_fn on_tmpfs, void *user,
                        char *err, size_t cap)
{
    if (cap) err[0] = 0;
    int rc = write_state(dir, state, on_tmpfs, user, err, cap);
    if (rc) {
        /* Unlinking needs no free space, so this works when the write failed
         * for lack of it. */
        char path[4096];
        if (join(path, sizeof path, dir, FIPSD_GW_STATE_FILE) == 0) (void)unlink(path);
        if (join(path, sizeof path, dir, FIPSD_GW_STATE_TMP) == 0) (void)unlink(path);
    }
    return rc;
}

fipsd_gw_pool_error fipsd_gw_state_start_pool(const char *dir, const char *cidr, uint32_t total, uint64_t ttl_secs,
                                              uint64_t grace_secs, uint32_t random, fipsd_gw_instant now,
                                              fipsd_gw_tmpfs_fn on_tmpfs, void *user, fipsd_gw_started *out)
{
    memset(out, 0, sizeof *out);
    fipsd_gw_state_load(dir, cidr, total, &out->loaded);
    fipsd_gw_pool_begin begin;
    fipsd_gw_state_start_from(&out->loaded, random, &begin);
    fipsd_gw_pool_error error = FIPSD_GW_POOL_OK;
    out->pool = fipsd_gw_pool_start(cidr, ttl_secs, grace_secs, &begin, now, &error);
    if (!out->pool) { fipsd_gw_loaded_clear(&out->loaded); return error ? error : FIPSD_GW_POOL_INVALID_CIDR; }
    fipsd_gw_pool_state carry;
    memset(&carry, 0, sizeof carry);
    if (fipsd_gw_pool_carry_state(out->pool, &carry)) {
        out->carried = true;
        out->carried_rc = fipsd_gw_state_save(dir, &carry, on_tmpfs, user, out->carried_err, sizeof out->carried_err);
        fipsd_gw_pool_state_clear(&carry);
    }
    return FIPSD_GW_POOL_OK;
}

fipsd_gw_save_report fipsd_gw_save_log_report(fipsd_gw_save_log *log, int rc, const char *err)
{
    if (rc) {
        const char *text = err && *err ? err : strerror(rc);
        bool repeat = log->failed && !strcmp(log->last, text);
        if (repeat) FIPSD_DEBUG(MODULE, "Pool state not saved: error=%s", text);
        else FIPSD_WARN(MODULE, "Pool state not saved; after a restart only a random starting address protects answers clients still hold: error=%s", text);
        log->failed = true; snprintf(log->last, sizeof log->last, "%s", text);
        return repeat ? FIPSD_GW_SAVE_REPEATED : FIPSD_GW_SAVE_FAILED;
    }
    if (log->failed) { log->failed = false; log->last[0] = 0; FIPSD_INFO(MODULE, "Pool state saved again"); return FIPSD_GW_SAVE_RECOVERED; }
    return FIPSD_GW_SAVE_OK;
}
