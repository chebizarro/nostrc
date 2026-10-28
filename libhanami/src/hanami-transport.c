/*
 * hanami-transport.c - Blossom transport plugin for libgit2
 *
 * SPDX-License-Identifier: MIT
 *
 * Implements a custom git_transport for the "blossom://" URL scheme.
 * Fetch reads objects from Blossom servers, push uploads to Blossom.
 * Refs are advertised/updated via NIP-34 kind 30618 events.
 */

#include "hanami/hanami-transport.h"
#include "hanami-manifest.h"
#include "hanami-sealed.h"
#include "hanami/hanami-nostr.h"
#include <blossom/client.h>

#include <git2.h>
#include <git2/sys/transport.h>
#if defined(__has_include)
#  if __has_include(<git2/sys/errors.h>)
#    include <git2/sys/errors.h>
#  endif
#endif

#include <nip34.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * URL parsing
 * ========================================================================= */

#define BLOSSOM_PREFIX "blossom://"
#define BLOSSOM_PREFIX_LEN 10

hanami_error_t hanami_transport_parse_url(const char *url,
                                           char **endpoint,
                                           char **owner_pubkey,
                                           char **repo_id)
{
    if (!url || !endpoint || !owner_pubkey || !repo_id)
        return HANAMI_ERR_INVALID_ARG;

    *endpoint = NULL;
    *owner_pubkey = NULL;
    *repo_id = NULL;

    if (strncmp(url, BLOSSOM_PREFIX, BLOSSOM_PREFIX_LEN) != 0)
        return HANAMI_ERR_INVALID_ARG;

    const char *rest = url + BLOSSOM_PREFIX_LEN;

    /* Find host/endpoint: everything up to first '/' after prefix */
    const char *first_slash = strchr(rest, '/');
    if (!first_slash || first_slash == rest)
        return HANAMI_ERR_INVALID_ARG;

    /* Build endpoint as "https://<host>" */
    size_t host_len = (size_t)(first_slash - rest);
    size_t ep_len = 8 + host_len + 1; /* "https://" + host + NUL */
    *endpoint = malloc(ep_len);
    if (!*endpoint)
        return HANAMI_ERR_NOMEM;
    snprintf(*endpoint, ep_len, "https://%.*s", (int)host_len, rest);

    /* Owner pubkey: between first and second '/' */
    const char *after_host = first_slash + 1;
    const char *second_slash = strchr(after_host, '/');
    if (!second_slash || second_slash == after_host) {
        free(*endpoint);
        *endpoint = NULL;
        return HANAMI_ERR_INVALID_ARG;
    }

    size_t pk_len = (size_t)(second_slash - after_host);
    *owner_pubkey = strndup(after_host, pk_len);
    if (!*owner_pubkey) {
        free(*endpoint);
        *endpoint = NULL;
        return HANAMI_ERR_NOMEM;
    }

    /* Repo ID: everything after second '/' (strip trailing '/') */
    const char *repo_start = second_slash + 1;
    if (!*repo_start) {
        free(*endpoint);
        free(*owner_pubkey);
        *endpoint = NULL;
        *owner_pubkey = NULL;
        return HANAMI_ERR_INVALID_ARG;
    }

    size_t repo_len = strlen(repo_start);
    while (repo_len > 0 && repo_start[repo_len - 1] == '/')
        repo_len--;

    if (repo_len == 0) {
        free(*endpoint);
        free(*owner_pubkey);
        *endpoint = NULL;
        *owner_pubkey = NULL;
        return HANAMI_ERR_INVALID_ARG;
    }

    *repo_id = strndup(repo_start, repo_len);
    if (!*repo_id) {
        free(*endpoint);
        free(*owner_pubkey);
        *endpoint = NULL;
        *owner_pubkey = NULL;
        return HANAMI_ERR_NOMEM;
    }

    return HANAMI_OK;
}

/* =========================================================================
 * Transport struct
 * ========================================================================= */

typedef struct {
    git_transport parent;

    /* Parsed URL components */
    char *endpoint;
    char *owner_pubkey;
    char *repo_id;

    /* Dependencies (borrowed from global opts) */
    hanami_nostr_ctx_t *nostr_ctx;
    blossom_client_t *blossom_client;

    /* Connection state */
    bool connected;
    int direction;

    /* Repository reference (captured from callbacks) */
    git_repository *repo;

    /* Ref advertisement (cached from NIP-34 state) */
    git_remote_head **refs;
    size_t ref_count;
} hanami_transport_t;

/* Global transport options (set during register) */
static hanami_transport_opts_t s_transport_opts = {0};
static bool s_registered = false;

/* =========================================================================
 * Ref advertisement helpers
 * ========================================================================= */

static void free_refs(hanami_transport_t *t)
{
    if (t->refs) {
        for (size_t i = 0; i < t->ref_count; i++) {
            if (t->refs[i]) {
                free(t->refs[i]->name);
                free(t->refs[i]);
            }
        }
        free(t->refs);
        t->refs = NULL;
        t->ref_count = 0;
    }
}

static int load_refs_from_nostr(hanami_transport_t *t)
{
    if (!t->nostr_ctx)
        return -1;

    nip34_repo_state_t *state = NULL;
    hanami_error_t err = hanami_nostr_fetch_state(t->nostr_ctx,
                                                   t->repo_id,
                                                   t->owner_pubkey,
                                                   &state);

    if (err == HANAMI_ERR_NOT_FOUND || !state) {
        /* Empty repo — no refs to advertise */
        return 0;
    }
    if (err != HANAMI_OK)
        return -1;

    /* Count refs: state->refs + optional HEAD */
    size_t total = state->ref_count + (state->head ? 1 : 0);
    t->refs = calloc(total, sizeof(git_remote_head *));
    if (!t->refs) {
        nip34_repo_state_free(state);
        return -1;
    }

    size_t idx = 0;

    /* Add HEAD if present */
    if (state->head) {
        git_remote_head *h = calloc(1, sizeof(*h));
        if (h) {
            h->name = strdup("HEAD");
            /* If HEAD is symbolic (ref: xxx), try to resolve from state refs */
            if (strncmp(state->head, "ref: ", 5) == 0) {
                const char *target_name = state->head + 5;
                for (size_t i = 0; i < state->ref_count; i++) {
                    if (strcmp(state->refs[i].refname, target_name) == 0 &&
                        state->refs[i].target) {
                        git_oid_fromstr(&h->oid, state->refs[i].target);
                        break;
                    }
                }
            } else {
                git_oid_fromstr(&h->oid, state->head);
            }
            t->refs[idx++] = h;
        }
    }

    /* Add remaining refs */
    for (size_t i = 0; i < state->ref_count; i++) {
        const nip34_ref_t *ref = &state->refs[i];
        if (!ref->refname || !ref->target)
            continue;
        /* Skip symbolic refs for advertisement (HEAD is handled above) */
        if (strncmp(ref->target, "ref: ", 5) == 0)
            continue;

        git_remote_head *h = calloc(1, sizeof(*h));
        if (!h) continue;
        h->name = strdup(ref->refname);
        git_oid_fromstr(&h->oid, ref->target);
        t->refs[idx++] = h;
    }

    t->ref_count = idx;
    nip34_repo_state_free(state);
    return 0;
}

/* =========================================================================
 * Vtable: connect
 * ========================================================================= */

static int blossom_connect(git_transport *_transport, const char *url,
                           int direction,
                           const git_remote_connect_options *connect_opts)
{
    (void)connect_opts;
    hanami_transport_t *t = (hanami_transport_t *)_transport;

    /* Parse URL */
    hanami_error_t err = hanami_transport_parse_url(url,
                                                     &t->endpoint,
                                                     &t->owner_pubkey,
                                                     &t->repo_id);
    if (err != HANAMI_OK)
        return -1;

    t->direction = direction;
    t->nostr_ctx = s_transport_opts.nostr_ctx;
    t->blossom_client = s_transport_opts.blossom_client;

    /* Load ref advertisement from Nostr */
    load_refs_from_nostr(t);

    t->connected = true;
    return 0;
}

/* =========================================================================
 * Vtable: set_connect_opts
 * ========================================================================= */

static int blossom_set_connect_opts(git_transport *_transport,
                                    const git_remote_connect_options *opts)
{
    (void)_transport;
    (void)opts;
    return 0; /* Nothing to reconfigure */
}

/* =========================================================================
 * Vtable: capabilities
 * ========================================================================= */

static int blossom_capabilities(unsigned int *caps, git_transport *_transport)
{
    (void)_transport;
    /* We don't support thin packs or side-band */
    *caps = 0;
    return 0;
}

/* =========================================================================
 * Vtable: ls
 * ========================================================================= */

static int blossom_ls(const git_remote_head ***out, size_t *size,
                      git_transport *_transport)
{
    hanami_transport_t *t = (hanami_transport_t *)_transport;

    *out = (const git_remote_head **)t->refs;
    *size = t->ref_count;
    return 0;
}

/* =========================================================================
 * Vtable: negotiate_fetch
 * ========================================================================= */

static int blossom_negotiate_fetch(git_transport *_transport,
                                   git_repository *repo,
                                   const git_fetch_negotiation *fetch_data)
{
    hanami_transport_t *t = (hanami_transport_t *)_transport;
    (void)fetch_data;
    
    /* Store repo reference for later use (e.g., in push) */
    t->repo = repo;
    
    /* Blossom uses a lazy-fetch model: objects are downloaded on demand
     * when the ODB backend's read() is called. Negotiation is a no-op
     * because all wanted objects will be fetched individually by hash
     * from the Blossom server when accessed. */
    return 0;
}

/* =========================================================================
 * Vtable: shallow_roots
 * ========================================================================= */

static int blossom_shallow_roots(git_oidarray *out, git_transport *_transport)
{
    (void)_transport;
    out->ids = NULL;
    out->count = 0;
    return 0;
}

/* =========================================================================
 * Vtable: download_pack
 * ========================================================================= */

static int blossom_download_pack(git_transport *_transport,
                                 git_repository *repo,
                                 git_indexer_progress *stats)
{
    hanami_transport_t *t = (hanami_transport_t *)_transport;
    
    /* Store repo reference for later use (e.g., in push) */
    t->repo = repo;

    /* Blossom uses a lazy-fetch architecture: no pack download needed.
     * Objects are fetched individually via the ODB backend's read() when
     * libgit2 resolves commits/trees/blobs. Zero stats indicates no
     * packfile was transferred (objects arrive on demand). */
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
    return 0;
}

/* =========================================================================
 * Vtable: push
 * ========================================================================= */

static int blossom_push(git_transport *_transport, git_push *push)
{
    hanami_transport_t *t = (hanami_transport_t *)_transport;
    (void)push;

    /* A public transport must never project a private repository's refs. */
    if (hanami_repo_private(t->repo))
        return -1;
    if (!t->repo || !t->nostr_ctx || !t->blossom_client || !t->repo_id || !t->owner_pubkey) {
        git_error_set(GIT_ERROR_NET, "hanami: blossom:// push needs a repository, "
                      "Blossom client and Nostr context");
        return -1;
    }
    /* Same pipeline as hanami_push_to_blossom: objects, then the manifest
     * segment, then the tagged 30618; success needs relay OK true. */
    return hanami_push_repo_state(t->repo, t->blossom_client, t->nostr_ctx,
                                  t->repo_id, t->owner_pubkey) == HANAMI_OK ? 0 : -1;
}

/* =========================================================================
 * Vtable: is_connected, cancel, close, free
 * ========================================================================= */

static int blossom_is_connected(git_transport *_transport)
{
    hanami_transport_t *t = (hanami_transport_t *)_transport;
    return t->connected ? 1 : 0;
}

static void blossom_cancel(git_transport *_transport)
{
    hanami_transport_t *t = (hanami_transport_t *)_transport;
    t->connected = false;
}

static int blossom_close(git_transport *_transport)
{
    hanami_transport_t *t = (hanami_transport_t *)_transport;
    t->connected = false;
    return 0;
}

static void blossom_free(git_transport *_transport)
{
    hanami_transport_t *t = (hanami_transport_t *)_transport;

    free_refs(t);
    free(t->endpoint);
    free(t->owner_pubkey);
    free(t->repo_id);
    free(t);
}

/* =========================================================================
 * Transport factory (git_transport_cb)
 * ========================================================================= */

static int blossom_transport_cb(git_transport **out, git_remote *owner,
                                void *param)
{
    (void)param;

    hanami_transport_t *t = calloc(1, sizeof(*t));
    if (!t)
        return -1;
    /* Borrowed: the remote's repository outlives its transport. Push-only
     * connections never run the fetch callbacks that used to capture it. */
    t->repo = owner ? git_remote_owner(owner) : NULL;

    if (git_transport_init(&t->parent, GIT_TRANSPORT_VERSION) < 0) {
        free(t);
        return -1;
    }

    t->parent.connect          = blossom_connect;
    t->parent.set_connect_opts = blossom_set_connect_opts;
    t->parent.capabilities     = blossom_capabilities;
    t->parent.ls               = blossom_ls;
    t->parent.push             = blossom_push;
    t->parent.negotiate_fetch  = blossom_negotiate_fetch;
    t->parent.shallow_roots    = blossom_shallow_roots;
    t->parent.download_pack    = blossom_download_pack;
    t->parent.is_connected     = blossom_is_connected;
    t->parent.cancel           = blossom_cancel;
    t->parent.close            = blossom_close;
    t->parent.free             = blossom_free;

    *out = &t->parent;
    return 0;
}

/* =========================================================================
 * Registration
 * ========================================================================= */

hanami_error_t hanami_transport_register(const hanami_transport_opts_t *opts)
{
    if (!opts)
        return HANAMI_ERR_INVALID_ARG;

    if (s_registered)
        return HANAMI_OK; /* Already registered */

    s_transport_opts = *opts;

    int rc = git_transport_register("blossom://", blossom_transport_cb, NULL);
    if (rc < 0)
        return HANAMI_ERR_LIBGIT2;

    s_registered = true;
    return HANAMI_OK;
}

hanami_error_t hanami_transport_unregister(void)
{
    if (!s_registered)
        return HANAMI_OK;

    int rc = git_transport_unregister("blossom://");
    if (rc < 0)
        return HANAMI_ERR_LIBGIT2;

    memset(&s_transport_opts, 0, sizeof(s_transport_opts));
    s_registered = false;
    return HANAMI_OK;
}
