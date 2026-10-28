/*
 * hanami-fetch.c - durable plaintext clone and fetch over Blossom + Nostr
 *
 * The owner's kind-30618 supplies refs and the head of the object manifest
 * chain (hanami-manifest.c). Objects are read through the verifying Blossom
 * ODB backend and written into the repository's own ODB, so a clone is an
 * ordinary self-contained Git repository; libgit2 writes refs and checks out.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "hanami-fetch.h"
#include "hanami-discovery.h"
#include "hanami-index-private.h"
#include "hanami-manifest.h"
#include "hanami-sealed.h"
#include "hanami/hanami-odb-backend.h"
#include <git2/sys/odb_backend.h>
#include <git2/sys/transport.h>
#include <git2/errors.h>
#if defined(__has_include)
#  if __has_include(<git2/sys/errors.h>)
#    include <git2/sys/errors.h>
#  endif
#endif
#include <errno.h>
#include <dirent.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

hanami_error_t hanami_parse_nostr_uri(const char *uri, char **owner, char **repo_id)
{
    if (!uri || !owner || !repo_id) return HANAMI_ERR_INVALID_ARG;
    *owner = NULL; *repo_id = NULL;
    if (strncmp(uri, "nostr://", 8) != 0) return HANAMI_ERR_INVALID_ARG;
    const char *rest = uri + 8, *slash = strchr(rest, '/');
    if (!slash || slash == rest || !slash[1]) return HANAMI_ERR_INVALID_ARG;
    *owner = strndup(rest, (size_t)(slash - rest));
    *repo_id = strdup(slash + 1);
    if (!*owner || !*repo_id) {
        free(*owner); free(*repo_id); *owner = NULL; *repo_id = NULL;
        return HANAMI_ERR_NOMEM;
    }
    return HANAMI_OK;
}

/* =========================================================================
 * Operation context
 * ========================================================================= */

typedef struct {
    hanami_error_t err;
    char msg[512];
    const char *repo_id, *endpoint;
    char *relays_csv;
    blossom_client_t *client;
    hanami_index_t *cache;
    git_odb_backend *remote;
    nip34_repo_state_t *state;  /* NULL: no 30618 yet (empty repository) */
    char head[65];
} fetch_ctx;

static int fail(fetch_ctx *c, hanami_error_t err, const char *fmt, ...)
{
    if (c->err == HANAMI_OK) {
        c->err = err;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->msg, sizeof(c->msg), fmt, ap);
        va_end(ap);
    }
    git_error_set(GIT_ERROR_INVALID, "%s", c->msg);
    return -1;
}

static void ctx_dispose(fetch_ctx *c)
{
    if (c->remote) c->remote->free(c->remote);
    hanami_index_close(c->cache);
    blossom_client_free(c->client);
    nip34_repo_state_free(c->state);
    free(c->relays_csv);
}

static hanami_error_t ctx_client(fetch_ctx *c, const hanami_signer_t *signer, bool insecure)
{
    blossom_client_opts_t opts = {
        .endpoint = c->endpoint, .timeout_seconds = 60,
        .allow_insecure = insecure, .user_agent = "libhanami/" HANAMI_VERSION_STRING
    };
    hanami_error_t err = blossom_client_new(&opts, signer, &c->client);
    return err == HANAMI_OK ? HANAMI_OK
        : (fail(c, err, "hanami: cannot use Blossom server %s", c->endpoint), err);
}

static hanami_error_t ctx_fetch_state(fetch_ctx *c, const char *owner,
    const char *const *relays, const hanami_signer_t *signer)
{
    hanami_nostr_ctx_t *nostr = NULL;
    hanami_error_t err = hanami_nostr_ctx_new(relays, signer, &nostr);
    if (err == HANAMI_OK)
        err = hanami_nostr_fetch_state_manifest(nostr, c->repo_id, owner, &c->state, c->head);
    hanami_nostr_ctx_free(nostr);
    if (err == HANAMI_ERR_NOT_FOUND) return HANAMI_OK;
    if (err != HANAMI_OK)
        fail(c, err, "hanami: kind-30618 state query for %s/%s failed: %s",
             owner, c->repo_id, hanami_strerror(err));
    return err;
}

/* Open .git/hanami/index.sqlite, import the manifest chain, check coverage. */
static hanami_error_t ctx_prepare(fetch_ctx *c, git_repository *repo)
{
    const char *gitdir = git_repository_path(repo);
    size_t n = strlen(gitdir) + 32;
    char *dir = malloc(n), *file = malloc(n);
    if (!dir || !file) { free(dir); free(file); return HANAMI_ERR_NOMEM; }
    snprintf(dir, n, "%shanami", gitdir);
    snprintf(file, n, "%shanami/index.sqlite", gitdir);
    hanami_error_t err = HANAMI_OK;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) err = HANAMI_ERR_IO;
    if (err == HANAMI_OK && !c->cache) err = hanami_index_open_file(&c->cache, file);
    free(dir); free(file);
    if (err != HANAMI_OK)
        return fail(c, err, "hanami: cannot open .git/hanami/index.sqlite cache"), err;

    bool has_refs = c->state && (c->state->ref_count > 0 || c->state->head);
    if (has_refs && !c->head[0])
        return fail(c, HANAMI_ERR_NOT_FOUND,
            "hanami: kind-30618 for %s has no [\"%s\", <sha256>] tag; it was likely "
            "republished by a client without hanami's manifest extension, so the "
            "Blossom objects cannot be located (try the announcement's Git clone URL)",
            c->repo_id, HANAMI_MANIFEST_TAG), HANAMI_ERR_NOT_FOUND;
    if (c->head[0]) {
        err = hanami_manifest_sync(c->client, c->cache, c->repo_id, c->head, NULL);
        if (err != HANAMI_OK)
            return fail(c, err, "hanami: manifest chain %s for %s is unavailable or invalid: %s",
                        c->head, c->repo_id, hanami_strerror(err)), err;
    }
    if (c->state && hanami_manifest_check_coverage(c->state, c->cache) != HANAMI_OK)
        return fail(c, HANAMI_ERR_INDEX,
            "hanami: kind-30618 for %s references objects absent from manifest %s",
            c->repo_id, c->head), HANAMI_ERR_INDEX;
    hanami_odb_backend_opts_t opts = { .index = c->cache, .client = c->client,
                                       .verify_on_read = true };
    if (!c->remote && (err = hanami_odb_backend_new(&c->remote, &opts)) != HANAMI_OK)
        return fail(c, err, "hanami: cannot create Blossom ODB backend"), err;
    return HANAMI_OK;
}

/* =========================================================================
 * Object closure: post-order DFS, so a locally present object always has
 * its whole closure present (an interrupted fetch never leaves a commit
 * whose tree or parents are missing).
 * ========================================================================= */

typedef struct {
    git_oid oid;
    void *data; size_t len; git_object_t type;
    git_oid *kids; size_t nkids, next;
} frame;

static int add_hex_kid(frame *f, size_t cap, const char *hex)
{
    if (f->nkids == cap) return -1;
    char buf[GIT_OID_SHA1_HEXSIZE + 1];
    memcpy(buf, hex, GIT_OID_SHA1_HEXSIZE); buf[GIT_OID_SHA1_HEXSIZE] = 0;
    return git_oid_fromstr(&f->kids[f->nkids++], buf);
}

static bool line_has(const char *p, const char *end, const char *key)
{
    size_t k = strlen(key);
    return (size_t)(end - p) >= k + GIT_OID_SHA1_HEXSIZE + 1 && memcmp(p, key, k) == 0 &&
           p[k + GIT_OID_SHA1_HEXSIZE] == '\n';
}

/* Children: commit -> tree, parents; tag -> object; tree -> entries
 * (gitlinks excluded); blob -> none. */
static int parse_kids(frame *f)
{
    const char *p = f->data, *end = p + f->len;
    size_t cap = 0;
    if (f->type == GIT_OBJECT_TREE) {
        for (const char *q = p; q < end; ) {
            const char *nul = memchr(q, 0, (size_t)(end - q));
            if (!nul || end - nul < 21) return -1;
            q = nul + 21; cap++;
        }
    } else if (f->type == GIT_OBJECT_COMMIT) {
        cap = 1 + f->len / (7 + GIT_OID_SHA1_HEXSIZE + 1);
    } else if (f->type == GIT_OBJECT_TAG) {
        cap = 1;
    }
    f->kids = cap ? calloc(cap, sizeof(git_oid)) : NULL;
    if (cap && !f->kids) return -1;
    if (f->type == GIT_OBJECT_COMMIT) {
        if (!line_has(p, end, "tree ") || add_hex_kid(f, cap, p + 5) < 0) return -1;
        p += 5 + GIT_OID_SHA1_HEXSIZE + 1;
        while (line_has(p, end, "parent ")) {
            if (add_hex_kid(f, cap, p + 7) < 0) return -1;
            p += 7 + GIT_OID_SHA1_HEXSIZE + 1;
        }
    } else if (f->type == GIT_OBJECT_TAG) {
        if (!line_has(p, end, "object ") || add_hex_kid(f, cap, p + 7) < 0) return -1;
    } else if (f->type == GIT_OBJECT_TREE) {
        while (p < end) {
            const char *nul = memchr(p, 0, (size_t)(end - p));
            bool gitlink = (size_t)(nul - p) > 7 && memcmp(p, "160000 ", 7) == 0;
            if (!gitlink) {
                if (f->nkids == cap) return -1;
                git_oid_fromraw(&f->kids[f->nkids++], (const unsigned char *)nul + 1);
            }
            p = nul + 21;
        }
    }
    return 0;
}

static void frame_dispose(git_odb_backend *remote, frame *f)
{
    if (f->data) git_odb_backend_data_free(remote, f->data);
    free(f->kids);
}

static int fetch_closure(fetch_ctx *c, git_odb *odb, const git_oid *root,
                         git_indexer_progress *stats)
{
    if (git_odb_exists(odb, root)) return 0;
    size_t n = 0, cap = 64;
    frame *stack = calloc(cap, sizeof(*stack));
    if (!stack) return -1;
    stack[n++].oid = *root;
    int rc = 0;
    while (rc == 0 && n) {
        frame *top = &stack[n - 1];
        char hex[GIT_OID_SHA1_HEXSIZE + 1];
        git_oid_tostr(hex, sizeof(hex), &top->oid);
        if (!top->data) {
            /* Verifying read: wire sha256 and Git OID are both checked. */
            if (c->remote->read(&top->data, &top->len, &top->type, c->remote, &top->oid) < 0) {
                const git_error *e = git_error_last();
                rc = fail(c, HANAMI_ERR_BLOSSOM, "hanami: cannot fetch object %s: %s",
                          hex, e && e->message ? e->message : "not in manifest");
                break;
            }
            if (parse_kids(top) < 0) {
                rc = fail(c, HANAMI_ERR_LIBGIT2, "hanami: malformed Git object %s", hex);
                break;
            }
        }
        if (top->next < top->nkids) {
            const git_oid *kid = &top->kids[top->next++];
            if (git_odb_exists(odb, kid)) continue;
            if (n == cap) {
                frame *grown = realloc(stack, cap * 2 * sizeof(*stack));
                if (!grown) { rc = -1; break; }
                stack = grown; cap *= 2;
                memset(stack + n, 0, (cap - n) * sizeof(*stack));
            }
            stack[n++].oid = *kid;
            continue;
        }
        git_oid written;
        if (git_odb_write(&written, odb, top->data, top->len, top->type) < 0 ||
            !git_oid_equal(&written, &top->oid)) {
            rc = fail(c, HANAMI_ERR_LIBGIT2, "hanami: cannot store object %s", hex);
            break;
        }
        if (stats) {
            stats->received_objects++; stats->indexed_objects++;
            stats->total_objects++; stats->received_bytes += top->len;
        }
        frame_dispose(c->remote, top);
        memset(top, 0, sizeof(*top));
        n--;
    }
    while (n) frame_dispose(c->remote, &stack[--n]);
    free(stack);
    return rc;
}

/* =========================================================================
 * Per-operation transport
 * ========================================================================= */

typedef struct {
    git_transport parent;
    fetch_ctx *ctx;
    git_remote_head **heads; size_t count;
    git_oid *wants; size_t want_count;
    bool connected;
} hanami_transport;

static int t_connect(git_transport *t, const char *url, int direction,
                     const git_remote_connect_options *opts)
{
    (void)url; (void)opts;
    if (direction != GIT_DIRECTION_FETCH) {
        git_error_set(GIT_ERROR_NET, "hanami: use hanami_push_to_blossom to push");
        return -1;
    }
    ((hanami_transport *)t)->connected = true;
    return 0;
}
static int t_options(git_transport *t, const git_remote_connect_options *opts)
{ (void)t; (void)opts; return 0; }
static int t_caps(unsigned int *caps, git_transport *t) { (void)t; *caps = 0; return 0; }
static int t_ls(const git_remote_head ***heads, size_t *count, git_transport *t)
{
    hanami_transport *ht = (hanami_transport *)t;
    *heads = (const git_remote_head **)ht->heads; *count = ht->count; return 0;
}
static int t_negotiate(git_transport *t, git_repository *r, const git_fetch_negotiation *n)
{
    (void)r;
    hanami_transport *ht = (hanami_transport *)t;
    free(ht->wants); ht->wants = NULL; ht->want_count = 0;
    if (!n->refs_len) return 0;
    ht->wants = calloc(n->refs_len, sizeof(git_oid));
    if (!ht->wants) return -1;
    for (size_t i = 0; i < n->refs_len; i++) ht->wants[ht->want_count++] = n->refs[i]->oid;
    return 0;
}
static int t_roots(git_oidarray *out, git_transport *t)
{ (void)t; out->ids = NULL; out->count = 0; return 0; }
static int t_download(git_transport *t, git_repository *repo, git_indexer_progress *stats)
{
    hanami_transport *ht = (hanami_transport *)t;
    if (stats) memset(stats, 0, sizeof(*stats));
    git_odb *odb = NULL;
    if (git_repository_odb(&odb, repo) < 0) return -1;
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < ht->want_count; i++)
        rc = fetch_closure(ht->ctx, odb, &ht->wants[i], stats);
    git_odb_free(odb);
    return rc;
}
static int t_push(git_transport *t, git_push *push) { (void)t; (void)push; return -1; }
static int t_connected(git_transport *t) { return ((hanami_transport *)t)->connected; }
static void t_cancel(git_transport *t) { (void)t; }
static int t_close(git_transport *t) { ((hanami_transport *)t)->connected = false; return 0; }
static void t_free(git_transport *t)
{
    hanami_transport *ht = (hanami_transport *)t;
    for (size_t i = 0; i < ht->count; i++) {
        free(ht->heads[i]->name); free(ht->heads[i]->symref_target); free(ht->heads[i]);
    }
    free(ht->heads); free(ht->wants); free(ht);
}

#define SYMREF_MAX_HOPS 8

/* Resolve a (possibly chained) symbolic target. *out = direct OID target, or
 * NULL when the chain ends at a missing ref (unborn). -1 for a cyclic or
 * over-long chain. */
static int resolve_target(const nip34_repo_state_t *s, const char *target, const char **out)
{
    for (int hop = 0; hop <= SYMREF_MAX_HOPS; hop++) {
        if (!target || strncmp(target, "ref: ", 5) != 0) { *out = target; return 0; }
        const char *next = NULL;
        for (size_t j = 0; j < s->ref_count && !next; j++)
            if (!strcmp(s->refs[j].refname, target + 5)) next = s->refs[j].target;
        if (!next) { *out = NULL; return 0; }
        target = next;
    }
    return -1;
}

static int transport_new(git_transport **out, git_remote *remote, void *payload)
{
    (void)remote;
    fetch_ctx *c = payload;
    hanami_transport *t = calloc(1, sizeof(*t));
    if (!t || git_transport_init(&t->parent, GIT_TRANSPORT_VERSION) < 0) { free(t); return -1; }
    t->ctx = c;
    t->parent.connect = t_connect; t->parent.set_connect_opts = t_options;
    t->parent.capabilities = t_caps; t->parent.ls = t_ls;
    t->parent.negotiate_fetch = t_negotiate; t->parent.shallow_roots = t_roots;
    t->parent.download_pack = t_download; t->parent.push = t_push;
    t->parent.is_connected = t_connected; t->parent.cancel = t_cancel;
    t->parent.close = t_close; t->parent.free = t_free;
    size_t total = c->state ? c->state->ref_count + 1 : 0;
    t->heads = calloc(total + 1, sizeof(*t->heads));
    if (!t->heads) { free(t); return -1; }
    for (size_t i = 0; i < total; i++) {
        const char *name = i == 0 ? "HEAD" : c->state->refs[i - 1].refname;
        const char *raw = i == 0 ? c->state->head : c->state->refs[i - 1].target;
        const char *target = NULL;
        if (resolve_target(c->state, raw, &target) < 0) {
            fail(c, HANAMI_ERR_NOSTR, "hanami: kind-30618 symbolic ref %s is cyclic or "
                 "longer than %d hops", name, SYMREF_MAX_HOPS);
            t_free(&t->parent);
            return -1;
        }
        if (!raw || !target) continue; /* unborn */
        if (!hanami_oid_hex_valid(target)) {
            fail(c, HANAMI_ERR_NOSTR, "hanami: kind-30618 ref %s has invalid target", name);
            t_free(&t->parent);
            return -1;
        }
        git_remote_head *h = calloc(1, sizeof(*h));
        if (!h) { t_free(&t->parent); return -1; }
        t->heads[t->count++] = h;
        const char *sym = strncmp(raw, "ref: ", 5) == 0 ? raw + 5 : NULL;
        h->name = strdup(name);
        h->symref_target = sym ? strdup(sym) : NULL;
        if (!h->name || (sym && !h->symref_target) || git_oid_fromstr(&h->oid, target) < 0) {
            t_free(&t->parent); return -1;
        }
    }
    *out = &t->parent;
    return 0;
}

/* =========================================================================
 * Clone
 * ========================================================================= */

static char *join_relays(const char *const *relays)
{
    size_t n = 1;
    for (size_t i = 0; relays && relays[i]; i++) n += strlen(relays[i]) + 1;
    char *out = calloc(1, n);
    for (size_t i = 0; out && relays && relays[i]; i++) {
        if (i) strcat(out, ",");
        strcat(out, relays[i]);
    }
    return out;
}

static int clone_repo_cb(git_repository **out, const char *path, int bare, void *payload)
{
    fetch_ctx *c = payload;
    if (git_repository_init(out, path, bare) < 0) return -1;
    git_config *config = NULL;
    int rc = git_repository_config(&config, *out);
    if (rc == 0) rc = git_config_set_string(config, "hanami.endpoint", c->endpoint);
    if (rc == 0 && c->relays_csv && *c->relays_csv)
        rc = git_config_set_string(config, "hanami.relays", c->relays_csv);
    git_config_free(config);
    if (rc < 0) fail(c, HANAMI_ERR_LIBGIT2, "hanami: cannot write clone configuration");
    else if (ctx_prepare(c, *out) != HANAMI_OK) rc = -1;
    if (rc < 0) {
        /* Close the cache before libgit2 removes the directory. */
        if (c->remote) { c->remote->free(c->remote); c->remote = NULL; }
        hanami_index_close(c->cache); c->cache = NULL;
        git_repository_free(*out); *out = NULL;
        return -1;
    }
    return 0;
}

static int remove_entry(const char *path, const struct stat *st, int kind, struct FTW *ftw)
{
    (void)st; (void)kind; (void)ftw;
    return remove(path);
}
static int remove_child(const char *path, const struct stat *st, int kind, struct FTW *ftw)
{
    (void)st; (void)kind;
    return ftw->level == 0 ? 0 : remove(path);
}

/* Destination ownership for failure cleanup: either this call created the
 * directory atomically (mkdir), or it was an existing empty directory. The
 * identity is re-checked before removal so a replaced path is never touched. */
typedef struct { bool claimed, created; struct stat id; } destination;

static hanami_error_t claim_destination(fetch_ctx *c, const char *path, destination *d)
{
    memset(d, 0, sizeof(*d));
    char *parents = strdup(path);
    if (!parents) return HANAMI_ERR_NOMEM;
    for (char *p = parents + 1; *p; p++)
        if (*p == '/') { *p = 0; (void)mkdir(parents, 0755); *p = '/'; }
    free(parents);
    if (mkdir(path, 0755) == 0) {
        d->created = true;
    } else if (errno != EEXIST) {
        return fail(c, HANAMI_ERR_IO, "hanami: cannot create %s", path), HANAMI_ERR_IO;
    }
    if (lstat(path, &d->id) != 0 || !S_ISDIR(d->id.st_mode))
        return fail(c, HANAMI_ERR_IO, "hanami: %s exists and is not a directory", path),
               HANAMI_ERR_IO;
    if (!d->created) {
        DIR *dir = opendir(path);
        struct dirent *e;
        bool empty = dir != NULL;
        while (dir && (e = readdir(dir)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { empty = false; break; }
        if (dir) closedir(dir);
        if (!empty)
            return fail(c, HANAMI_ERR_INVALID_ARG,
                        "hanami: %s exists and is not an empty directory", path),
                   HANAMI_ERR_INVALID_ARG;
    }
    d->claimed = true;
    return HANAMI_OK;
}

static void release_destination(const char *path, const destination *d)
{
    struct stat now;
    if (!d->claimed || lstat(path, &now) != 0 || !S_ISDIR(now.st_mode) ||
        now.st_dev != d->id.st_dev || now.st_ino != d->id.st_ino)
        return;
    nftw(path, d->created ? remove_entry : remove_child, 16, FTW_DEPTH | FTW_PHYS);
}

hanami_error_t hanami_clone_materialize(git_repository **out, const char *uri,
    const char *local_path, const char *owner, const char *repo_id,
    const char *endpoint, const char *const *relays,
    const hanami_signer_t *signer, const hanami_config_t *config)
{
    fetch_ctx c = { .repo_id = repo_id, .endpoint = endpoint };
    destination dest = {0};
    c.relays_csv = join_relays(relays);
    hanami_error_t err = c.relays_csv ? HANAMI_OK : HANAMI_ERR_NOMEM;
    if (err == HANAMI_OK) err = ctx_client(&c, signer, hanami_config_get_allow_insecure(config));
    if (err == HANAMI_OK) err = ctx_fetch_state(&c, owner, relays, signer);
    if (err == HANAMI_OK) err = claim_destination(&c, local_path, &dest);
    if (err == HANAMI_OK) {
        git_clone_options opts;
        git_clone_options_init(&opts, GIT_CLONE_OPTIONS_VERSION);
        opts.local = GIT_CLONE_NO_LOCAL;
        opts.repository_cb = clone_repo_cb; opts.repository_cb_payload = &c;
        opts.fetch_opts.callbacks.transport = transport_new;
        opts.fetch_opts.callbacks.payload = &c;
        if (git_clone(out, uri, local_path, &opts) < 0) {
            err = c.err != HANAMI_OK ? c.err : HANAMI_ERR_LIBGIT2;
            if (c.err != HANAMI_OK) git_error_set(GIT_ERROR_INVALID, "%s", c.msg);
            *out = NULL;
        }
    }
    if (err != HANAMI_OK && c.err != HANAMI_OK) git_error_set(GIT_ERROR_INVALID, "%s", c.msg);
    ctx_dispose(&c);
    /* Leave nothing behind that this clone created (libgit2 does not clean
     * up after a failing repository callback). */
    if (err != HANAMI_OK)
        release_destination(local_path, &dest);
    return err;
}

/* =========================================================================
 * Fetch
 * ========================================================================= */

static char *repo_config_string(git_repository *repo, const char *key)
{
    git_config *config = NULL;
    git_buf value = GIT_BUF_INIT;
    char *out = NULL;
    if (git_repository_config(&config, repo) == 0 &&
        git_config_get_string_buf(&value, config, key) == 0 && value.ptr && *value.ptr)
        out = strdup(value.ptr);
    git_buf_dispose(&value);
    git_config_free(config);
    return out;
}

static char **split_csv(const char *csv)
{
    size_t n = 2;
    for (const char *p = csv; *p; p++) n += *p == ',';
    char **list = calloc(n, sizeof(*list)), *dup = strdup(csv), *save = NULL;
    size_t i = 0;
    for (char *tok = dup && list ? strtok_r(dup, ",", &save) : NULL; tok;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        if (*tok && !(list[i++] = strdup(tok))) break;
    }
    free(dup);
    return list;
}

static void free_list(char **list)
{
    for (size_t i = 0; list && list[i]; i++) free(list[i]);
    free(list);
}

hanami_error_t hanami_fetch(git_repository *repo, const char *remote_name,
                            const hanami_signer_t *signer, const hanami_config_t *config)
{
    if (!repo) return HANAMI_ERR_INVALID_ARG;
    if (hanami_sealed_for_repo(repo) || hanami_repo_private(repo)) return HANAMI_ERR_AUTH;
    git_remote *remote = NULL;
    if (git_remote_lookup(&remote, repo, remote_name ? remote_name : "origin") < 0)
        return HANAMI_ERR_NOT_FOUND;
    char *owner = NULL, *repo_id = NULL, *endpoint = NULL;
    char **relay_list = NULL;
    hanami_error_t err = hanami_parse_nostr_uri(git_remote_url(remote), &owner, &repo_id);
    if (err != HANAMI_OK) { git_remote_free(remote); return err; }

    static const char *default_relays[] = { "wss://nos.lol", "wss://relay.nostr.band", NULL };
    const char *const *relays = default_relays;
    if (hanami_config_get_relay_count(config) > 0) {
        relays = hanami_config_get_relays(config);
    } else {
        char *csv = repo_config_string(repo, "hanami.relays");
        if (csv) { relay_list = split_csv(csv); free(csv); }
        if (relay_list && relay_list[0]) relays = (const char *const *)relay_list;
    }
    const char *configured = hanami_config_get_endpoint(config);
    endpoint = configured && *configured ? strdup(configured)
                                         : repo_config_string(repo, "hanami.endpoint");
    if (!endpoint) {
        hanami_nostr_ctx_t *nostr = NULL;
        err = hanami_nostr_ctx_new(relays, signer, &nostr);
        if (err == HANAMI_OK)
            err = hanami_select_blossom_endpoint(nostr, owner, repo_id, config, &endpoint);
        hanami_nostr_ctx_free(nostr);
    }

    fetch_ctx c = { .repo_id = repo_id, .endpoint = endpoint };
    if (err == HANAMI_OK) err = ctx_client(&c, signer, hanami_config_get_allow_insecure(config));
    if (err == HANAMI_OK) err = ctx_fetch_state(&c, owner, relays, signer);
    if (err == HANAMI_OK && c.state) err = ctx_prepare(&c, repo);
    if (err == HANAMI_OK && c.state) {
        git_fetch_options opts;
        git_fetch_options_init(&opts, GIT_FETCH_OPTIONS_VERSION);
        opts.callbacks.transport = transport_new;
        opts.callbacks.payload = &c;
        if (git_remote_fetch(remote, NULL, &opts, "hanami fetch") < 0) {
            err = c.err != HANAMI_OK ? c.err : HANAMI_ERR_LIBGIT2;
            if (c.err != HANAMI_OK) git_error_set(GIT_ERROR_INVALID, "%s", c.msg);
        }
    }
    ctx_dispose(&c);
    free_list(relay_list);
    free(endpoint); free(owner); free(repo_id);
    git_remote_free(remote);
    return err;
}
