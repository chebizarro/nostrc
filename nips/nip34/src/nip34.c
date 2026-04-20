/**
 * @file nip34.c
 * @brief NIP-34: Git Stuff — event creation & parsing
 *
 * SPDX-License-Identifier: MIT
 */

#include "nip34.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static const char *find_tag_value(const NostrEvent *event, const char *key)
{
    if (!event || !event->tags || !key)
        return NULL;

    NostrTags *tags = event->tags;
    for (size_t i = 0; i < tags->count; i++) {
        NostrTag *tag = tags->data[i];
        if (!tag || tag->size < 2)
            continue;
        const char *tag_key = string_array_get(tag, 0);
        if (tag_key && strcmp(tag_key, key) == 0)
            return string_array_get(tag, 1);
    }
    return NULL;
}

/* Collect all tag values for a given key into a NULL-terminated array.
 * Returns count in *out_count. Caller frees the array and each string. */
static char **collect_tag_values(const NostrEvent *event, const char *key,
                                 size_t *out_count)
{
    *out_count = 0;
    if (!event || !event->tags || !key)
        return NULL;

    /* Count matches */
    size_t n = 0;
    for (size_t i = 0; i < event->tags->count; i++) {
        NostrTag *tag = event->tags->data[i];
        if (tag && tag->size >= 2) {
            const char *k = string_array_get(tag, 0);
            if (k && strcmp(k, key) == 0) n++;
        }
    }
    if (n == 0) return NULL;

    char **arr = calloc(n + 1, sizeof(char *));
    if (!arr) return NULL;

    size_t idx = 0;
    for (size_t i = 0; i < event->tags->count && idx < n; i++) {
        NostrTag *tag = event->tags->data[i];
        if (tag && tag->size >= 2) {
            const char *k = string_array_get(tag, 0);
            if (k && strcmp(k, key) == 0) {
                const char *v = string_array_get(tag, 1);
                arr[idx++] = v ? strdup(v) : strdup("");
            }
        }
    }
    *out_count = idx;
    return arr;
}

static void free_string_array(char **arr, size_t count)
{
    if (!arr) return;
    for (size_t i = 0; i < count; i++)
        free(arr[i]);
    free(arr);
}

/* Helper to create a 2-element tag and append to a NostrTags.
 * Grows capacity automatically. */
static int tags_add2(NostrTags *tags, const char *key, const char *value)
{
    NostrTag *tag = new_string_array(2);
    if (!tag) return -1;
    string_array_add(tag, key);
    string_array_add(tag, value);
    nostr_tags_append(tags, tag);
    return 0;
}

static int tags_add3(NostrTags *tags, const char *k, const char *v1,
                     const char *v2)
{
    NostrTag *tag = new_string_array(3);
    if (!tag) return -1;
    string_array_add(tag, k);
    string_array_add(tag, v1);
    string_array_add(tag, v2);
    nostr_tags_append(tags, tag);
    return 0;
}

/* Allocate a fresh NostrTags with initial capacity */
static NostrTags *new_tags(size_t capacity)
{
    NostrTags *tags = calloc(1, sizeof(NostrTags));
    if (!tags) return NULL;
    tags->data = calloc(capacity, sizeof(NostrTag *));
    if (!tags->data) { free(tags); return NULL; }
    tags->count = 0;
    tags->capacity = capacity;
    return tags;
}

/* =========================================================================
 * Repository (kind 30617) — creation
 * ========================================================================= */

NostrEvent *nip34_create_repo_announcement(const char *repo_id,
                                           const char *name,
                                           const char *desc,
                                           const char *const *clone_urls,
                                           const char *const *web_urls,
                                           const char *const *relay_urls,
                                           const char *const *maintainers)
{
    if (!repo_id || !name)
        return NULL;

    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    nostr_event_set_kind(ev, NIP34_KIND_REPOSITORY);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, desc ? desc : "");

    NostrTags *tags = new_tags(16);
    if (!tags) { nostr_event_free(ev); return NULL; }

    /* d tag (required for addressable events) */
    tags_add2(tags, "d", repo_id);
    tags_add2(tags, "name", name);

    if (desc)
        tags_add2(tags, "description", desc);

    if (clone_urls) {
        for (size_t i = 0; clone_urls[i]; i++)
            tags_add2(tags, "clone", clone_urls[i]);
    }

    if (web_urls) {
        for (size_t i = 0; web_urls[i]; i++)
            tags_add2(tags, "web", web_urls[i]);
    }

    if (relay_urls) {
        for (size_t i = 0; relay_urls[i]; i++)
            tags_add2(tags, "relays", relay_urls[i]);
    }

    if (maintainers) {
        for (size_t i = 0; maintainers[i]; i++)
            tags_add2(tags, "p", maintainers[i]);
    }

    nostr_event_set_tags(ev, tags);
    return ev;
}

/* =========================================================================
 * Repository (kind 30617) — parsing
 * ========================================================================= */

nip34_result_t nip34_parse_repository(const NostrEvent *event,
                                      nip34_repository_t **out)
{
    if (!event || !out)
        return NIP34_ERR_NULL_PARAM;

    if (nostr_event_get_kind(event) != NIP34_KIND_REPOSITORY)
        return NIP34_ERR_INVALID_KIND;

    *out = NULL;

    const char *d = find_tag_value(event, "d");
    if (!d)
        return NIP34_ERR_MISSING_TAG;

    nip34_repository_t *repo = calloc(1, sizeof(*repo));
    if (!repo)
        return NIP34_ERR_ALLOC;

    repo->id = strdup(d);

    const char *n = find_tag_value(event, "name");
    if (n) repo->name = strdup(n);

    const char *desc = find_tag_value(event, "description");
    if (desc) repo->description = strdup(desc);

    const char *r = find_tag_value(event, "r");
    if (r) repo->earliest_unique_commit = strdup(r);

    repo->clone = collect_tag_values(event, "clone", &repo->clone_count);
    repo->web = collect_tag_values(event, "web", &repo->web_count);
    repo->relays = collect_tag_values(event, "relays", &repo->relay_count);
    repo->maintainers = collect_tag_values(event, "p", &repo->maintainer_count);

    *out = repo;
    return NIP34_OK;
}

void nip34_repository_free(nip34_repository_t *repo)
{
    if (!repo) return;
    free(repo->id);
    free(repo->name);
    free(repo->description);
    free(repo->earliest_unique_commit);
    free_string_array(repo->clone, repo->clone_count);
    free_string_array(repo->web, repo->web_count);
    free_string_array(repo->relays, repo->relay_count);
    free_string_array(repo->maintainers, repo->maintainer_count);
    free(repo);
}

/* =========================================================================
 * Repository State (kind 30618) — creation
 * ========================================================================= */

NostrEvent *nip34_create_repo_state(const char *repo_id,
                                    const nip34_ref_t *refs,
                                    size_t ref_count,
                                    const char *head)
{
    if (!repo_id)
        return NULL;

    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    nostr_event_set_kind(ev, NIP34_KIND_REPOSITORY_STATE);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, "");

    NostrTags *tags = new_tags(ref_count + 4);
    if (!tags) { nostr_event_free(ev); return NULL; }

    /* d tag */
    tags_add2(tags, "d", repo_id);

    /* Each ref as its own tag: [refname, target] */
    for (size_t i = 0; i < ref_count; i++) {
        if (refs[i].refname && refs[i].target)
            tags_add2(tags, refs[i].refname, refs[i].target);
    }

    /* HEAD as a tag */
    if (head)
        tags_add2(tags, "HEAD", head);

    nostr_event_set_tags(ev, tags);
    return ev;
}

/* =========================================================================
 * Repository State (kind 30618) — parsing
 * ========================================================================= */

nip34_result_t nip34_parse_repo_state(const NostrEvent *event,
                                      nip34_repo_state_t **out)
{
    if (!event || !out)
        return NIP34_ERR_NULL_PARAM;

    if (nostr_event_get_kind(event) != NIP34_KIND_REPOSITORY_STATE)
        return NIP34_ERR_INVALID_KIND;

    *out = NULL;

    const char *d = find_tag_value(event, "d");
    if (!d)
        return NIP34_ERR_MISSING_TAG;

    nip34_repo_state_t *state = calloc(1, sizeof(*state));
    if (!state)
        return NIP34_ERR_ALLOC;

    state->repo_id = strdup(d);

    /* HEAD */
    const char *head = find_tag_value(event, "HEAD");
    if (head)
        state->head = strdup(head);

    /* Count ref tags (tags starting with "refs/") */
    size_t n = 0;
    if (event->tags) {
        for (size_t i = 0; i < event->tags->count; i++) {
            NostrTag *tag = event->tags->data[i];
            if (tag && tag->size >= 2) {
                const char *k = string_array_get(tag, 0);
                if (k && strncmp(k, "refs/", 5) == 0) n++;
            }
        }
    }

    if (n > 0) {
        state->refs = calloc(n, sizeof(nip34_ref_t));
        if (!state->refs) {
            nip34_repo_state_free(state);
            return NIP34_ERR_ALLOC;
        }

        size_t idx = 0;
        for (size_t i = 0; i < event->tags->count && idx < n; i++) {
            NostrTag *tag = event->tags->data[i];
            if (tag && tag->size >= 2) {
                const char *k = string_array_get(tag, 0);
                if (k && strncmp(k, "refs/", 5) == 0) {
                    state->refs[idx].refname = strdup(k);
                    const char *v = string_array_get(tag, 1);
                    state->refs[idx].target = v ? strdup(v) : strdup("");
                    idx++;
                }
            }
        }
        state->ref_count = idx;
    }

    *out = state;
    return NIP34_OK;
}

void nip34_repo_state_free(nip34_repo_state_t *state)
{
    if (!state) return;
    free(state->repo_id);
    free(state->head);
    if (state->refs) {
        for (size_t i = 0; i < state->ref_count; i++) {
            free(state->refs[i].refname);
            free(state->refs[i].target);
        }
        free(state->refs);
    }
    free(state);
}

/* =========================================================================
 * Patch (kind 1617) — creation
 * ========================================================================= */

NostrEvent *nip34_create_patch(const char *repo_addr,
                               const char *content,
                               const char *commit_id,
                               const char *parent_id,
                               const char *subject)
{
    if (!repo_addr || !content)
        return NULL;

    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    nostr_event_set_kind(ev, NIP34_KIND_PATCH);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, content);

    NostrTags *tags = new_tags(8);
    if (!tags) { nostr_event_free(ev); return NULL; }

    /* Repository reference */
    tags_add2(tags, "a", repo_addr);

    if (commit_id)
        tags_add3(tags, "commit", commit_id, "");

    if (parent_id)
        tags_add3(tags, "parent-commit", parent_id, "");

    if (subject)
        tags_add2(tags, "subject", subject);

    nostr_event_set_tags(ev, tags);
    return ev;
}

/* =========================================================================
 * Patch (kind 1617) — parsing
 * ========================================================================= */

nip34_result_t nip34_parse_patch(const NostrEvent *event,
                                 nip34_patch_t **out)
{
    if (!event || !out)
        return NIP34_ERR_NULL_PARAM;

    if (nostr_event_get_kind(event) != NIP34_KIND_PATCH)
        return NIP34_ERR_INVALID_KIND;

    *out = NULL;

    nip34_patch_t *patch = calloc(1, sizeof(*patch));
    if (!patch)
        return NIP34_ERR_ALLOC;

    const char *a = find_tag_value(event, "a");
    if (a) patch->repo_addr = strdup(a);

    const char *c = find_tag_value(event, "commit");
    if (c) patch->commit_id = strdup(c);

    const char *p = find_tag_value(event, "parent-commit");
    if (p) patch->parent_id = strdup(p);

    const char *s = find_tag_value(event, "subject");
    if (s) patch->subject = strdup(s);

    const char *content = nostr_event_get_content(event);
    if (content) patch->content = strdup(content);

    /* Check for root marker (t tag with value "root") */
    const char *t = find_tag_value(event, "t");
    patch->is_root = (t && strcmp(t, "root") == 0);

    *out = patch;
    return NIP34_OK;
}

void nip34_patch_free(nip34_patch_t *patch)
{
    if (!patch) return;
    free(patch->repo_addr);
    free(patch->commit_id);
    free(patch->parent_id);
    free(patch->subject);
    free(patch->content);
    free(patch);
}

/* =========================================================================
 * Status events (kinds 1630–1633)
 * ========================================================================= */

NostrEvent *nip34_create_status(const char *target_event_id,
                                nip34_status_kind_t status,
                                const char *content)
{
    if (!target_event_id)
        return NULL;

    if (status < NIP34_STATUS_OPEN || status > NIP34_STATUS_DRAFT)
        return NULL;

    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    nostr_event_set_kind(ev, (int)status);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, content ? content : "");

    NostrTags *tags = new_tags(2);
    if (!tags) { nostr_event_free(ev); return NULL; }

    tags_add2(tags, "e", target_event_id);

    nostr_event_set_tags(ev, tags);
    return ev;
}

/* =========================================================================
 * Error strings
 * ========================================================================= */

const char *nip34_strerror(nip34_result_t result)
{
    switch (result) {
    case NIP34_OK:              return "Success";
    case NIP34_ERR_NULL_PARAM:  return "Null parameter";
    case NIP34_ERR_ALLOC:       return "Memory allocation failed";
    case NIP34_ERR_INVALID_KIND: return "Invalid event kind";
    case NIP34_ERR_MISSING_TAG: return "Required tag missing from event";
    case NIP34_ERR_PARSE:       return "Parse error";
    default:                    return "Unknown NIP-34 error";
    }
}
