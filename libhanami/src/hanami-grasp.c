/*
 * hanami-grasp.c - GRASP server compatibility layer
 *
 * SPDX-License-Identifier: MIT
 *
 * Implements GRASP detection, URL parsing, and the publish-then-push
 * workflow for Git relays that authorize via NIP-34 kind 30618 events.
 */

#include "hanami/hanami-grasp.h"
#include "hanami/hanami-nostr.h"

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

    if (out_idx != 32)
        return NULL;

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

/**
 * Collect current ref state from the local repository for the given refspecs.
 * Returns an allocated array of nip34_ref_t. Caller frees with free_ref_array.
 */
static nip34_ref_t *collect_local_refs(git_repository *repo,
                                        const char *const *refspecs,
                                        size_t refspec_count,
                                        size_t *out_count)
{
    *out_count = 0;

    if (!refspecs || refspec_count == 0) {
        /* Default: push all refs */
        git_reference_iterator *iter = NULL;
        if (git_reference_iterator_new(&iter, repo) != 0)
            return NULL;

        /* Count first pass */
        size_t cap = 16;
        nip34_ref_t *refs = calloc(cap, sizeof(nip34_ref_t));
        if (!refs) {
            git_reference_iterator_free(iter);
            return NULL;
        }

        git_reference *ref = NULL;
        size_t count = 0;
        while (git_reference_next(&ref, iter) == 0) {
            if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) {
                git_reference_free(ref);
                continue;
            }

            if (count >= cap) {
                size_t new_cap = cap * 2;
                nip34_ref_t *tmp = realloc(refs, new_cap * sizeof(nip34_ref_t));
                if (!tmp) {
                    git_reference_free(ref);
                    break;  /* cap unchanged — bounds checks below remain valid */
                }
                refs = tmp;
                cap = new_cap;  /* only update cap after successful realloc */
            }

            refs[count].refname = strdup(git_reference_name(ref));
            char oid_hex[41] = {0};
            git_oid_tostr(oid_hex, sizeof(oid_hex), git_reference_target(ref));
            refs[count].target = strdup(oid_hex);
            count++;
            git_reference_free(ref);
        }
        git_reference_iterator_free(iter);

        /* Add HEAD */
        git_reference *head = NULL;
        if (git_repository_head(&head, repo) == 0) {
            if (count >= cap) {
                cap++;
                nip34_ref_t *tmp = realloc(refs, cap * sizeof(nip34_ref_t));
                if (tmp) refs = tmp;
            }
            if (count < cap) {
                refs[count].refname = strdup("HEAD");
                const char *target_name = git_reference_symbolic_target(head);
                if (target_name) {
                    size_t tlen = 5 + strlen(target_name) + 1;
                    char *t = malloc(tlen);
                    if (t) {
                        snprintf(t, tlen, "ref: %s", target_name);
                        refs[count].target = t;
                    } else {
                        refs[count].target = strdup("ref: refs/heads/main");
                    }
                } else {
                    char oid_hex[41] = {0};
                    git_oid_tostr(oid_hex, sizeof(oid_hex), git_reference_target(head));
                    refs[count].target = strdup(oid_hex);
                }
                count++;
            }
            git_reference_free(head);
        }

        *out_count = count;
        return refs;
    }

    /* Push specific refspecs */
    size_t cap = refspec_count + 1;
    nip34_ref_t *refs = calloc(cap, sizeof(nip34_ref_t));
    if (!refs)
        return NULL;

    size_t count = 0;
    for (size_t i = 0; i < refspec_count && refspecs[i]; i++) {
        const char *spec = refspecs[i];

        /* Simple refspec: "refs/heads/main" or "refs/heads/main:refs/heads/main" */
        const char *colon = strchr(spec, ':');
        const char *local_ref = spec;
        size_t local_len = colon ? (size_t)(colon - spec) : strlen(spec);

        char *local_name = strndup(local_ref, local_len);
        if (!local_name)
            continue;

        git_reference *ref = NULL;
        if (git_reference_lookup(&ref, repo, local_name) == 0) {
            git_reference *resolved = NULL;
            if (git_reference_resolve(&resolved, ref) == 0) {
                const char *remote_name = colon ? colon + 1 : local_name;
                refs[count].refname = strdup(remote_name);
                char oid_hex[41] = {0};
                git_oid_tostr(oid_hex, sizeof(oid_hex), git_reference_target(resolved));
                refs[count].target = strdup(oid_hex);
                count++;
                git_reference_free(resolved);
            }
            git_reference_free(ref);
        }
        free(local_name);
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

hanami_error_t hanami_push_to_grasp(git_repository *repo,
                                     const hanami_grasp_push_opts_t *opts)
{
    if (!repo || !opts)
        return HANAMI_ERR_INVALID_ARG;
    if (!opts->clone_url || !opts->signer || !opts->repo_id)
        return HANAMI_ERR_INVALID_ARG;
    if (!opts->relay_urls || opts->relay_count == 0)
        return HANAMI_ERR_INVALID_ARG;

    /* Validate this is a GRASP URL */
    hanami_grasp_info_t *info = NULL;
    hanami_error_t err = hanami_grasp_parse_clone_url(opts->clone_url, &info);
    if (err != HANAMI_OK)
        return err;
    hanami_grasp_info_free(info);

    /* Step 1: Collect local refs to publish */
    size_t ref_count = 0;
    nip34_ref_t *refs = collect_local_refs(repo, opts->refspecs,
                                            opts->refspec_count, &ref_count);
    if (!refs && ref_count > 0) {
        return HANAMI_ERR_LIBGIT2;
    }

    /* Separate HEAD from regular refs for the state event */
    const char *head_value = NULL;
    for (size_t i = 0; i < ref_count; i++) {
        if (refs[i].refname && strcmp(refs[i].refname, "HEAD") == 0) {
            head_value = refs[i].target;
            break;
        }
    }

    /* Step 2: Publish kind 30618 state BEFORE push (GRASP authorization) */
    hanami_nostr_ctx_t *nostr_ctx = NULL;
    err = hanami_nostr_ctx_new(opts->relay_urls, opts->signer, &nostr_ctx);
    if (err != HANAMI_OK) {
        free_ref_array(refs, ref_count);
        return err;
    }

    /* Build ref array without HEAD for the state event */
    size_t state_ref_count = 0;
    nip34_ref_t *state_refs = calloc(ref_count, sizeof(nip34_ref_t));
    if (state_refs) {
        for (size_t i = 0; i < ref_count; i++) {
            if (refs[i].refname && strcmp(refs[i].refname, "HEAD") != 0) {
                state_refs[state_ref_count].refname = refs[i].refname;
                state_refs[state_ref_count].target = refs[i].target;
                state_ref_count++;
            }
        }
    }

    err = hanami_nostr_publish_state(nostr_ctx, opts->repo_id,
                                     state_refs, state_ref_count,
                                     head_value);
    free(state_refs);
    hanami_nostr_ctx_free(nostr_ctx);

    if (err != HANAMI_OK) {
        free_ref_array(refs, ref_count);
        return err;
    }

    /* Step 3: Push via standard Git Smart HTTP */
    const char *remote_name = opts->remote_name ? opts->remote_name : "grasp";

    /* Create/update remote with the GRASP clone URL */
    git_remote *remote = NULL;
    int rc = git_remote_lookup(&remote, repo, remote_name);
    if (rc == GIT_ENOTFOUND) {
        rc = git_remote_create(&remote, repo, remote_name, opts->clone_url);
    }

    if (rc < 0) {
        free_ref_array(refs, ref_count);
        return HANAMI_ERR_LIBGIT2;
    }

    /* Build refspec strings for push */
    git_strarray push_refspecs = {0};
    if (opts->refspecs && opts->refspec_count > 0) {
        push_refspecs.strings = (char **)opts->refspecs;
        push_refspecs.count = opts->refspec_count;
    } else {
        /* Default: push all heads */
        char *default_spec = "refs/heads/*:refs/heads/*";
        push_refspecs.strings = &default_spec;
        push_refspecs.count = 1;
    }

    git_push_options push_opts = GIT_PUSH_OPTIONS_INIT;
    rc = git_remote_push(remote, &push_refspecs, &push_opts);

    git_remote_free(remote);
    free_ref_array(refs, ref_count);

    return (rc == 0) ? HANAMI_OK : HANAMI_ERR_NETWORK;
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

    (void)relay_urls;
    (void)relay_count;

    /* Use standard libgit2 fetch — GRASP serves standard Smart HTTP */
    git_remote *remote = NULL;
    int rc = git_remote_create_anonymous(&remote, repo, clone_url);
    if (rc < 0)
        return HANAMI_ERR_LIBGIT2;

    git_fetch_options fetch_opts = GIT_FETCH_OPTIONS_INIT;
    rc = git_remote_fetch(remote, NULL, &fetch_opts, "grasp fetch");

    git_remote_free(remote);
    return (rc == 0) ? HANAMI_OK : HANAMI_ERR_NETWORK;
}
