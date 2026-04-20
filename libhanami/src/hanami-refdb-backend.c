/*
 * hanami-refdb-backend.c - Custom git_refdb_backend for NIP-34 Nostr refs
 *
 * SPDX-License-Identifier: MIT
 *
 * Stores Git refs as NIP-34 kind 30618 repository state events. Each write
 * publishes an updated state event to Nostr relays. Reads query the locally
 * cached state (refreshed on init and on explicit refresh).
 */

#include "hanami/hanami-refdb-backend.h"
#include "hanami/hanami-nostr.h"

#include <git2/refs.h>
#include <git2/oid.h>
#include <git2/errors.h>
#include <git2/sys/refdb_backend.h>
#include <git2/sys/refs.h>

#include <nip34.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Internal ref entry stored in our cache
 * ========================================================================= */

typedef struct hanami_cached_ref {
    char *refname;              /* e.g. "refs/heads/main" or "HEAD" */
    bool is_symbolic;
    char *sym_target;           /* if symbolic: "refs/heads/main" */
    git_oid oid;                /* if direct: object id */
} hanami_cached_ref_t;

/* =========================================================================
 * Extended backend struct (embeds git_refdb_backend as first member)
 * ========================================================================= */

typedef struct {
    git_refdb_backend parent;   /* Must be first */

    hanami_nostr_ctx_t *nostr_ctx;  /* Borrowed */
    char *repo_id;                  /* Owned copy */
    char *owner_pubkey;             /* Owned copy */

    /* Cached state */
    hanami_cached_ref_t *refs;
    size_t ref_count;
    size_t ref_capacity;
} hanami_refdb_nostr_t;

/* =========================================================================
 * Cache management
 * ========================================================================= */

static void cached_ref_clear(hanami_cached_ref_t *r)
{
    free(r->refname);
    free(r->sym_target);
    r->refname = NULL;
    r->sym_target = NULL;
}

static void cache_clear(hanami_refdb_nostr_t *be)
{
    for (size_t i = 0; i < be->ref_count; i++)
        cached_ref_clear(&be->refs[i]);
    be->ref_count = 0;
}

static hanami_cached_ref_t *cache_find(hanami_refdb_nostr_t *be,
                                        const char *refname)
{
    for (size_t i = 0; i < be->ref_count; i++) {
        if (strcmp(be->refs[i].refname, refname) == 0)
            return &be->refs[i];
    }
    return NULL;
}

static int cache_ensure_capacity(hanami_refdb_nostr_t *be)
{
    if (be->ref_count < be->ref_capacity)
        return 0;

    size_t new_cap = be->ref_capacity ? be->ref_capacity * 2 : 16;
    hanami_cached_ref_t *new_refs = realloc(be->refs,
                                            new_cap * sizeof(hanami_cached_ref_t));
    if (!new_refs)
        return -1;

    /* Zero new entries */
    memset(new_refs + be->ref_capacity, 0,
           (new_cap - be->ref_capacity) * sizeof(hanami_cached_ref_t));

    be->refs = new_refs;
    be->ref_capacity = new_cap;
    return 0;
}

/**
 * Add or update a ref in the cache. Returns 0 on success, -1 on error.
 * For direct refs: sym_target=NULL, oid is set.
 * For symbolic refs: sym_target is set, oid is zeroed.
 */
static int cache_set(hanami_refdb_nostr_t *be, const char *refname,
                     bool is_symbolic, const char *sym_target,
                     const git_oid *oid)
{
    hanami_cached_ref_t *existing = cache_find(be, refname);

    if (existing) {
        /* Update in-place */
        free(existing->sym_target);
        existing->sym_target = NULL;
        existing->is_symbolic = is_symbolic;
        if (is_symbolic && sym_target)
            existing->sym_target = strdup(sym_target);
        if (oid)
            git_oid_cpy(&existing->oid, oid);
        else
            memset(&existing->oid, 0, sizeof(existing->oid));
        return 0;
    }

    /* Add new entry */
    if (cache_ensure_capacity(be) < 0)
        return -1;

    hanami_cached_ref_t *r = &be->refs[be->ref_count];
    memset(r, 0, sizeof(*r));
    r->refname = strdup(refname);
    if (!r->refname)
        return -1;

    r->is_symbolic = is_symbolic;
    if (is_symbolic && sym_target)
        r->sym_target = strdup(sym_target);
    if (oid)
        git_oid_cpy(&r->oid, oid);

    be->ref_count++;
    return 0;
}

static int cache_remove(hanami_refdb_nostr_t *be, const char *refname)
{
    for (size_t i = 0; i < be->ref_count; i++) {
        if (strcmp(be->refs[i].refname, refname) == 0) {
            cached_ref_clear(&be->refs[i]);
            /* Swap with last */
            if (i < be->ref_count - 1)
                be->refs[i] = be->refs[be->ref_count - 1];
            memset(&be->refs[be->ref_count - 1], 0, sizeof(hanami_cached_ref_t));
            be->ref_count--;
            return 0;
        }
    }
    return GIT_ENOTFOUND;
}

/* =========================================================================
 * State sync: load from NIP-34 kind 30618
 * ========================================================================= */

static int load_state_from_nostr(hanami_refdb_nostr_t *be)
{
    nip34_repo_state_t *state = NULL;
    hanami_error_t err = hanami_nostr_fetch_state(be->nostr_ctx,
                                                   be->repo_id,
                                                   be->owner_pubkey,
                                                   &state);
    if (err == HANAMI_ERR_NOT_FOUND)
        return 0; /* No state yet — empty repo, not an error */
    if (err != HANAMI_OK || !state)
        return 0; /* Network error — keep whatever cache we have */

    cache_clear(be);

    /* Import refs from state */
    for (size_t i = 0; i < state->ref_count; i++) {
        const nip34_ref_t *ref = &state->refs[i];
        if (!ref->refname || !ref->target)
            continue;

        /* Check if target is symbolic: starts with "ref: " */
        if (strncmp(ref->target, "ref: ", 5) == 0) {
            cache_set(be, ref->refname, true, ref->target + 5, NULL);
        } else {
            /* Direct ref — parse hex OID */
            git_oid oid;
            if (git_oid_fromstr(&oid, ref->target) == 0)
                cache_set(be, ref->refname, false, NULL, &oid);
        }
    }

    /* HEAD */
    if (state->head) {
        if (strncmp(state->head, "ref: ", 5) == 0) {
            cache_set(be, "HEAD", true, state->head + 5, NULL);
        } else {
            git_oid oid;
            if (git_oid_fromstr(&oid, state->head) == 0)
                cache_set(be, "HEAD", false, NULL, &oid);
        }
    }

    nip34_repo_state_free(state);
    return 0;
}

/* =========================================================================
 * Publish state: create and send kind 30618
 * ========================================================================= */

static int publish_state(hanami_refdb_nostr_t *be)
{
    /* Build nip34_ref_t array from cache, excluding HEAD */
    size_t max_refs = be->ref_count;
    nip34_ref_t *refs = calloc(max_refs, sizeof(nip34_ref_t));
    if (!refs && max_refs > 0)
        return -1;

    size_t ref_count = 0;
    const char *head_value = NULL;

    for (size_t i = 0; i < be->ref_count; i++) {
        hanami_cached_ref_t *cr = &be->refs[i];
        if (!cr->refname)
            continue;

        if (strcmp(cr->refname, "HEAD") == 0) {
            /* HEAD handled separately */
            if (cr->is_symbolic && cr->sym_target) {
                /* We'll construct "ref: X" for the head param */
                head_value = cr->sym_target; /* will be used with "ref: " prefix below */
            }
            continue;
        }

        nip34_ref_t *r = &refs[ref_count];
        r->refname = cr->refname;  /* Borrowed, nip34 only reads */
        if (cr->is_symbolic && cr->sym_target) {
            /* Encode as "ref: <target>" */
            size_t len = 5 + strlen(cr->sym_target) + 1;
            char *buf = malloc(len);
            if (buf) {
                snprintf(buf, len, "ref: %s", cr->sym_target);
                r->target = buf;
            }
        } else {
            char *hex = malloc(65);
            if (hex) {
                git_oid_tostr(hex, 65, &cr->oid);
                r->target = hex;
            }
        }
        ref_count++;
    }

    /* Build head string */
    char *head_str = NULL;
    if (head_value) {
        size_t hlen = 5 + strlen(head_value) + 1;
        head_str = malloc(hlen);
        if (head_str)
            snprintf(head_str, hlen, "ref: %s", head_value);
    }

    hanami_error_t err = hanami_nostr_publish_state(be->nostr_ctx,
                                                     be->repo_id,
                                                     refs, ref_count,
                                                     head_str);

    /* Cleanup allocated target strings */
    for (size_t i = 0; i < ref_count; i++)
        free(refs[i].target);
    free(refs);
    free(head_str);

    return (err == HANAMI_OK) ? 0 : -1;
}

/* =========================================================================
 * Vtable: exists
 * ========================================================================= */

static int nostr_refdb_exists(int *exists, git_refdb_backend *_backend,
                              const char *ref_name)
{
    hanami_refdb_nostr_t *be = (hanami_refdb_nostr_t *)_backend;
    *exists = (cache_find(be, ref_name) != NULL) ? 1 : 0;
    return 0;
}

/* =========================================================================
 * Vtable: lookup
 * ========================================================================= */

static int nostr_refdb_lookup(git_reference **out, git_refdb_backend *_backend,
                              const char *ref_name)
{
    hanami_refdb_nostr_t *be = (hanami_refdb_nostr_t *)_backend;

    hanami_cached_ref_t *cr = cache_find(be, ref_name);
    if (!cr)
        return GIT_ENOTFOUND;

    if (cr->is_symbolic) {
        *out = git_reference__alloc_symbolic(cr->refname, cr->sym_target);
    } else {
        *out = git_reference__alloc(cr->refname, &cr->oid, NULL);
    }

    return *out ? 0 : -1;
}

/* =========================================================================
 * Vtable: iterator
 * ========================================================================= */

typedef struct {
    git_reference_iterator parent;
    hanami_refdb_nostr_t *backend;
    size_t index;
    char *glob;
} hanami_ref_iterator_t;

/* Simple glob matching (supports '*' wildcard only) */
static bool simple_glob_match(const char *pattern, const char *str)
{
    if (!pattern || !*pattern)
        return true;

    const char *p = pattern;
    const char *s = str;

    while (*p && *s) {
        if (*p == '*') {
            p++;
            if (!*p) return true; /* trailing * matches all */
            while (*s) {
                if (simple_glob_match(p, s))
                    return true;
                s++;
            }
            return false;
        }
        if (*p != *s)
            return false;
        p++;
        s++;
    }

    while (*p == '*') p++;
    return (*p == '\0' && *s == '\0');
}

static int nostr_ref_iterator_next(git_reference **out,
                                   git_reference_iterator *_iter)
{
    hanami_ref_iterator_t *iter = (hanami_ref_iterator_t *)_iter;
    hanami_refdb_nostr_t *be = iter->backend;

    while (iter->index < be->ref_count) {
        hanami_cached_ref_t *cr = &be->refs[iter->index];
        iter->index++;

        if (!cr->refname)
            continue;

        /* Apply glob filter */
        if (iter->glob && !simple_glob_match(iter->glob, cr->refname))
            continue;

        if (cr->is_symbolic) {
            *out = git_reference__alloc_symbolic(cr->refname, cr->sym_target);
        } else {
            *out = git_reference__alloc(cr->refname, &cr->oid, NULL);
        }

        return *out ? 0 : -1;
    }

    return GIT_ITEROVER;
}

static int nostr_ref_iterator_next_name(const char **out,
                                        git_reference_iterator *_iter)
{
    hanami_ref_iterator_t *iter = (hanami_ref_iterator_t *)_iter;
    hanami_refdb_nostr_t *be = iter->backend;

    while (iter->index < be->ref_count) {
        hanami_cached_ref_t *cr = &be->refs[iter->index];
        iter->index++;

        if (!cr->refname)
            continue;

        if (iter->glob && !simple_glob_match(iter->glob, cr->refname))
            continue;

        *out = cr->refname;
        return 0;
    }

    return GIT_ITEROVER;
}

static void nostr_ref_iterator_free(git_reference_iterator *_iter)
{
    hanami_ref_iterator_t *iter = (hanami_ref_iterator_t *)_iter;
    free(iter->glob);
    free(iter);
}

static int nostr_refdb_iterator(git_reference_iterator **out,
                                struct git_refdb_backend *_backend,
                                const char *glob)
{
    hanami_refdb_nostr_t *be = (hanami_refdb_nostr_t *)_backend;

    hanami_ref_iterator_t *iter = calloc(1, sizeof(*iter));
    if (!iter)
        return -1;

    iter->parent.next = nostr_ref_iterator_next;
    iter->parent.next_name = nostr_ref_iterator_next_name;
    iter->parent.free = nostr_ref_iterator_free;
    iter->backend = be;
    iter->index = 0;

    if (glob) {
        iter->glob = strdup(glob);
        if (!iter->glob) {
            free(iter);
            return -1;
        }
    }

    *out = (git_reference_iterator *)iter;
    return 0;
}

/* =========================================================================
 * Vtable: write
 * ========================================================================= */

static int nostr_refdb_write(git_refdb_backend *_backend,
                             const git_reference *ref, int force,
                             const git_signature *who, const char *message,
                             const git_oid *old, const char *old_target)
{
    (void)who;
    (void)message;
    hanami_refdb_nostr_t *be = (hanami_refdb_nostr_t *)_backend;

    const char *name = git_reference_name(ref);
    if (!name)
        return -1;

    /* Check if ref already exists and force is not set */
    hanami_cached_ref_t *existing = cache_find(be, name);
    if (existing && !force) {
        /* Verify old value if provided */
        if (old) {
            if (existing->is_symbolic || git_oid_cmp(&existing->oid, old) != 0)
                return GIT_EMODIFIED;
        }
        if (old_target) {
            if (!existing->is_symbolic ||
                !existing->sym_target ||
                strcmp(existing->sym_target, old_target) != 0)
                return GIT_EMODIFIED;
        }
        /* If both old and old_target are NULL, the ref should not exist */
        if (!old && !old_target)
            return GIT_EEXISTS;
    }

    /* Update cache */
    git_reference_t type = git_reference_type(ref);
    if (type == GIT_REFERENCE_SYMBOLIC) {
        const char *target = git_reference_symbolic_target(ref);
        cache_set(be, name, true, target, NULL);
    } else {
        const git_oid *oid = git_reference_target(ref);
        cache_set(be, name, false, NULL, oid);
    }

    /* Publish updated state to relays */
    return publish_state(be);
}

/* =========================================================================
 * Vtable: rename
 * ========================================================================= */

static int nostr_refdb_rename(git_reference **out, git_refdb_backend *_backend,
                              const char *old_name, const char *new_name,
                              int force, const git_signature *who,
                              const char *message)
{
    (void)who;
    (void)message;
    hanami_refdb_nostr_t *be = (hanami_refdb_nostr_t *)_backend;

    hanami_cached_ref_t *old_ref = cache_find(be, old_name);
    if (!old_ref)
        return GIT_ENOTFOUND;

    if (!force && cache_find(be, new_name))
        return GIT_EEXISTS;

    /* Copy old ref data, add new, remove old */
    bool is_sym = old_ref->is_symbolic;
    char *sym_target = old_ref->sym_target ? strdup(old_ref->sym_target) : NULL;
    git_oid oid;
    git_oid_cpy(&oid, &old_ref->oid);

    cache_set(be, new_name, is_sym, sym_target, &oid);
    cache_remove(be, old_name);
    free(sym_target);

    /* Create output reference */
    hanami_cached_ref_t *new_ref = cache_find(be, new_name);
    if (!new_ref)
        return -1;

    if (new_ref->is_symbolic) {
        *out = git_reference__alloc_symbolic(new_ref->refname, new_ref->sym_target);
    } else {
        *out = git_reference__alloc(new_ref->refname, &new_ref->oid, NULL);
    }

    if (!*out)
        return -1;

    return publish_state(be);
}

/* =========================================================================
 * Vtable: del
 * ========================================================================= */

static int nostr_refdb_del(git_refdb_backend *_backend, const char *ref_name,
                           const git_oid *old_id, const char *old_target)
{
    hanami_refdb_nostr_t *be = (hanami_refdb_nostr_t *)_backend;

    hanami_cached_ref_t *existing = cache_find(be, ref_name);
    if (!existing)
        return GIT_ENOTFOUND;

    /* Verify old value if provided */
    if (old_id && (!existing->is_symbolic &&
                   git_oid_cmp(&existing->oid, old_id) != 0))
        return GIT_EMODIFIED;
    if (old_target && existing->is_symbolic &&
        existing->sym_target &&
        strcmp(existing->sym_target, old_target) != 0)
        return GIT_EMODIFIED;

    cache_remove(be, ref_name);
    return publish_state(be);
}

/* =========================================================================
 * Vtable: reflog (not supported — Nostr has no reflog concept)
 * ========================================================================= */

static int nostr_refdb_has_log(git_refdb_backend *_backend, const char *refname)
{
    (void)_backend;
    (void)refname;
    return 0; /* No reflog */
}

static int nostr_refdb_ensure_log(git_refdb_backend *_backend,
                                  const char *refname)
{
    (void)_backend;
    (void)refname;
    return 0; /* No-op: we don't maintain reflogs */
}

static int nostr_refdb_reflog_read(git_reflog **out, git_refdb_backend *_backend,
                                   const char *name)
{
    (void)out;
    (void)_backend;
    (void)name;
    return GIT_ENOTFOUND;
}

static int nostr_refdb_reflog_write(git_refdb_backend *_backend,
                                    git_reflog *reflog)
{
    (void)_backend;
    (void)reflog;
    return 0; /* Silently succeed — we don't persist reflogs */
}

static int nostr_refdb_reflog_rename(git_refdb_backend *_backend,
                                     const char *old_name,
                                     const char *new_name)
{
    (void)_backend;
    (void)old_name;
    (void)new_name;
    return 0;
}

static int nostr_refdb_reflog_delete(git_refdb_backend *_backend,
                                     const char *name)
{
    (void)_backend;
    (void)name;
    return 0;
}

/* =========================================================================
 * Vtable: free
 * ========================================================================= */

static void nostr_refdb_free(git_refdb_backend *_backend)
{
    hanami_refdb_nostr_t *be = (hanami_refdb_nostr_t *)_backend;

    cache_clear(be);
    free(be->refs);
    free(be->repo_id);
    free(be->owner_pubkey);
    free(be);
}

/* =========================================================================
 * Constructor
 * ========================================================================= */

hanami_error_t hanami_refdb_backend_new(git_refdb_backend **out,
                                        const hanami_refdb_backend_opts_t *opts)
{
    if (!out || !opts)
        return HANAMI_ERR_INVALID_ARG;
    if (!opts->nostr_ctx || !opts->repo_id || !opts->owner_pubkey)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    hanami_refdb_nostr_t *be = calloc(1, sizeof(*be));
    if (!be)
        return HANAMI_ERR_NOMEM;

    if (git_refdb_init_backend(&be->parent, GIT_REFDB_BACKEND_VERSION) < 0) {
        free(be);
        return HANAMI_ERR_LIBGIT2;
    }

    be->nostr_ctx = opts->nostr_ctx;
    be->repo_id = strdup(opts->repo_id);
    be->owner_pubkey = strdup(opts->owner_pubkey);
    if (!be->repo_id || !be->owner_pubkey) {
        free(be->repo_id);
        free(be->owner_pubkey);
        free(be);
        return HANAMI_ERR_NOMEM;
    }

    /* Wire up vtable */
    be->parent.exists       = nostr_refdb_exists;
    be->parent.lookup       = nostr_refdb_lookup;
    be->parent.iterator     = nostr_refdb_iterator;
    be->parent.write        = nostr_refdb_write;
    be->parent.rename       = nostr_refdb_rename;
    be->parent.del          = nostr_refdb_del;
    be->parent.has_log      = nostr_refdb_has_log;
    be->parent.ensure_log   = nostr_refdb_ensure_log;
    be->parent.free         = nostr_refdb_free;
    be->parent.reflog_read  = nostr_refdb_reflog_read;
    be->parent.reflog_write = nostr_refdb_reflog_write;
    be->parent.reflog_rename = nostr_refdb_reflog_rename;
    be->parent.reflog_delete = nostr_refdb_reflog_delete;

    /* Try to load initial state from relays (best-effort) */
    load_state_from_nostr(be);

    *out = &be->parent;
    return HANAMI_OK;
}
