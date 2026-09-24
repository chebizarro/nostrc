/*
 * test_syncd_batcher.c — unit test for the debounce/coalesce state
 * machine. Uses a caller-controlled virtual clock so timing is
 * deterministic.
 */

#include "nh_syncd.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t g_now_ns;
static uint64_t now_fn(void *ud) { (void)ud; return g_now_ns; }

static void t_debounce(void) {
    nh_syncd_batcher *b = NULL;
    /* 5s debounce, 60s hold — the max-hold branch is exercised in
     * t_max_hold. */
    assert(nh_syncd_batcher_new(now_fn, NULL,
                                5ull * 1000000000ull,
                                60ull * 1000000000ull, &b) == 0);
    /* Idle. */
    assert(nh_syncd_batcher_poll(b) == NH_SYNCD_BATCHER_IDLE);
    assert(nh_syncd_batcher_next_tick_ms(b) == UINT64_MAX);

    g_now_ns = 100ull * 1000000000ull;
    nh_syncd_batcher_push(b, "a.txt", NH_SYNCD_CHANGE_CREATE);
    assert(nh_syncd_batcher_poll(b) == NH_SYNCD_BATCHER_COALESCING);
    /* 5 seconds until debounce expires. */
    assert(nh_syncd_batcher_next_tick_ms(b) == 5000);

    /* A second event 2s later resets the debounce clock. */
    g_now_ns += 2ull * 1000000000ull;
    nh_syncd_batcher_push(b, "b.txt", NH_SYNCD_CHANGE_MODIFY);
    /* Wait 4s — still coalescing (last push was 2s ago after
     * advance, wait was 4s from t=102s → t=106s → since_last=4s < 5s). */
    g_now_ns += 4ull * 1000000000ull;
    assert(nh_syncd_batcher_poll(b) == NH_SYNCD_BATCHER_COALESCING);

    /* 1 more second → 5s since last: READY. */
    g_now_ns += 1ull * 1000000000ull;
    assert(nh_syncd_batcher_poll(b) == NH_SYNCD_BATCHER_READY);
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, false);
    assert(bat != NULL);
    assert(nh_syncd_batch_len(bat) == 2);
    /* After take, idle again. */
    assert(nh_syncd_batcher_poll(b) == NH_SYNCD_BATCHER_IDLE);
    nh_syncd_batch_free(bat);
    nh_syncd_batcher_free(b);
    printf("t_debounce OK\n");
}

static void t_max_hold(void) {
    nh_syncd_batcher *b = NULL;
    /* 5s debounce, 10s max-hold. A sustained trickle of events every
     * 1s should never reset the max-hold; take() must be READY at
     * t + 10s regardless of debounce. */
    assert(nh_syncd_batcher_new(now_fn, NULL,
                                5ull * 1000000000ull,
                                10ull * 1000000000ull, &b) == 0);
    g_now_ns = 1000ull * 1000000000ull;
    nh_syncd_batcher_push(b, "x", NH_SYNCD_CHANGE_MODIFY);
    for (int i = 0; i < 9; i++) {
        g_now_ns += 1ull * 1000000000ull;
        char rel[32]; snprintf(rel, sizeof rel, "y%d", i);
        nh_syncd_batcher_push(b, rel, NH_SYNCD_CHANGE_MODIFY);
        /* debounce ticks reset — still coalescing until t+10s */
        assert(nh_syncd_batcher_poll(b) == NH_SYNCD_BATCHER_COALESCING);
    }
    g_now_ns += 1ull * 1000000000ull;
    assert(nh_syncd_batcher_poll(b) == NH_SYNCD_BATCHER_READY);
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, false);
    assert(bat != NULL);
    assert(nh_syncd_batch_len(bat) == 10);
    nh_syncd_batch_free(bat);
    nh_syncd_batcher_free(b);
    printf("t_max_hold OK\n");
}

static void t_coalesce(void) {
    nh_syncd_batcher *b = NULL;
    assert(nh_syncd_batcher_new(now_fn, NULL, 1000, 100000, &b) == 0);
    g_now_ns = 1;
    /* CREATE + MODIFY = CREATE */
    nh_syncd_batcher_push(b, "a", NH_SYNCD_CHANGE_CREATE);
    nh_syncd_batcher_push(b, "a", NH_SYNCD_CHANGE_MODIFY);
    /* CREATE + DELETE = DELETE */
    nh_syncd_batcher_push(b, "b", NH_SYNCD_CHANGE_CREATE);
    nh_syncd_batcher_push(b, "b", NH_SYNCD_CHANGE_DELETE);
    /* MODIFY + CREATE = MODIFY (spurious re-notify) */
    nh_syncd_batcher_push(b, "c", NH_SYNCD_CHANGE_MODIFY);
    nh_syncd_batcher_push(b, "c", NH_SYNCD_CHANGE_CREATE);
    /* DELETE + CREATE = CREATE (moved back in) */
    nh_syncd_batcher_push(b, "d", NH_SYNCD_CHANGE_DELETE);
    nh_syncd_batcher_push(b, "d", NH_SYNCD_CHANGE_CREATE);
    g_now_ns += 2000; /* > debounce_ns=1000 */
    assert(nh_syncd_batcher_poll(b) == NH_SYNCD_BATCHER_READY);
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, false);
    assert(nh_syncd_batch_len(bat) == 4);
    /* Verify each coalesced kind. */
    int seen_a = 0, seen_b = 0, seen_c = 0, seen_d = 0;
    for (size_t i = 0; i < nh_syncd_batch_len(bat); i++) {
        const char *rel = NULL; nh_syncd_change_kind k = 0;
        assert(nh_syncd_batch_at(bat, i, &rel, &k) == 0);
        if (!strcmp(rel, "a")) { assert(k == NH_SYNCD_CHANGE_CREATE); seen_a = 1; }
        else if (!strcmp(rel, "b")) { assert(k == NH_SYNCD_CHANGE_DELETE); seen_b = 1; }
        else if (!strcmp(rel, "c")) { assert(k == NH_SYNCD_CHANGE_MODIFY); seen_c = 1; }
        else if (!strcmp(rel, "d")) { assert(k == NH_SYNCD_CHANGE_CREATE); seen_d = 1; }
    }
    assert(seen_a && seen_b && seen_c && seen_d);
    nh_syncd_batch_free(bat);
    nh_syncd_batcher_free(b);
    printf("t_coalesce OK\n");
}

static void t_force_take_empty(void) {
    nh_syncd_batcher *b = NULL;
    assert(nh_syncd_batcher_new(now_fn, NULL, 5ull*1000000000ull, 60ull*1000000000ull, &b) == 0);
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, true);
    assert(bat != NULL);
    assert(nh_syncd_batch_len(bat) == 0);
    /* Batch id starts at 1 and increments per take. */
    assert(nh_syncd_batch_id(bat) == 1);
    nh_syncd_batch_free(bat);
    nh_syncd_batcher_free(b);
    printf("t_force_take_empty OK\n");
}

int main(void) {
    t_debounce();
    t_max_hold();
    t_coalesce();
    t_force_take_empty();
    printf("test_syncd_batcher: OK\n");
    return 0;
}
