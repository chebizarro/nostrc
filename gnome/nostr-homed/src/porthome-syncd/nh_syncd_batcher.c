/*
 * nh_syncd_batcher.c — debounce + coalesce state machine (§6.2).
 *
 * SPDX-License-Identifier: MIT
 *
 * Model:
 *   - `t_first` records when the current run of activity began.
 *   - `t_last`  records the most recent push.
 *   - IDLE when the pending set is empty.
 *   - COALESCING when 0 < (now - t_last) < debounce_ns AND
 *                     (now - t_first) < max_hold_ns.
 *   - READY when (now - t_last) >= debounce_ns OR
 *               (now - t_first) >= max_hold_ns.
 *
 * The batcher is CLOCK-INJECTED. The caller supplies now_fn, which
 * lets tests advance a virtual clock in single-nanosecond steps.
 *
 * The pending set is a jansson object (rel_path -> kind int). This
 * gives us dedup + linear serialization for free. The final batch
 * hands the caller a stable array copy.
 */

#include "nh_syncd.h"

#include <jansson.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct nh_syncd_batcher {
    nh_syncd_now_fn now_fn;
    void           *now_ud;
    uint64_t        debounce_ns;
    uint64_t        max_hold_ns;
    /* Pending set (rel_path -> integer changekind). */
    json_t         *pending;
    uint64_t        t_first;
    uint64_t        t_last;
    /* Monotonic batch id, incremented on every take(). */
    uint64_t        next_batch_id;
};

struct nh_syncd_batch {
    uint64_t id;
    struct {
        char                *rel;
        nh_syncd_change_kind k;
    } *items;
    size_t   items_n;
};

int nh_syncd_batcher_new(nh_syncd_now_fn now_fn, void *now_ud,
                         uint64_t debounce_ns,
                         uint64_t max_hold_ns,
                         nh_syncd_batcher **out)
{
    if (!now_fn || !out) return NH_SYNCD_ERR_ARG;
    nh_syncd_batcher *b = calloc(1, sizeof *b);
    if (!b) return NH_SYNCD_ERR_OOM;
    b->now_fn = now_fn;
    b->now_ud = now_ud;
    b->debounce_ns = debounce_ns ? debounce_ns : (NH_SYNCD_DEBOUNCE_SECS * 1000000000ull);
    b->max_hold_ns = max_hold_ns ? max_hold_ns : (NH_SYNCD_FORCE_FLUSH_SECS * 1000000000ull);
    b->pending = json_object();
    if (!b->pending) { free(b); return NH_SYNCD_ERR_OOM; }
    b->next_batch_id = 1;
    *out = b;
    return NH_SYNCD_OK;
}

void nh_syncd_batcher_free(nh_syncd_batcher *b) {
    if (!b) return;
    if (b->pending) json_decref(b->pending);
    free(b);
}

/* Coalesce rule table:
 *   existing → new    →  final
 *   (none)   → C      →  C
 *   (none)   → M      →  M
 *   (none)   → D      →  D
 *   C        → M      →  C   (a modify after create is still "just create")
 *   C        → D      →  D
 *   M        → C      →  M   (spurious re-create after modify, keep MODIFY)
 *   M        → D      →  D
 *   D        → C      →  C   (moved back in — treat as new content)
 *   D        → M      →  M   (should not happen; be lenient)
 */
static nh_syncd_change_kind coalesce(nh_syncd_change_kind cur,
                                     nh_syncd_change_kind add) {
    if (cur == 0) return add;
    if (cur == NH_SYNCD_CHANGE_CREATE) {
        if (add == NH_SYNCD_CHANGE_MODIFY) return NH_SYNCD_CHANGE_CREATE;
        return add;
    }
    if (cur == NH_SYNCD_CHANGE_MODIFY) {
        if (add == NH_SYNCD_CHANGE_CREATE) return NH_SYNCD_CHANGE_MODIFY;
        return add;
    }
    /* cur == DELETE */
    return add;
}

int nh_syncd_batcher_push(nh_syncd_batcher *b,
                          const char *rel_path,
                          nh_syncd_change_kind kind)
{
    if (!b || !rel_path) return NH_SYNCD_ERR_ARG;
    if (kind != NH_SYNCD_CHANGE_CREATE && kind != NH_SYNCD_CHANGE_MODIFY
        && kind != NH_SYNCD_CHANGE_DELETE) return NH_SYNCD_ERR_ARG;

    uint64_t now = b->now_fn(b->now_ud);
    if (json_object_size(b->pending) == 0) {
        b->t_first = now;
    }
    b->t_last = now;

    nh_syncd_change_kind cur = 0;
    json_t *existing = json_object_get(b->pending, rel_path);
    if (json_is_integer(existing))
        cur = (nh_syncd_change_kind)json_integer_value(existing);
    nh_syncd_change_kind final_k = coalesce(cur, kind);
    if (json_object_set_new(b->pending, rel_path,
                            json_integer((json_int_t)final_k)) != 0)
        return NH_SYNCD_ERR_OOM;
    return NH_SYNCD_OK;
}

nh_syncd_batcher_state nh_syncd_batcher_poll(nh_syncd_batcher *b) {
    if (!b) return NH_SYNCD_BATCHER_IDLE;
    if (json_object_size(b->pending) == 0) return NH_SYNCD_BATCHER_IDLE;
    uint64_t now = b->now_fn(b->now_ud);
    uint64_t since_last  = now - b->t_last;
    uint64_t since_first = now - b->t_first;
    if (since_last >= b->debounce_ns || since_first >= b->max_hold_ns)
        return NH_SYNCD_BATCHER_READY;
    return NH_SYNCD_BATCHER_COALESCING;
}

uint64_t nh_syncd_batcher_next_tick_ms(nh_syncd_batcher *b) {
    if (!b) return UINT64_MAX;
    if (json_object_size(b->pending) == 0) return UINT64_MAX;
    uint64_t now = b->now_fn(b->now_ud);
    uint64_t since_last  = now - b->t_last;
    uint64_t since_first = now - b->t_first;
    uint64_t rem_d = since_last  >= b->debounce_ns ? 0 : (b->debounce_ns - since_last);
    uint64_t rem_h = since_first >= b->max_hold_ns ? 0 : (b->max_hold_ns - since_first);
    uint64_t rem   = rem_d < rem_h ? rem_d : rem_h;
    /* Round up to nearest ms with saturation. */
    return (rem + 999999ull) / 1000000ull;
}

nh_syncd_batch *nh_syncd_batcher_take(nh_syncd_batcher *b, bool force) {
    if (!b) return NULL;
    if (!force) {
        if (nh_syncd_batcher_poll(b) != NH_SYNCD_BATCHER_READY) return NULL;
    }
    size_t n = (size_t)json_object_size(b->pending);
    nh_syncd_batch *out = calloc(1, sizeof *out);
    if (!out) return NULL;
    out->id = b->next_batch_id++;
    if (n == 0) {
        out->items = NULL;
        out->items_n = 0;
        return out;
    }
    out->items = calloc(n, sizeof *out->items);
    if (!out->items) { free(out); return NULL; }
    size_t idx = 0;
    const char *k; json_t *v;
    json_object_foreach(b->pending, k, v) {
        out->items[idx].rel = strdup(k);
        out->items[idx].k   = (nh_syncd_change_kind)json_integer_value(v);
        if (!out->items[idx].rel) {
            /* Roll back partial state. */
            for (size_t j = 0; j < idx; j++) free(out->items[j].rel);
            free(out->items); free(out);
            return NULL;
        }
        idx++;
    }
    out->items_n = idx;
    /* Reset. */
    json_object_clear(b->pending);
    b->t_first = 0;
    b->t_last  = 0;
    return out;
}

uint64_t nh_syncd_batch_id(const nh_syncd_batch *b)      { return b ? b->id      : 0; }
size_t   nh_syncd_batch_len(const nh_syncd_batch *b)     { return b ? b->items_n : 0; }
int nh_syncd_batch_at(const nh_syncd_batch *b, size_t i,
                      const char **out_rel, nh_syncd_change_kind *out_k) {
    if (!b || i >= b->items_n) return NH_SYNCD_ERR_ARG;
    if (out_rel) *out_rel = b->items[i].rel;
    if (out_k)   *out_k   = b->items[i].k;
    return NH_SYNCD_OK;
}
void nh_syncd_batch_free(nh_syncd_batch *b) {
    if (!b) return;
    for (size_t i = 0; i < b->items_n; i++) free(b->items[i].rel);
    free(b->items);
    free(b);
}
