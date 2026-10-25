/* SPDX-License-Identifier: MIT */
#include "vc.h"
#include <porthome/crypto.h>
#include <openssl/crypto.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static int owner(vc_app *a)
{
    const porthome_dir_node *root = vault_tree_node_at(a->state.tree, 0, NULL);
    return root && root->vault && !CRYPTO_memcmp(root->vault->owner, a->pub, 32) ? 0
                                                                                 : VAULT_ERR_AUTH;
}
static int previous(vc_app *a, porthome_head **head)
{
    vc_state *s = &a->state;
    *head = NULL;
    if (s->current == SIZE_MAX)
        return 0;
    porthome_ring_t ring;
    const porthome_dir_node *root = vault_tree_node_at(s->tree, 0, &ring);
    return porthome_head_event_open(s->heads[s->current].event, root->vault->vault_id, &ring,
                                    s->heads[s->current].epoch, head);
}
static uint64_t next_time(const porthome_head *p)
{
    uint64_t now = (uint64_t)time(NULL);
    return p && now <= p->written_at ? p->written_at + 1 : now;
}
static void head_free(porthome_head *h)
{
    if (h) {
        porthome_head_dispose(h);
        free(h);
    }
}
static int outgoing_head(vc_app *a, const uint8_t address[32])
{
    porthome_head *p = NULL, *h = NULL;
    int rc = previous(a, &p);
    if (rc)
        return rc;
    rc = vault_tree_head_new(a->state.tree, p, a->pub, address, next_time(p), &h);
    head_free(p);
    if (rc)
        return rc;
    porthome_ring_t ring;
    const porthome_dir_node *root = vault_tree_node_at(a->state.tree, 0, &ring);
    NostrEvent *e = NULL;
    rc = porthome_head_event_new(h, ring.entries[ring.count - 1].node_key, a->pub, &e);
    if (!rc)
        rc = nostr_event_sign(e, a->secret);
    if (!rc)
        rc = vc_set_outgoing(a, e, root->epoch);
    nostr_event_free(e);
    head_free(h);
    return rc;
}
static int finish_outgoing(vc_app *a)
{
    vc_state *s = &a->state;
    if (!s->outgoing)
        return 0;
    int rc = vc_publish(a);
    if (rc)
        return rc;
    int kind = nostr_event_get_kind(s->outgoing);
    if (kind == PORTHOME_HEAD_KIND) {
        rc = vc_head_add(a, s->outgoing, s->outgoing_epoch, &s->current);
        if (rc)
            return rc;
        s->needs_publication = 0;
        if (s->phase == VC_PHASE_SHARE)
            s->phase = VC_PHASE_NONE;
        if (s->phase == VC_PHASE_REVOKE && s->rotation) {
            s->rotation = 0;
            s->distribution = 1;
            s->delivery_cursor = 0;
        }
    } else if (kind == 1059) {
        memcpy(s->last_grant, nostr_event_get_id(s->outgoing), 65);
        if (s->phase == VC_PHASE_GRANT)
            s->phase = VC_PHASE_NONE;
        else if (s->distribution)
            s->delivery_cursor++;
    } else
        return VAULT_ERR_ARG;
    nostr_event_free(s->outgoing);
    s->outgoing = NULL;
    s->outgoing_epoch = 0;
    s->ack = s->intent = s->rejected = 0;
    return vc_state_save(a);
}
static int seal_publish(vc_app *a)
{
    vault_blob_io io = {a, vc_blob_read, vc_blob_put};
    for (size_t i = 0; a->state.phase == VC_PHASE_SHARE && i < vault_tree_count(a->state.tree);
         i++) {
        const porthome_dir_node *n = vault_tree_node_at(a->state.tree, i, NULL);
        for (size_t j = 0; j < n->entries_len; j++)
            for (size_t k = 0; k < n->entries[j].chunks_len; k++) {
                uint8_t *b = NULL;
                size_t z = 0;
                const uint8_t *hash = n->entries[j].chunks[k].sha256;
                int rc = vc_cache_get(a, hash, &b, &z);
                if (rc)
                    return rc;
                rc = vc_blob_put(a, b, z, hash);
                free(b);
                if (rc)
                    return rc;
            }
    }
    uint8_t root[32];
    int rc = vault_tree_seal(a->state.tree, &io, root);
    if (!rc)
        rc = outgoing_head(a, root);
    if (!rc)
        rc = finish_outgoing(a);
    return rc;
}
static int latest_owner(vc_app *a)
{
    int rc = owner(a);
    if (rc)
        return rc;
    vc_state *s = &a->state;
    const porthome_dir_node *root = vault_tree_node_at(s->tree, 0, NULL);
    porthome_vault_info policy = *root->vault;
    policy.writers_len = 0;
    policy.writers = NULL;
    rc = vc_query_heads(a, &policy);
    if (rc)
        return rc;
    porthome_ring_t ring;
    root = vault_tree_node_at(s->tree, 0, &ring);
    size_t best = SIZE_MAX;
    uint64_t seq = 0;
    for (size_t i = 0; i < s->heads_len; i++) {
        if (strcmp(nostr_event_get_pubkey(s->heads[i].event), a->public_key))
            continue;
        porthome_head *h = NULL;
        rc = porthome_head_event_open(s->heads[i].event, root->vault->vault_id, &ring,
                                      s->heads[i].epoch, &h);
        if (rc)
            return rc;
        if (best == SIZE_MAX || h->seq > seq) {
            best = i;
            seq = h->seq;
        } else if (h->seq == seq) {
            head_free(h);
            vc_error(a, "owner fork: refusing administrative mutation");
            return VAULT_ERR_CONFLICT;
        }
        head_free(h);
    }
    if (best == SIZE_MAX)
        return VAULT_ERR_INCOMPLETE;
    /* Only the selected owner's signed ancestor closure can authorize an
     * administrative successor. Unrelated writer forks cannot veto revoke. */
    porthome_head *decoded[VAULT_MAX_HEADS] = {0};
    uint8_t authors[VAULT_MAX_HEADS][32];
    size_t queue[VAULT_MAX_HEADS], count = 1;
    unsigned char seen[VAULT_MAX_HEADS] = {0};
    vault_record *records[VAULT_MAX_HEADS] = {0};
    vault_tree *chosen = NULL;
    vault_fold_result *fold = NULL;
    for (size_t i = 0; i < s->heads_len; i++) {
        if (porthome_from_hex64(nostr_event_get_pubkey(s->heads[i].event), authors[i])) {
            rc = VAULT_ERR_AUTH;
            goto closure_done;
        }
        int parsed = porthome_head_event_open(s->heads[i].event, root->vault->vault_id, &ring,
                                              s->heads[i].epoch, &decoded[i]);
        if (parsed && i == best) {
            rc = parsed;
            goto closure_done;
        }
    }
    queue[0] = best;
    seen[best] = 1;
    for (size_t at = 0; at < count; at++) {
        const porthome_head *h = decoded[queue[at]];
        for (size_t p = 0; p < h->parents_len; p++) {
            int found = 0;
            for (size_t i = 0; i < s->heads_len; i++)
                if (decoded[i] && decoded[i]->seq == h->parents[p].seq &&
                    !memcmp(authors[i], h->parents[p].writer, 32) &&
                    !memcmp(decoded[i]->root_manifest, h->parents[p].root_manifest, 32)) {
                    found = 1;
                    if (!seen[i]) {
                        seen[i] = 1;
                        queue[count++] = i;
                    }
                }
            if (!found) {
                vc_error(a, "owner predecessor has missing signed parent history");
                rc = VAULT_ERR_INCOMPLETE;
                goto closure_done;
            }
        }
    }
    for (size_t at = 0; at < count; at++) {
        vault_tree *t = NULL;
        rc = vc_record_load(a, s->heads[queue[at]].event, s->heads[queue[at]].epoch, &records[at],
                            &t);
        if (at == 0)
            chosen = t;
        else
            vault_tree_free(t);
        if (rc)
            goto closure_done;
    }
    rc = vault_fold(root->vault, (const vault_record *const *)records, count, &fold);
    if (!rc && fold->view != 0)
        rc = VAULT_ERR_CONFLICT;
    if (!rc && vault_tree_node_at(chosen, 0, NULL)->epoch < root->epoch)
        rc = VAULT_ERR_EPOCH;
closure_done:
    for (size_t i = 0; i < s->heads_len; i++)
        head_free(decoded[i]);
    for (size_t i = 0; i < count; i++)
        vault_record_free(records[i]);
    vault_fold_result_free(fold);
    if (rc) {
        vault_tree_free(chosen);
        return rc;
    }
    vault_tree_free(s->tree);
    s->tree = chosen;
    s->current = best;
    return vc_state_save(a);
}
static int carrier_publish(void *ctx, const NostrEvent *e)
{
    vc_app *a = ctx;
    int rc = vc_set_outgoing(a, e, 0);
    return rc ? rc : finish_outgoing(a);
}
static int deliver(vc_app *a, const uint8_t recipient[32], unsigned cap, int history)
{
    vc_state *s = &a->state;
    porthome_ring_t r;
    const porthome_dir_node *root = vault_tree_node_at(s->tree, 0, &r);
    porthome_head *h = NULL;
    int rc = previous(a, &h);
    if (rc || !h) {
        head_free(h);
        return rc ? rc : VAULT_ERR_INCOMPLETE;
    }
    vault_grant g = {.cap = cap,
                     .relays = s->relays,
                     .relays_len = s->relays_len,
                     .blossom = s->blossom,
                     .blossom_len = s->blossom_len,
                     .issued_at = (uint64_t)time(NULL)};
    memcpy(g.vault_id, root->vault->vault_id, 32);
    memcpy(g.node_id, root->node_id, 32);
    memcpy(g.owner, a->pub, 32);
    memcpy(g.root_manifest, h->root_manifest, 32);
    head_free(h);
    if (!history) {
        r.entries += r.count - 1;
        r.count = 1;
    }
    rc = vault_ring_copy(&g.ring, &r);
    if (rc)
        return rc;
    vault_nip59_transport t = {a, carrier_publish, NULL};
    vault_nip59_carrier *c = NULL;
    rc = vault_nip59_carrier_new(a->sk, &t, &c);
    if (!rc)
        rc = vault_grant_send(vault_nip59_carrier_interface(c), recipient, &g);
    vault_nip59_carrier_free(c);
    vault_ring_dispose(&g.ring);
    return rc;
}
int vc_command_share(vc_app *a)
{
    vc_state *s = &a->state;
    int rc = 0;
    if (a->options.resume) {
        if (s->phase != VC_PHASE_SHARE)
            return VAULT_ERR_ARG;
    } else {
        if (s->tree)
            return VAULT_ERR_ARG;
        rc = vc_import(a, a->options.source, &s->tree);
        if (rc)
            return rc;
        s->phase = VC_PHASE_SHARE;
        if ((rc = vc_state_save(a)))
            return rc;
    }
    if ((rc = vc_reconcile_blob(a)))
        return rc;
    if (s->outgoing)
        rc = finish_outgoing(a);
    else
        rc = seal_publish(a);
    if (!rc) {
        char v[65];
        porthome_hex64(vault_tree_node_at(s->tree, 0, NULL)->vault->vault_id, v);
        printf("{\"vault_id\":\"%s\",\"head\":\"%s\"}\n", v,
               nostr_event_get_id(s->heads[s->current].event));
    }
    return rc;
}
int vc_command_grant(vc_app *a)
{
    vc_state *s = &a->state;
    int rc;
    if (a->options.resume) {
        if (s->phase != VC_PHASE_GRANT)
            return VAULT_ERR_ARG;
    } else {
        if (s->phase)
            return VAULT_ERR_AGAIN;
        if ((rc = latest_owner(a)))
            return rc;
        if (porthome_from_hex64(a->options.member_hex, s->member))
            return VAULT_ERR_ARG;
        if (!CRYPTO_memcmp(s->member, a->pub, 32))
            return VAULT_ERR_ARG;
        s->cap = a->options.cap;
        s->history = a->options.history;
        s->phase = VC_PHASE_GRANT;
        s->last_grant[0] = 0;
        rc = vault_tree_add_member(s->tree, a->pub, s->member, s->cap);
        if (!rc)
            rc = vc_state_save(a);
        if (rc)
            return rc;
    }
    if ((rc = vc_reconcile_blob(a)))
        return rc;
    if (s->outgoing) {
        rc = finish_outgoing(a);
        if (rc || s->phase == VC_PHASE_NONE)
            goto done;
    }
    /* Persisted policy is published before the new writer's carrier. */
    {
        porthome_head *h = NULL;
        rc = previous(a, &h);
        const porthome_dir_node *root = vault_tree_node_at(s->tree, 0, NULL);
        int changed = !h || h->grants_rev != root->vault->grants_rev;
        head_free(h);
        if (rc)
            return rc;
        if (changed && (rc = seal_publish(a)))
            return rc;
    }
    rc = deliver(a, s->member, s->cap, s->history);
done:
    if (!rc)
        printf("{\"delivered\":\"%s\",\"history\":%s}\n", s->last_grant,
               s->history ? "true" : "false");
    return rc;
}
static int distribute(vc_app *a)
{
    vc_state *s = &a->state;
    const porthome_vault_info *v = vault_tree_node_at(s->tree, 0, NULL)->vault;
    size_t total = v->writers_len + v->readers_len;
    while (s->delivery_cursor < total) {
        size_t i = s->delivery_cursor;
        const uint8_t *member = i < v->writers_len ? v->writers[i] : v->readers[i - v->writers_len];
        unsigned cap = i < v->writers_len ? VAULT_CAP_WRITE : VAULT_CAP_READ;
        int duplicate = !memcmp(member, v->owner, 32);
        if (i >= v->writers_len)
            for (size_t j = 0; j < v->writers_len; j++)
                if (!memcmp(member, v->writers[j], 32))
                    duplicate = 1;
        if (duplicate) {
            s->delivery_cursor++;
            int rc = vc_state_save(a);
            if (rc)
                return rc;
            continue;
        }
        int rc = deliver(a, member, cap, 0);
        if (rc)
            return rc;
    }
    s->distribution = 0;
    return vc_state_save(a);
}
int vc_command_revoke(vc_app *a)
{
    vc_state *s = &a->state;
    int rc = 0;
    if (a->options.resume) {
        if (s->phase != VC_PHASE_REVOKE && !s->migrating)
            return VAULT_ERR_ARG;
        if (!s->phase) {
            s->phase = VC_PHASE_REVOKE;
            s->rotation = 0;
            s->eager = a->options.eager;
            if ((rc = vc_state_save(a)))
                return rc;
        }
    } else {
        if (s->phase)
            return VAULT_ERR_AGAIN;
        if ((rc = latest_owner(a)))
            return rc;
        if (porthome_from_hex64(a->options.member_hex, s->member))
            return VAULT_ERR_ARG;
        porthome_head *p = NULL;
        rc = previous(a, &p);
        if (rc)
            return rc;
        vault_tree *next = NULL;
        rc = vault_tree_revoke(s->tree, p, a->pub, s->member, next_time(p), &next);
        head_free(p);
        if (rc)
            return rc;
        vault_tree_free(s->tree);
        s->tree = next;
        s->phase = VC_PHASE_REVOKE;
        s->rotation = 1;
        s->migrating = 1;
        s->eager = a->options.eager;
        if ((rc = vc_state_save(a)))
            return rc;
    }
    if ((rc = vc_reconcile_blob(a)))
        return rc;
    if (s->outgoing && (rc = finish_outgoing(a)))
        return rc;
    if (s->distribution) {
        if ((rc = distribute(a)))
            return rc;
        goto done;
    }
    vault_tree_reseal_result *result = NULL;
    vault_blob_io io = {a, vc_blob_read, vc_blob_put};
    a->chunk_io = 1;
    rc = vault_tree_reseal(s->tree, s->eager, a->options.budget, &io, &result);
    a->chunk_io = 0;
    if (result)
        s->migrating = result->remaining != 0;
    size_t completed = result ? result->completed : 0;
    vault_tree_reseal_result_free(result);
    int saved = vc_state_save(a);
    if (saved)
        return saved;
    if (rc && rc != VAULT_ERR_BUDGET)
        return rc;
    if (s->eager && rc == VAULT_ERR_BUDGET)
        return rc;
    if (s->rotation || s->needs_publication || completed) {
        rc = seal_publish(a);
        if (rc)
            return rc;
    }
    if (s->distribution && (rc = distribute(a)))
        return rc;
done:
    s->phase = VC_PHASE_NONE;
    if ((rc = vc_state_save(a)))
        return rc;
    fprintf(stderr, "Service allowlist removal is an operator action; no service administration "
                    "API is assumed.\n");
    printf("{\"epoch\":%llu,\"remaining\":%s,\"service_allowlist\":\"operator-action-required\"}\n",
           (unsigned long long)vault_tree_node_at(s->tree, 0, NULL)->epoch,
           s->migrating ? "true" : "false");
    return s->migrating ? VAULT_ERR_BUDGET : 0;
}
int vc_command_fold(vc_app *a)
{
    vault_record **r = NULL;
    size_t n = 0;
    vault_fold_result *f = NULL;
    int rc = vc_collect_records(a, &r, &n, &f);
    if (rc && rc != VAULT_ERR_CONFLICT)
        goto done;
    printf("{\"owner_view\":");
    if (f->view == SIZE_MAX)
        printf("null");
    else
        printf("\"%s\"", nostr_event_get_id(a->state.heads[f->view].event));
    printf(",\"frontier\":[");
    for (size_t i = 0; i < f->frontier_len; i++)
        printf("%s\"%s\"", i ? "," : "", nostr_event_get_id(a->state.heads[f->frontier[i]].event));
    printf("],\"conflicts\":[");
    for (size_t i = 0; i < f->conflicts_len; i++) {
        char writer[65];
        porthome_hex64(f->conflicts[i].writer, writer);
        printf("%s{\"path_enc\":\"%s\",\"writer\":\"%s\"}", i ? "," : "", f->conflicts[i].path_enc,
               writer);
    }
    printf("],\"retained_roots\":%zu}\n", f->retained_roots_len);
done:
    vault_fold_result_free(f);
    vc_records_free(r, n);
    return rc;
}
int vc_command_fsck(vc_app *a)
{
    vault_record **r = NULL;
    size_t n = 0;
    vault_fold_result *f = NULL;
    int rc = vc_collect_records(a, &r, &n, &f);
    if (rc && rc != VAULT_ERR_CONFLICT)
        goto done;
    rc = 0;
    size_t nodes = 0, chunks = 0;
    for (size_t i = 0; i < n; i++) {
        vault_record *again = NULL;
        vault_tree *tree = NULL;
        rc = vc_record_load(a, a->state.heads[i].event, a->state.heads[i].epoch, &again, &tree);
        if (rc)
            break;
        vault_blob_io io = {a, vc_blob_read, vc_blob_put};
        vault_tree_check_result stats;
        rc = vault_tree_check(tree, &io, &stats);
        vault_record_free(again);
        vault_tree_free(tree);
        if (rc)
            break;
        nodes += stats.nodes;
        chunks += stats.chunks;
    }
    if (!rc)
        printf("{\"healthy\":true,\"snapshots\":%zu,\"nodes\":%zu,\"chunks\":%zu}\n", n, nodes,
               chunks);
done:
    vault_fold_result_free(f);
    vc_records_free(r, n);
    return rc;
}
