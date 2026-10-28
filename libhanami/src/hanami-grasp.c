/*
 * hanami-grasp.c - GRASP server compatibility layer
 *
 * SPDX-License-Identifier: MIT
 *
 * Implements GRASP detection, URL parsing, and the publish-then-push
 * workflow for Git relays that authorize via NIP-34 kind 30618 events.
 */

#include "hanami/hanami-grasp.h"
#include "hanami-sealed.h"
#include "hanami/hanami-nostr.h"
#include "hanami-nostr-internal.h"
#include <stdint.h>

#include <git2.h>
#include <nip34.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* =========================================================================
 * Bech32 npub decoding
 * ========================================================================= */

static const char BECH32_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static int bech32_char_to_val(char c)
{
    const char *p = strchr(BECH32_CHARSET, c);
    return p ? (int)(p - BECH32_CHARSET) : -1;
}

/**
 * Decode an npub bech32 string to a 32-byte pubkey.
 * Returns allocated 64-char hex string or NULL on error.
 */
static char *npub_to_hex(const char *npub)
{
    if (!npub || strncmp(npub, "npub1", 5) != 0)
        return NULL;

    size_t len = strlen(npub);
    if (len < 59) /* npub1 + 53 chars minimum for 32 bytes */
        return NULL;

    /* Decode bech32 data portion (after '1' separator) */
    const char *data_part = npub + 5; /* skip "npub1" */
    size_t data_len = len - 5;

    /* Decode 5-bit values */
    uint8_t values[64];
    if (data_len > 64)
        return NULL;

    for (size_t i = 0; i < data_len; i++) {
        int v = bech32_char_to_val(data_part[i]);
        if (v < 0) return NULL;
        values[i] = (uint8_t)v;
    }

    /* Skip 6-byte checksum at end */
    if (data_len < 6)
        return NULL;
    size_t payload_5bit_len = data_len - 6;

    /* Convert from 5-bit to 8-bit */
    uint8_t pubkey[32];
    size_t out_idx = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < payload_5bit_len && out_idx < 32; i++) {
        acc = (acc << 5) | values[i];
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            pubkey[out_idx++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }

    /* Validate: npub must decode to exactly 32 bytes (pubkey length) */
    if (out_idx != 32)
        return NULL;

    /* Note: This implementation does not verify the bech32 checksum.
     * For production use, consider using nostr_nip19_decode_npub from
     * the nip19 library which includes full checksum validation. */

    /* Convert to hex */
    static const char hex[] = "0123456789abcdef";
    char *result = malloc(65);
    if (!result) return NULL;

    for (int i = 0; i < 32; i++) {
        result[i * 2]     = hex[(pubkey[i] >> 4) & 0xf];
        result[i * 2 + 1] = hex[pubkey[i] & 0xf];
    }
    result[64] = '\0';
    return result;
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * Extract the hostname from a URL.
 * Handles http(s)://<host>/... and ws(s)://<host>/...
 * Returns allocated string or NULL.
 */
static char *extract_host(const char *url)
{
    if (!url)
        return NULL;

    const char *start = NULL;

    if (strncmp(url, "https://", 8) == 0)
        start = url + 8;
    else if (strncmp(url, "http://", 7) == 0)
        start = url + 7;
    else if (strncmp(url, "wss://", 6) == 0)
        start = url + 6;
    else if (strncmp(url, "ws://", 5) == 0)
        start = url + 5;
    else
        return NULL;

    /* Host ends at '/', ':', or end of string */
    const char *end = start;
    while (*end && *end != '/' && *end != ':')
        end++;

    if (end == start)
        return NULL;

    return strndup(start, (size_t)(end - start));
}

/**
 * Case-insensitive host comparison.
 */
static bool hosts_equal(const char *a, const char *b)
{
    if (!a || !b)
        return false;

    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * Check if a string starts with "npub1" (bech32 npub).
 */
static bool is_npub(const char *s)
{
    return s && strncmp(s, "npub1", 5) == 0 && strlen(s) >= 59;
}

/**
 * Check if a string ends with ".git".
 */
static bool ends_with_git(const char *s)
{
    if (!s)
        return false;
    size_t len = strlen(s);
    return len >= 4 && strcmp(s + len - 4, ".git") == 0;
}



/* =========================================================================
 * GRASP info lifecycle
 * ========================================================================= */

void hanami_grasp_info_free(hanami_grasp_info_t *info)
{
    if (!info)
        return;
    free(info->host);
    free(info->npub);
    free(info->pubkey);
    free(info->repo_name);
    free(info->clone_url);
    free(info->relay_url);
    free(info);
}

/* =========================================================================
 * GRASP server detection
 * ========================================================================= */

bool hanami_is_grasp_server(const char *clone_url,
                            const char *const *relay_urls,
                            size_t relay_count)
{
    if (!clone_url || !relay_urls || relay_count == 0)
        return false;

    /* Check clone URL matches GRASP pattern: http(s)://<host>/<npub>/<repo>.git */
    if (strncmp(clone_url, "https://", 8) != 0 &&
        strncmp(clone_url, "http://", 7) != 0)
        return false;

    char *clone_host = extract_host(clone_url);
    if (!clone_host)
        return false;

    /* Find path portion after host */
    const char *scheme_end = strstr(clone_url, "://");
    const char *path_start = strchr(scheme_end + 3, '/');
    if (!path_start || !*(path_start + 1)) {
        free(clone_host);
        return false;
    }

    /* Parse path: /<npub>/<repo>.git */
    const char *npub_start = path_start + 1;
    const char *slash2 = strchr(npub_start, '/');
    if (!slash2) {
        free(clone_host);
        return false;
    }

    /* Check npub component */
    size_t npub_len = (size_t)(slash2 - npub_start);
    char *npub_part = strndup(npub_start, npub_len);
    if (!npub_part) {
        free(clone_host);
        return false;
    }

    if (!is_npub(npub_part)) {
        free(npub_part);
        free(clone_host);
        return false;
    }
    free(npub_part);

    /* Check repo component ends with .git */
    const char *repo_start = slash2 + 1;
    if (!ends_with_git(repo_start)) {
        free(clone_host);
        return false;
    }

    /* Check if any relay URL matches the same host */
    bool found_relay = false;
    for (size_t i = 0; i < relay_count && relay_urls[i]; i++) {
        char *relay_host = extract_host(relay_urls[i]);
        if (relay_host && hosts_equal(clone_host, relay_host)) {
            found_relay = true;
            free(relay_host);
            break;
        }
        free(relay_host);
    }

    free(clone_host);
    return found_relay;
}

/* =========================================================================
 * Clone URL parsing
 * ========================================================================= */

hanami_error_t hanami_grasp_parse_clone_url(const char *clone_url,
                                             hanami_grasp_info_t **out)
{
    if (!clone_url || !out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    /* Must be http(s):// */
    bool tls = false;
    const char *after_scheme = NULL;
    if (strncmp(clone_url, "https://", 8) == 0) {
        tls = true;
        after_scheme = clone_url + 8;
    } else if (strncmp(clone_url, "http://", 7) == 0) {
        after_scheme = clone_url + 7;
    } else {
        return HANAMI_ERR_INVALID_ARG;
    }

    /* Extract host (up to first '/') */
    const char *first_slash = strchr(after_scheme, '/');
    if (!first_slash || first_slash == after_scheme)
        return HANAMI_ERR_INVALID_ARG;

    size_t host_len = (size_t)(first_slash - after_scheme);

    /* Parse path: /<npub>/<repo>.git */
    const char *npub_start = first_slash + 1;
    const char *second_slash = strchr(npub_start, '/');
    if (!second_slash || second_slash == npub_start)
        return HANAMI_ERR_INVALID_ARG;

    size_t npub_len = (size_t)(second_slash - npub_start);
    char *npub = strndup(npub_start, npub_len);
    if (!npub)
        return HANAMI_ERR_NOMEM;

    if (!is_npub(npub)) {
        free(npub);
        return HANAMI_ERR_INVALID_ARG;
    }

    /* Repo name (strip .git suffix) */
    const char *repo_start = second_slash + 1;
    if (!*repo_start || !ends_with_git(repo_start)) {
        free(npub);
        return HANAMI_ERR_INVALID_ARG;
    }

    size_t repo_full_len = strlen(repo_start);
    size_t repo_name_len = repo_full_len - 4; /* strip ".git" */
    if (repo_name_len == 0) {
        free(npub);
        return HANAMI_ERR_INVALID_ARG;
    }

    /* Allocate info struct */
    hanami_grasp_info_t *info = calloc(1, sizeof(*info));
    if (!info) {
        free(npub);
        return HANAMI_ERR_NOMEM;
    }

    info->host = strndup(after_scheme, host_len);
    info->npub = npub;
    info->pubkey = npub_to_hex(npub); /* bech32 decode npub → hex pubkey */
    info->repo_name = strndup(repo_start, repo_name_len);
    info->clone_url = strdup(clone_url);
    info->uses_tls = tls;

    /* Build relay URL */
    size_t relay_len = (tls ? 6 : 5) + host_len + 2; /* "wss://" + host + "/" + NUL */
    info->relay_url = malloc(relay_len);
    if (info->relay_url) {
        snprintf(info->relay_url, relay_len, "%s%.*s/",
                 tls ? "wss://" : "ws://", (int)host_len, after_scheme);
    }

    if (!info->host || !info->repo_name || !info->clone_url || !info->relay_url) {
        hanami_grasp_info_free(info);
        return HANAMI_ERR_NOMEM;
    }

    *out = info;
    return HANAMI_OK;
}

/* =========================================================================
 * URL builders
 * ========================================================================= */

hanami_error_t hanami_grasp_build_clone_url(const char *host,
                                             const char *npub,
                                             const char *repo_name,
                                             bool use_tls,
                                             char **out)
{
    if (!host || !npub || !repo_name || !out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    /* Format: http(s)://<host>/<npub>/<repo>.git */
    const char *scheme = use_tls ? "https://" : "http://";
    size_t len = strlen(scheme) + strlen(host) + 1 + strlen(npub) + 1 +
                 strlen(repo_name) + 4 + 1; /* .git + NUL */

    char *url = malloc(len);
    if (!url)
        return HANAMI_ERR_NOMEM;

    snprintf(url, len, "%s%s/%s/%s.git", scheme, host, npub, repo_name);
    *out = url;
    return HANAMI_OK;
}

hanami_error_t hanami_grasp_build_relay_url(const char *host,
                                             bool use_tls,
                                             char **out)
{
    if (!host || !out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    const char *scheme = use_tls ? "wss://" : "ws://";
    size_t len = strlen(scheme) + strlen(host) + 1; /* + NUL */

    char *url = malloc(len);
    if (!url)
        return HANAMI_ERR_NOMEM;

    snprintf(url, len, "%s%s", scheme, host);
    *out = url;
    return HANAMI_OK;
}

/* =========================================================================
 * PR refname helper
 * ========================================================================= */

hanami_error_t hanami_grasp_pr_refname(const char *event_id, char **out)
{
    if (!event_id || !out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    /* Format: refs/nostr/<event_id> */
    size_t len = 12 + strlen(event_id) + 1; /* "refs/nostr/" + id + NUL */
    char *ref = malloc(len);
    if (!ref)
        return HANAMI_ERR_NOMEM;

    snprintf(ref, len, "refs/nostr/%s", event_id);
    *out = ref;
    return HANAMI_OK;
}

/* =========================================================================
 * GRASP push workflow
 * ========================================================================= */

static void free_ref_array(nip34_ref_t *refs, size_t count);

static int append_push_ref(nip34_ref_t **refs, size_t *count, git_strarray *specs,
    const char *source, const char *destination, const git_oid *oid, bool force)
{
    char hex[GIT_OID_HEXSZ + 1] = {0};
    if (oid) git_oid_tostr(hex, sizeof(hex), oid);
    for (size_t i = 0; i < *count; i++)
        if (!strcmp((*refs)[i].refname, destination))
            return strcmp((*refs)[i].target, hex) ? -1 : 0;
    char *name = strdup(destination), *target = strdup(hex);
    if (!name || !target) { free(name); free(target); return -1; }
    nip34_ref_t *tmp = realloc(*refs, (*count + 1) * sizeof(**refs));
    if (!tmp) { free(name); free(target); return -1; }
    *refs = tmp;
    size_t len = strlen(source) + strlen(destination) + 3;
    char *spec = malloc(len);
    char **strings = realloc(specs->strings, (*count + 1) * sizeof(*strings));
    if (strings) specs->strings = strings;
    if (!spec || !strings) { free(spec); free(name); free(target); return -1; }
    snprintf(spec, len, "%s%s:%s", force ? "+" : "", source, destination);
    specs->strings[*count] = spec;
    (*refs)[(*count)++] = (nip34_ref_t){ name, target };
    specs->count = *count;
    return 0;
}

/* Resolve the same push refspecs libgit2 will use, failing closed on incomplete
 * collection. Targets are destination refs; an empty OID denotes deletion. */
static nip34_ref_t *collect_local_refs(git_repository *repo,
                                        const char *const *refspecs,
                                        size_t refspec_count,
                                        size_t *out_count, git_strarray *expanded)
{
    *out_count = 0;
    const char *defaults[] = { "refs/heads/*:refs/heads/*" };
    if (!refspec_count) { refspecs = defaults; refspec_count = 1; }
    nip34_ref_t *refs = calloc(1, sizeof(*refs));
    if (!refs) return NULL;
    size_t count = 0;
    for (size_t i = 0; i < refspec_count; i++) {
        git_refspec *spec = NULL;
        git_reference_iterator *iter = NULL;
        git_reference *ref = NULL, *resolved = NULL;
        git_buf destination = GIT_BUF_INIT;
        int rc = -1;
        if (!refspecs[i] || git_refspec_parse(&spec, refspecs[i], 0) != 0)
            goto item_done;
        const char *src = git_refspec_src(spec), *dst = git_refspec_dst(spec);
        if (!src || !*src) {
            rc = dst && *dst ? append_push_ref(&refs, &count, expanded, "", dst, NULL, git_refspec_force(spec)) : -1;
        } else if (strchr(src, '*')) {
            if (!dst || git_reference_iterator_new(&iter, repo) != 0)
                goto item_done;
            while ((rc = git_reference_next(&ref, iter)) == 0) {
                if (git_refspec_src_matches(spec, git_reference_name(ref))) {
                    if (git_reference_resolve(&resolved, ref) != 0 ||
                        git_refspec_transform(&destination, spec, git_reference_name(ref)) != 0 ||
                        append_push_ref(&refs, &count, expanded, git_reference_name(ref),
                            destination.ptr, git_reference_target(resolved), git_refspec_force(spec)) != 0) {
                        rc = -1;
                        break;
                    }
                    git_reference_free(resolved); resolved = NULL;
                    git_buf_dispose(&destination);
                }
                git_reference_free(ref); ref = NULL;
            }
            if (rc == GIT_ITEROVER) rc = 0;
        } else if (git_reference_dwim(&ref, repo, src) == 0 &&
                   git_reference_resolve(&resolved, ref) == 0) {
            rc = append_push_ref(&refs, &count, expanded, git_reference_name(resolved),
                dst && *dst ? dst : git_reference_name(resolved),
                git_reference_target(resolved), git_refspec_force(spec));
        }
item_done:
        git_buf_dispose(&destination);
        git_reference_free(resolved);
        git_reference_free(ref);
        git_reference_iterator_free(iter);
        git_refspec_free(spec);
        if (rc != 0) { free_ref_array(refs, count); return NULL; }
    }
    *out_count = count;
    return refs;
}

static void free_ref_array(nip34_ref_t *refs, size_t count)
{
    if (!refs)
        return;
    for (size_t i = 0; i < count; i++) {
        free(refs[i].refname);
        free(refs[i].target);
    }
    free(refs);
}

/* Apply one ref update to a complete state (empty target means deletion). */
static int state_set(nip34_ref_t **refs, size_t *count,
                     const char *name, const char *target)
{
    size_t i;
    for (i = 0; i < *count; i++)
        if (!strcmp((*refs)[i].refname, name)) break;
    if (!*target) {
        if (i < *count) {
            free((*refs)[i].refname);
            free((*refs)[i].target);
            (*refs)[i] = (*refs)[--*count];
        }
        return 0;
    }
    char *value = strdup(target), *key = strdup(name);
    if (!value || !key) { free(value); free(key); return -1; }
    if (i == *count) {
        nip34_ref_t *tmp = realloc(*refs, (*count + 1) * sizeof(**refs));
        if (!tmp) { free(value); free(key); return -1; }
        *refs = tmp;
        (*count)++;
    } else {
        free((*refs)[i].refname);
        free((*refs)[i].target);
    }
    (*refs)[i] = (nip34_ref_t){ key, value };
    return 0;
}

typedef struct {
    nip34_ref_t *updates;
    size_t count;
    bool rejected;
} grasp_push_plan;

static int push_status(const char *ref, const char *status, void *data)
{
    (void)ref;
    if (status) ((grasp_push_plan *)data)->rejected = true;
    return 0;
}

static int push_negotiation(const git_push_update **updates, size_t count, void *data)
{
    grasp_push_plan *plan = data;
    for (size_t i = 0; i < count; i++) {
        size_t j;
        for (j = 0; j < plan->count; j++)
            if (!strcmp(updates[i]->dst_refname, plan->updates[j].refname)) break;
        git_oid expected = {{0}};
        if (j == plan->count ||
            (*plan->updates[j].target &&
             git_oid_fromstr(&expected, plan->updates[j].target) != 0) ||
            git_oid_cmp(&expected, &updates[i]->dst) != 0)
            return -1; /* Local source changed after signing; do not upload it. */
    }
    return 0;
}

/* The clone path percent-encodes the same d-tag identifier, not another repo. */
static bool repo_identifier_matches(const char *path, const char *id)
{
    while (*path) {
        unsigned char c = (unsigned char)*path++;
        if (c == '%') {
            if (!path[0] || !path[1] || !isxdigit((unsigned char)path[0]) ||
                !isxdigit((unsigned char)path[1])) return false;
            char hex[] = {path[0], path[1], 0};
            c = (unsigned char)strtoul(hex, NULL, 16);
            path += 2;
        }
        if (!c || c != (unsigned char)*id++) return false;
    }
    return *id == '\0';
}

hanami_error_t hanami_push_to_grasp(git_repository *repo,
                                     const hanami_grasp_push_opts_t *opts)
{
    return hanami_push_to_grasp_with_result(repo, opts, NULL);
}

hanami_error_t hanami_push_to_grasp_with_result(git_repository *repo,
    const hanami_grasp_push_opts_t *opts, hanami_grasp_push_result_t *result)
{
    hanami_grasp_push_result_t local = {0};
    if (!result) result = &local;
    memset(result, 0, sizeof(*result));
    hanami_error_t err = HANAMI_ERR_INVALID_ARG;
    hanami_grasp_info_t *info = NULL;
    hanami_nostr_ctx_t *ctx = NULL, *replicas = NULL;
    NostrEvent *previous = NULL, *intended = NULL, *correction = NULL;
    nip34_repo_state_t *baseline = NULL;
    nip34_ref_t *state = NULL;
    size_t state_count = 0;
    grasp_push_plan plan = {0};
    git_strarray expanded = {0};
    git_remote *remote = NULL;
    git_reference *head = NULL;
    char *initial_head = NULL;
    if (!repo || !opts || !opts->clone_url || !opts->signer ||
        !opts->signer->sign || !opts->signer->pubkey || !opts->repo_id ||
        !opts->relay_urls || !opts->relay_count ||
        (opts->refspec_count && !opts->refspecs)) goto done;
    err = hanami_grasp_parse_clone_url(opts->clone_url, &info);
    if (err != HANAMI_OK) goto done;
    if (hanami_repo_private(repo)) { err = HANAMI_ERR_AUTH; goto done; }
    if (!info->pubkey || !repo_identifier_matches(info->repo_name, opts->repo_id)) {
        err = HANAMI_ERR_INVALID_ARG;
        goto done;
    }

    /* The GRASP root, not an arbitrary accepting replica, authorizes the push. */
    const char *roots[] = { info->relay_url, NULL };
    err = hanami_nostr_ctx_new(roots, opts->signer, &ctx);
    if (err != HANAMI_OK) goto done;
    const char *name = opts->remote_name ? opts->remote_name : "grasp";
    int rc = git_remote_lookup(&remote, repo, name);
    if (rc == GIT_ENOTFOUND)
        rc = git_remote_create(&remote, repo, name, opts->clone_url);
    if (rc != 0) { err = HANAMI_ERR_LIBGIT2; goto done; }
    const char *push_url = git_remote_pushurl(remote);
    if (!push_url) push_url = git_remote_url(remote);
    if (!push_url || strcmp(push_url, opts->clone_url)) {
        err = HANAMI_ERR_INVALID_ARG;
        goto done;
    }

    plan.updates = collect_local_refs(repo, opts->refspecs,
                                     opts->refspec_count, &plan.count, &expanded);
    if (!plan.updates) { err = HANAMI_ERR_LIBGIT2; goto done; }
    if (!plan.count) { err = HANAMI_OK; goto done; }
    err = hanami_nostr_fetch_state_event(ctx, opts->repo_id,
                                         opts->signer->pubkey, &previous);
    if (err != HANAMI_OK && err != HANAMI_ERR_NOT_FOUND) goto done;
    if (previous) {
        if (previous->created_at >= INT64_MAX - 1 ||
            nip34_parse_repo_state(previous, &baseline) != NIP34_OK) {
            err = HANAMI_ERR_NOSTR;
            goto done;
        }
        for (size_t i = 0; i < baseline->ref_count; i++)
            if (state_set(&state, &state_count, baseline->refs[i].refname,
                          baseline->refs[i].target) != 0) {
                err = HANAMI_ERR_NOMEM;
                goto done;
            }
    }
    for (size_t i = 0; i < plan.count; i++)
        if (state_set(&state, &state_count, plan.updates[i].refname,
                      plan.updates[i].target) != 0) {
            err = HANAMI_ERR_NOMEM;
            goto done;
        }
    if (!baseline && git_reference_lookup(&head, repo, "HEAD") == 0) {
        const char *target = git_reference_symbolic_target(head);
        for (size_t i = 0; target && i < state_count; i++) {
            if (!strcmp(state[i].refname, target)) {
                initial_head = malloc(strlen(target) + 6);
                if (!initial_head) { err = HANAMI_ERR_NOMEM; goto done; }
                sprintf(initial_head, "ref: %s", target);
                break;
            }
        }
    }
    const char *head_value = baseline ? baseline->head : initial_head;
    if (head_value && !strncmp(head_value, "ref: ", 5)) {
        size_t i;
        for (i = 0; i < state_count; i++)
            if (!strcmp(state[i].refname, head_value + 5)) break;
        if (i == state_count) head_value = NULL;
    }
    err = hanami_nostr_create_state(ctx, opts->repo_id, state, state_count,
        head_value, previous ? previous->created_at + 1 : 0, &intended);
    if (err != HANAMI_OK) goto done;
    /* Prepare compensation before making any externally visible change. */
    if (intended->created_at == INT64_MAX) { err = HANAMI_ERR_NOSTR; goto done; }
    err = hanami_nostr_create_state(ctx, opts->repo_id,
        baseline ? baseline->refs : NULL, baseline ? baseline->ref_count : 0,
        baseline ? baseline->head : NULL, intended->created_at + 1, &correction);
    if (err != HANAMI_OK) goto done;
    err = hanami_nostr_publish_event(ctx, intended);
    if (err == HANAMI_OK) {
        result->state_accepted = true;
        git_push_options push_opts;
        git_push_options_init(&push_opts, GIT_PUSH_OPTIONS_VERSION);
        push_opts.callbacks.push_update_reference = push_status;
        push_opts.callbacks.push_negotiation = push_negotiation;
        push_opts.callbacks.payload = &plan;
        result->push_attempted = true;
        rc = git_remote_push(remote, &expanded, &push_opts);
        err = rc == 0 && !plan.rejected ? HANAMI_OK : HANAMI_ERR_NETWORK;
        result->push_completed = err == HANAMI_OK;
    }
    result->operation_error = err;
    NostrEvent *final = intended;
    if (err != HANAMI_OK) {
        /* A lost OK may still have stored the intended state. Correct even
         * an unconfirmed publish; never rely on purgatory expiry as rollback. */
        result->recovery_attempted = true;
        result->recovery_error = hanami_nostr_publish_event(ctx, correction);
        final = correction;
        if (result->recovery_error != HANAMI_OK) {
            err = result->recovery_error;
            goto done;
        }
    }
    /* Extra relays are replicas, not substitutes for the required GRASP OK. */
    if (hanami_nostr_ctx_new(opts->relay_urls, opts->signer, &replicas) == HANAMI_OK)
        (void)hanami_nostr_publish_event(replicas, final);
done:
    if (!result->operation_error) result->operation_error = err;
    git_reference_free(head);
    free(initial_head);
    git_remote_free(remote);
    free_ref_array(plan.updates, plan.count);
    git_strarray_dispose(&expanded);
    free_ref_array(state, state_count);
    nip34_repo_state_free(baseline);
    nostr_event_free(previous);
    nostr_event_free(intended);
    nostr_event_free(correction);
    hanami_nostr_ctx_free(replicas);
    hanami_nostr_ctx_free(ctx);
    hanami_grasp_info_free(info);
    return err;
}

/* =========================================================================
 * GRASP fetch
 * ========================================================================= */

hanami_error_t hanami_grasp_fetch(git_repository *repo,
                                   const char *clone_url,
                                   const char *const *relay_urls,
                                   size_t relay_count)
{
    if (!clone_url)
        return HANAMI_ERR_INVALID_ARG;

    /* Validate GRASP URL */
    hanami_grasp_info_t *info = NULL;
    hanami_error_t err = hanami_grasp_parse_clone_url(clone_url, &info);
    if (err != HANAMI_OK)
        return err;
    hanami_grasp_info_free(info);

    if (!repo)
        return HANAMI_OK; /* Info-only validation */

    /*
     * TODO: Use relay_urls/relay_count to query kind 30618 state event
     * and verify ref consistency after fetch. Currently unused as GRASP
     * serves standard Git Smart HTTP without requiring Nostr verification.
     */
    (void)relay_urls;
    (void)relay_count;

    /* Use standard libgit2 fetch — GRASP serves standard Smart HTTP */
    git_remote *remote = NULL;
    int rc = git_remote_create_anonymous(&remote, repo, clone_url);
    if (rc < 0)
        return HANAMI_ERR_LIBGIT2;

    git_fetch_options fetch_opts;
    git_fetch_options_init(&fetch_opts, GIT_FETCH_OPTIONS_VERSION);
    rc = git_remote_fetch(remote, NULL, &fetch_opts, "grasp fetch");

    git_remote_free(remote);
    return (rc == 0) ? HANAMI_OK : HANAMI_ERR_NETWORK;
}
