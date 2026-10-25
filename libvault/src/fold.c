/* SPDX-License-Identifier: MIT */
#include "internal.h"
static int bytes_compare(const void *a, const void *b) { return memcmp(a, b, 32); }
static int writers_hash(const porthome_vault_info *v, uint8_t hash[32])
{
    if (!v || v->writers_len > PORTHOME_VAULT_MAX_MEMBERS || (v->writers_len && !v->writers))
        return VAULT_ERR_ARG;
    uint8_t (*keys)[32] = NULL;
    if (v->writers_len) {
        keys = malloc(v->writers_len * 32);
        if (!keys)
            return VAULT_ERR_OOM;
        memcpy(keys, v->writers, v->writers_len * 32);
        qsort(keys, v->writers_len, 32, bytes_compare);
    }
    size_t n = 0;
    for (size_t i = 0; i < v->writers_len; ++i)
        if (!i || memcmp(keys[i], keys[i - 1], 32))
            memmove(keys[n++], keys[i], 32);
    int rc = porthome_sha256((const uint8_t *)keys, n * 32, hash);
    free(keys);
    return rc;
}
static int is_writer(const porthome_vault_info *v, const uint8_t key[32])
{
    for (size_t i = 0; i < v->writers_len; ++i)
        if (!memcmp(v->writers[i], key, 32))
            return 1;
    return 0;
}
static int record_compare(const vault_record *a, const vault_record *b)
{
    int c = memcmp(a->writer, b->writer, 32);
    if (c)
        return c;
    if (a->head->seq != b->head->seq)
        return a->head->seq < b->head->seq ? -1 : 1;
    return memcmp(a->head->root_manifest, b->head->root_manifest, 32);
}
static int conflict_compare(const void *a, const void *b)
{
    const vault_path_conflict *x = a, *y = b;
    int c = strcmp(x->path_enc, y->path_enc);
    if (c)
        return c;
    c = memcmp(x->writer, y->writer, 32);
    if (c)
        return c;
    if (x->seq != y->seq)
        return x->seq < y->seq ? -1 : 1;
    return memcmp(x->root_manifest, y->root_manifest, 32);
}
void vault_fold_result_free(vault_fold_result *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->conflicts_len; ++i)
        free(r->conflicts[i].path_enc);
    free(r->conflicts);
    free(r->frontier);
    free(r->dispositions);
    free(r->retained_roots);
    free(r);
}
typedef struct {
    const vault_record *const *records;
    size_t count;
    uint8_t *state, *ancestors;
} graph;
static int visit(graph *g, size_t index)
{
    if (g->state[index] == 1)
        return VAULT_ERR_CONFLICT;
    if (g->state[index] == 2)
        return 0;
    g->state[index] = 1;
    const porthome_head *h = g->records[index]->head;
    for (size_t i = 0; i < h->parents_len; ++i) {
        const porthome_head_parent *p = &h->parents[i];
        size_t j = 0;
        while (j < g->count && (memcmp(p->writer, g->records[j]->writer, 32) ||
                                p->seq != g->records[j]->head->seq ||
                                memcmp(p->root_manifest, g->records[j]->head->root_manifest, 32)))
            ++j;
        if (j == g->count)
            return VAULT_ERR_INCOMPLETE;
        for (size_t k = j + 1; k < g->count; ++k)
            if (!record_compare(g->records[j], g->records[k]) &&
                memcmp(g->records[j]->head_hash, g->records[k]->head_hash, 32))
                return VAULT_ERR_CONFLICT;
        int rc = visit(g, j);
        if (rc)
            return rc;
        g->ancestors[index * g->count + j] = 1;
        for (size_t k = 0; k < g->count; ++k)
            g->ancestors[index * g->count + k] |= g->ancestors[j * g->count + k];
    }
    g->state[index] = 2;
    return 0;
}
static int add_conflict(vault_fold_result *r, size_t *capacity, const char *path, size_t other,
                        int owner, int theirs, const vault_record *record)
{
    if (r->conflicts_len == *capacity) {
        size_t n = *capacity ? *capacity * 2 : 32;
        if (n > SIZE_MAX / sizeof(*r->conflicts))
            return PORTHOME_ERR_TOO_LARGE;
        vault_path_conflict *p = realloc(r->conflicts, n * sizeof(*p));
        if (!p)
            return VAULT_ERR_OOM;
        r->conflicts = p;
        *capacity = n;
    }
    vault_path_conflict *c = &r->conflicts[r->conflicts_len];
    memset(c, 0, sizeof(*c));
    c->path_enc = strdup(path);
    if (!c->path_enc)
        return VAULT_ERR_OOM;
    c->other_record = other;
    c->in_owner = owner;
    c->in_other = theirs;
    c->seq = record->head->seq;
    memcpy(c->writer, record->writer, 32);
    memcpy(c->root_manifest, record->head->root_manifest, 32);
    ++r->conflicts_len;
    return 0;
}
static int fold(const porthome_vault_info *anchor, const vault_record *const *records, size_t count,
                int checkpoint, vault_fold_result **out)
{
    if (out)
        *out = NULL;
    if (!out || !anchor || (count && !records))
        return VAULT_ERR_ARG;
    if (count > VAULT_MAX_HEADS)
        return PORTHOME_ERR_TOO_LARGE;
    uint8_t anchor_hash[32];
    int rc = writers_hash(anchor, anchor_hash);
    if (rc)
        return rc;
    for (size_t i = 0; i < count; ++i)
        if (!records[i])
            return VAULT_ERR_ARG;
    vault_fold_result *r = calloc(1, sizeof(*r));
    if (!r)
        return VAULT_ERR_OOM;
    r->view = SIZE_MAX;
    r->records_len = count;
    if (!count) {
        *out = r;
        return VAULT_ERR_INCOMPLETE;
    }
    r->frontier = calloc(count, sizeof(*r->frontier));
    r->dispositions = calloc(count, sizeof(*r->dispositions));
    size_t root_cap = 0;
    for (size_t i = 0; i < count; ++i)
        root_cap += 1 + records[i]->head->parents_len;
    r->retained_roots = malloc(root_cap * 32);
    uint8_t (*policies)[32] = calloc(count, 32), *state = calloc(count, 1),
            *edges = calloc(count * count, 1);
    if (!r->frontier || !r->dispositions || !r->retained_roots || !policies || !state || !edges) {
        rc = VAULT_ERR_OOM;
        goto finish;
    }
    for (size_t i = 0; i < count; ++i) {
        const porthome_head *h = records[i]->head;
        memcpy(r->retained_roots[r->retained_roots_len++], h->root_manifest, 32);
        for (size_t j = 0; j < h->parents_len; ++j)
            memcpy(r->retained_roots[r->retained_roots_len++], h->parents[j].root_manifest, 32);
        rc = writers_hash(records[i]->root->vault, policies[i]);
        if (rc)
            goto finish;
    }
    qsort(r->retained_roots, r->retained_roots_len, 32, bytes_compare);
    size_t roots = 0;
    for (size_t i = 0; i < r->retained_roots_len; ++i)
        if (!i || memcmp(r->retained_roots[i], r->retained_roots[i - 1], 32))
            memmove(r->retained_roots[roots++], r->retained_roots[i], 32);
    r->retained_roots_len = roots;
    const porthome_vault_info *policy = anchor;
    const uint8_t *policy_hash = anchor_hash;
    int ambiguous = 0;
    for (size_t i = 0; i < count; ++i) {
        const vault_record *v = records[i];
        const porthome_vault_info *p = v->root->vault;
        if (memcmp(v->writer, anchor->owner, 32) || memcmp(p->owner, anchor->owner, 32) ||
            memcmp(p->vault_id, anchor->vault_id, 32))
            continue;
        if (!checkpoint && p->grants_rev > policy->grants_rev) {
            policy = p;
            policy_hash = policies[i];
            ambiguous = 0;
        } else if (p->grants_rev == policy->grants_rev && memcmp(policy_hash, policies[i], 32))
            ambiguous = 1;
    }
    for (size_t i = 0; i < count; ++i) {
        const vault_record *v = records[i];
        const porthome_vault_info *p = v->root->vault;
        if (!is_writer(policy, v->writer))
            r->dispositions[i] = VAULT_HEAD_NONWRITER;
        else if (v->head->grants_rev < policy->grants_rev)
            r->dispositions[i] = VAULT_HEAD_STALE;
        else if (v->head->grants_rev != policy->grants_rev || memcmp(p->owner, policy->owner, 32) ||
                 memcmp(p->vault_id, policy->vault_id, 32) || memcmp(policies[i], policy_hash, 32))
            r->dispositions[i] = VAULT_HEAD_POLICY;
        else
            r->dispositions[i] = VAULT_HEAD_ACCEPTED;
        for (size_t j = 0; j < i; ++j)
            if (!record_compare(v, records[j])) {
                if (memcmp(v->head_hash, records[j]->head_hash, 32)) {
                    if (r->dispositions[i] == VAULT_HEAD_ACCEPTED &&
                        r->dispositions[j] == VAULT_HEAD_ACCEPTED &&
                        (!checkpoint || !memcmp(v->writer, policy->owner, 32)))
                        ambiguous = 1;
                } else if (r->dispositions[i] == VAULT_HEAD_ACCEPTED)
                    r->dispositions[i] = VAULT_HEAD_DUPLICATE;
                break;
            }
    }
    if (ambiguous) {
        rc = VAULT_ERR_CONFLICT;
        goto finish;
    }
    if (checkpoint) {
        for (size_t i = 0; i < count; ++i) {
            if (r->dispositions[i] != VAULT_HEAD_ACCEPTED)
                continue;
            const porthome_head *h = records[i]->head;
            for (size_t j = 0; j < count; ++j) {
                if (r->dispositions[j] != VAULT_HEAD_ACCEPTED)
                    continue;
                if (i != j && !memcmp(records[i]->writer, policy->owner, 32) &&
                    !memcmp(records[i]->writer, records[j]->writer, 32) &&
                    h->seq == records[j]->head->seq &&
                    memcmp(records[i]->head_hash, records[j]->head_hash, 32)) {
                    rc = VAULT_ERR_CONFLICT;
                    goto finish;
                }
                for (size_t p = 0; p < h->parents_len; ++p)
                    if (!memcmp(h->parents[p].writer, records[j]->writer, 32) &&
                        records[j]->head->seq <= h->parents[p].seq)
                        edges[i * count + j] = 1;
            }
        }
    } else {
        graph g = {records, count, state, edges};
        for (size_t i = 0; i < count; ++i)
            if (r->dispositions[i] == VAULT_HEAD_ACCEPTED) {
                rc = visit(&g, i);
                if (rc)
                    goto finish;
            }
    }
    for (size_t i = 0; i < count; ++i)
        if (r->dispositions[i] == VAULT_HEAD_ACCEPTED) {
            int ancestor = 0;
            for (size_t j = 0; j < count; ++j)
                if (r->dispositions[j] == VAULT_HEAD_ACCEPTED && edges[j * count + i])
                    ancestor = 1;
            if (!ancestor)
                r->frontier[r->frontier_len++] = i;
        }
    for (size_t i = 1; i < r->frontier_len; ++i) {
        size_t x = r->frontier[i], j = i;
        while (j && record_compare(records[x], records[r->frontier[j - 1]]) < 0) {
            r->frontier[j] = r->frontier[j - 1];
            --j;
        }
        r->frontier[j] = x;
    }
    if (!r->frontier_len) {
        rc = checkpoint ? VAULT_ERR_CONFLICT : VAULT_ERR_INCOMPLETE;
        goto finish;
    }
    if (r->frontier_len == 1)
        r->view = r->frontier[0];
    else {
        for (size_t i = 0; i < count; ++i)
            if (r->dispositions[i] == VAULT_HEAD_ACCEPTED &&
                !memcmp(records[i]->writer, policy->owner, 32)) {
                if (r->view == SIZE_MAX || records[i]->head->seq > records[r->view]->head->seq)
                    r->view = i;
            }
        if (r->view != SIZE_MAX)
            for (size_t i = 0; i < count; ++i)
                if (r->dispositions[i] == VAULT_HEAD_ACCEPTED &&
                    !memcmp(records[i]->writer, policy->owner, 32) &&
                    records[i]->head->seq == records[r->view]->head->seq &&
                    record_compare(records[i], records[r->view])) {
                    rc = VAULT_ERR_CONFLICT;
                    goto finish;
                }
        if (r->view == SIZE_MAX) {
            rc = VAULT_ERR_CONFLICT;
            goto finish;
        }
    }
    size_t capacity = 0;
    const vault_record *view = records[r->view];
    for (size_t f = 0; f < r->frontier_len; ++f) {
        size_t index = r->frontier[f];
        if (index == r->view)
            continue;
        const vault_record *other = records[index];
        size_t a = 0, b = 0;
        while (a < view->paths_len || b < other->paths_len) {
            int c =
                a == view->paths_len
                    ? 1
                    : (b == other->paths_len ? -1
                                             : strcmp(view->paths[a].path, other->paths[b].path));
            if (!c) {
                if (memcmp(view->paths[a].digest, other->paths[b].digest, 32)) {
                    rc = add_conflict(r, &capacity, view->paths[a].path, index, 1, 1, other);
                    if (rc)
                        goto finish;
                }
                ++a;
                ++b;
            } else if (c < 0) {
                rc = add_conflict(r, &capacity, view->paths[a++].path, index, 1, 0, other);
                if (rc)
                    goto finish;
            } else {
                rc = add_conflict(r, &capacity, other->paths[b++].path, index, 0, 1, other);
                if (rc)
                    goto finish;
            }
        }
    }
    qsort(r->conflicts, r->conflicts_len, sizeof(*r->conflicts), conflict_compare);
    rc = 0;
finish:
    free(policies);
    free(state);
    free(edges);
    if (rc == VAULT_ERR_OOM || rc == VAULT_ERR_ARG || rc == PORTHOME_ERR_TOO_LARGE) {
        vault_fold_result_free(r);
        return rc;
    }
    if (rc)
        r->view = SIZE_MAX;
    *out = r;
    return rc;
}
int vault_fold(const porthome_vault_info *anchor, const vault_record *const *records, size_t count,
               vault_fold_result **out)
{
    return fold(anchor, records, count, 0, out);
}
int vault_fold_checkpoint(const vault_grant *grant, const vault_record *const *records,
                          size_t count, size_t owner_index, vault_fold_result **out)
{
    if (out)
        *out = NULL;
    if (!grant || !records || owner_index >= count || count > VAULT_MAX_HEADS || !out)
        return VAULT_ERR_ARG;
    for (size_t i = 0; i < count; ++i)
        if (!records[i])
            return VAULT_ERR_ARG;
    const vault_record *owner = records[owner_index];
    const porthome_vault_info *policy = owner->root->vault;
    if (memcmp(owner->writer, grant->owner, 32) || memcmp(policy->owner, grant->owner, 32) ||
        memcmp(policy->vault_id, grant->vault_id, 32) ||
        memcmp(owner->root->node_id, grant->node_id, 32))
        return VAULT_ERR_AUTH;
    /* A caller cannot choose an older owner head from its current-head set. */
    for (size_t i = 0; i < count; ++i)
        if (!memcmp(records[i]->writer, grant->owner, 32) &&
            records[i]->head->seq > owner->head->seq)
            return VAULT_ERR_CONFLICT;
    return fold(policy, records, count, 1, out);
}
