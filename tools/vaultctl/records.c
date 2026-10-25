/* SPDX-License-Identifier: MIT */
#include "vc.h"
#include <stdlib.h>
#include <string.h>
int vc_record_load(vc_app *a, const NostrEvent *e, uint64_t epoch, vault_record **out,
                   vault_tree **tree)
{
    porthome_ring_t ring;
    const porthome_dir_node *root = vault_tree_node_at(a->state.tree, 0, &ring);
    if (!root || !root->vault)
        return VAULT_ERR_ARG;
    vault_blob_io io = {a, vc_blob_read, vc_blob_put};
    porthome_ring_t history;
    vault_ring owned = {0};
    int rc = vc_held_ring(a, root->node_id, epoch, &history);
    if (!rc)
        rc = vault_ring_copy(&owned, &history);
    if (rc)
        return rc;
    ring = vault_ring_view(&owned);
    rc = vault_record_fetch(e, root->vault->vault_id, root->node_id, &ring, epoch, vc_held_ring, a,
                            &io, out, tree);
    vault_ring_dispose(&owned);
    if (rc)
        vc_error(a, "manifest closure fetch failed (error %d)", rc);
    if (!rc && vault_record_head(*out)->has_ref_state) {
        vault_record_free(*out);
        vault_tree_free(*tree);
        *out = NULL;
        *tree = NULL;
        vc_error(a, "ref_state reachable: its codec is not available; check is incomplete");
        return VAULT_ERR_INCOMPLETE;
    }
    return rc;
}
void vc_records_free(vault_record **records, size_t count)
{
    for (size_t i = 0; i < count; i++)
        vault_record_free(records[i]);
    free(records);
}
int vc_collect_records(vc_app *a, vault_record ***out, size_t *count, vault_fold_result **fold)
{
    *out = NULL;
    *count = 0;
    *fold = NULL;
    vc_state *s = &a->state;
    if (s->phase != VC_PHASE_NONE || s->outgoing || s->pending_blob)
        return VAULT_ERR_AGAIN;
    const porthome_dir_node *root = vault_tree_node_at(s->tree, 0, NULL);
    int rc = vc_query_heads(a, root->vault);
    if (rc)
        return rc;
    vault_record **records = calloc(s->heads_len, sizeof *records);
    if (!records)
        return VAULT_ERR_OOM;
    size_t n = s->heads_len;
    for (size_t i = 0; i < n; i++) {
        vault_tree *t = NULL;
        rc = vc_record_load(a, s->heads[i].event, s->heads[i].epoch, &records[i], &t);
        vault_tree_free(t);
        if (rc) {
            vc_records_free(records, n);
            return rc;
        }
    }
    rc = vault_fold(root->vault, (const vault_record *const *)records, n, fold);
    if (rc && rc != VAULT_ERR_CONFLICT) {
        vc_error(a, "fold requires a complete signed parent closure (%zu heads held; error %d)", n,
                 rc);
        vc_records_free(records, n);
        return rc;
    }
    *out = records;
    *count = n;
    int saved = vc_state_save(a);
    return saved ? saved : rc;
}
