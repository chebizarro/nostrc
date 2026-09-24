/*
 * test_porthome_status_daemon_writers.c — Phase 5 h10m.1.1
 *   (bead nostrc-h10m.1.1): syncd's status writer + notifier
 *   record_fn wiring produces a well-formed porthome-status.json.
 *
 * SPDX-License-Identifier: MIT
 *
 * Covers:
 *   1. Serial writes from the syncd writer replace the "syncd" key
 *      atomically and never disturb a peer "fuse" key.
 *   2. Wiring nh_porthome_notifier_set_record → notify_record adapter
 *      appends into recent[] and the render carries the entries in
 *      most-recent-first order, capped at NH_SYNCD_STATUS_RECENT_CAP.
 *   3. Field mutators land in the emitted body.
 */

#define _GNU_SOURCE
#include "nh_syncd_status.h"
#include "nh_porthome_notify.h"
#include "nh_porthome_status.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void seed_fuse_key(const char *path) {
    int rc = nh_porthome_status_write_key(
        path, "fuse",
        "{\"mounted\":true,\"mountpoint\":\"/home/x/Portable\","
         "\"generation\":42}");
    assert(rc == 0);
}

static void test_syncd_key_replaces_and_preserves_peers(const char *path) {
    seed_fuse_key(path);
    nh_syncd_status_writer *w = nh_syncd_status_writer_new();
    nh_syncd_status_set_state(w, NH_SYNCD_STATE_PULLING);
    nh_syncd_status_set_last_push_gen(w, 100);
    nh_syncd_status_set_last_pull_gen(w, 101);
    nh_syncd_status_set_pinned_count(w, 7);
    nh_syncd_status_set_cache_bytes(w, 12345);
    nh_syncd_status_set_cache_quota(w, 67890, "env");
    nh_syncd_status_set_evict_rate(w, 3);
    nh_syncd_status_set_last_error(w, "");
    assert(nh_syncd_status_emit(w, path) == 0);

    /* Change some fields and re-emit. */
    nh_syncd_status_set_state(w, NH_SYNCD_STATE_RECONCILING);
    nh_syncd_status_set_last_pull_gen(w, 202);
    assert(nh_syncd_status_emit(w, path) == 0);

    char *body = NULL; size_t body_len = 0;
    assert(nh_porthome_status_read(path, &body, &body_len) == 0);
    /* fuse key survives. */
    assert(strstr(body, "\"fuse\":") != NULL);
    assert(strstr(body, "\"mountpoint\":\"/home/x/Portable\"") != NULL);
    /* syncd key reflects the last render. */
    assert(strstr(body, "\"state\":\"reconciling\"") != NULL);
    assert(strstr(body, "\"last_pull_gen\":202") != NULL);
    assert(strstr(body, "\"cache_quota_source\":\"env\"") != NULL);
    assert(strstr(body, "\"cache_quota_bytes\":67890") != NULL);
    /* Old value gone. */
    assert(strstr(body, "\"last_pull_gen\":101") == NULL);
    free(body);
    nh_syncd_status_writer_free(w);
    printf("syncd_key_replaces_and_preserves_peers OK\n");
}

static void test_recent_ring_rollover(const char *path) {
    (void)unlink(path);
    seed_fuse_key(path);
    nh_syncd_status_writer *w = nh_syncd_status_writer_new();
    /* Feed 25 events; recent[] cap is 20 → only the last 20 survive,
     * and slot 0 in the render must be the MOST RECENT. */
    for (int i = 0; i < 25; ++i) {
        char summary[64];
        snprintf(summary, sizeof summary, "sweep-%02d", i);
        nh_syncd_status_notify_record(w,
                                      (int64_t)(1000 + i),
                                      NH_NOTIFY_CAT_SWEEP,
                                      summary, /* key   */
                                      summary, /* summary */
                                      "",      /* body  */
                                      true);
    }
    /* Explicit emit (notify_record already emits, but be defensive). */
    assert(nh_syncd_status_emit(w, path) == 0);
    char *body = NULL; size_t body_len = 0;
    assert(nh_porthome_status_read(path, &body, &body_len) == 0);
    /* Most recent (i=24) must appear BEFORE i=23 in the array. */
    const char *p24 = strstr(body, "\"summary\":\"sweep-24\"");
    const char *p23 = strstr(body, "\"summary\":\"sweep-23\"");
    const char *p05 = strstr(body, "\"summary\":\"sweep-05\"");
    const char *p04 = strstr(body, "\"summary\":\"sweep-04\"");
    assert(p24 && p23 && p24 < p23);
    /* Slot 5 (i=5) is at the boundary of the 20-cap: rendered.
     * Slot 4 (i=4) has been evicted. */
    assert(p05 != NULL);
    assert(p04 == NULL);
    /* Category slug matches. */
    assert(strstr(body, "\"cat\":\"sweep\"") != NULL);
    free(body);
    nh_syncd_status_writer_free(w);
    printf("recent_ring_rollover OK\n");
}

static void test_concurrent_fuse_untouched(const char *path) {
    /* Confirm the syncd writer never overwrites the fuse key even
     * when writes interleave: seed fuse, mutate syncd, re-read fuse. */
    (void)unlink(path);
    nh_syncd_status_writer *w = nh_syncd_status_writer_new();
    nh_syncd_status_set_state(w, NH_SYNCD_STATE_IDLE);
    assert(nh_syncd_status_emit(w, path) == 0);
    /* Concurrent (serial-in-test) fuse writer lands its body. */
    assert(nh_porthome_status_write_key(path, "fuse",
             "{\"mounted\":true,\"generation\":9}") == 0);
    /* syncd re-emit must not touch fuse. */
    nh_syncd_status_set_state(w, NH_SYNCD_STATE_PUSHING);
    assert(nh_syncd_status_emit(w, path) == 0);
    char *body = NULL; size_t body_len = 0;
    assert(nh_porthome_status_read(path, &body, &body_len) == 0);
    assert(strstr(body, "\"fuse\":") != NULL);
    assert(strstr(body, "\"generation\":9") != NULL);
    assert(strstr(body, "\"state\":\"pushing\"") != NULL);
    free(body);
    nh_syncd_status_writer_free(w);
    printf("concurrent_fuse_untouched OK\n");
}


static void test_recent_head_dedupe(const char *path) {
    /* Bead nostrc-u40q: 5 consecutive PUSH-DONE notifications with the
     * same (cat, key_hash8) collapse into a single head slot with
     * count=5; then a DIFFERENT event advances the head and drops the
     * count field again (count==1 → omitted for backward compat). */
    (void)unlink(path);
    nh_syncd_status_writer *w = nh_syncd_status_writer_new();

    for (int i = 0; i < 5; ++i) {
        nh_syncd_status_notify_record(w,
                                      (int64_t)(2000 + i),
                                      NH_NOTIFY_CAT_SWEEP,
                                      "same-key",   /* key    */
                                      "push-done",  /* summary */
                                      "",           /* body   */
                                      true);
    }
    assert(nh_syncd_status_emit(w, path) == 0);
    {
        char *body = NULL; size_t body_len = 0;
        assert(nh_porthome_status_read(path, &body, &body_len) == 0);
        /* Head carries count=5 for the collapsed burst; ts is the
         * newest timestamp (2004). */
        assert(strstr(body, "\"count\":5") != NULL);
        assert(strstr(body, "\"ts\":2004") != NULL);
        /* And only ONE array entry — the older duplicates were
         * folded into the head. Count opening '{' inside recent[]. */
        const char *arr = strstr(body, "\"recent\":[");
        assert(arr != NULL);
        const char *arr_end = strchr(arr, ']');
        assert(arr_end != NULL);
        int braces = 0;
        for (const char *p = arr; p < arr_end; ++p)
            if (*p == '{') braces++;
        assert(braces == 1);
        free(body);
    }

    /* Push a DIFFERENT event → new head, no count field. */
    nh_syncd_status_notify_record(w, /*ts=*/2010,
                                  NH_NOTIFY_CAT_SWEEP,
                                  "other-key",
                                  "push-done-2",
                                  "", true);
    assert(nh_syncd_status_emit(w, path) == 0);
    {
        char *body = NULL; size_t body_len = 0;
        assert(nh_porthome_status_read(path, &body, &body_len) == 0);
        /* Two array entries now — head is the newest (other-key),
         * the collapsed burst is at recent[1]. */
        const char *pHead = strstr(body, "\"summary\":\"push-done-2\"");
        const char *pTail = strstr(body, "\"summary\":\"push-done\"");
        assert(pHead && pTail && pHead < pTail);
        /* Head entry has NO count field (count==1 → omitted for
         * backward compatibility with pre-u40q consumers). Look at
         * the substring bounded by the head entry's { ... }. */
        const char *arr = strstr(body, "\"recent\":[");
        assert(arr != NULL);
        const char *head_open = strchr(arr, '{');
        assert(head_open != NULL);
        const char *head_close = strchr(head_open, '}');
        assert(head_close != NULL);
        size_t head_len = (size_t)(head_close - head_open);
        char head_slice[512];
        if (head_len >= sizeof head_slice) head_len = sizeof head_slice - 1;
        memcpy(head_slice, head_open, head_len);
        head_slice[head_len] = '\0';
        assert(strstr(head_slice, "\"count\":") == NULL);
        /* And the older (deduped) entry preserves its count=5. */
        assert(strstr(body, "\"count\":5") != NULL);
        free(body);
    }

    /* One more same-key hit AFTER the head advanced → head (other-key)
     * gets count=2 rather than a new slot; the older collapsed entry
     * is unchanged. */
    nh_syncd_status_notify_record(w, /*ts=*/2011,
                                  NH_NOTIFY_CAT_SWEEP,
                                  "other-key",
                                  "push-done-2b",
                                  "", true);
    assert(nh_syncd_status_emit(w, path) == 0);
    {
        char *body = NULL; size_t body_len = 0;
        assert(nh_porthome_status_read(path, &body, &body_len) == 0);
        /* Head slot updates its ts + summary and shows count=2. */
        assert(strstr(body, "\"count\":2") != NULL);
        assert(strstr(body, "\"ts\":2011") != NULL);
        assert(strstr(body, "\"summary\":\"push-done-2b\"") != NULL);
        /* Older collapsed burst still there. */
        assert(strstr(body, "\"count\":5") != NULL);
        free(body);
    }

    /* Different CATEGORY with the same key must NOT collapse. */
    nh_syncd_status_notify_record(w, /*ts=*/2020,
                                  NH_NOTIFY_CAT_CONFLICT,
                                  "other-key",  /* same key text */
                                  "conflict",
                                  "", true);
    assert(nh_syncd_status_emit(w, path) == 0);
    {
        char *body = NULL; size_t body_len = 0;
        assert(nh_porthome_status_read(path, &body, &body_len) == 0);
        /* New head has a different category slug — must not carry
         * a count field yet, and the SWEEP burst is still visible
         * further down the array. */
        assert(strstr(body, "\"cat\":\"conflict\"") != NULL);
        assert(strstr(body, "\"count\":5") != NULL);
        free(body);
    }

    nh_syncd_status_writer_free(w);
    printf("recent_head_dedupe OK\n");
}

int main(void) {
    char tmpl[] = "/tmp/nh_porthome_status_daemon.XXXXXX";
    int fd = mkstemp(tmpl); assert(fd >= 0);
    close(fd); unlink(tmpl);

    test_syncd_key_replaces_and_preserves_peers(tmpl);
    test_recent_ring_rollover(tmpl);
    test_concurrent_fuse_untouched(tmpl);
    test_recent_head_dedupe(tmpl);

    unlink(tmpl);
    char lock[300]; snprintf(lock, sizeof lock, "%s.lock", tmpl);
    unlink(lock);
    printf("test_porthome_status_daemon_writers: all tests passed\n");
    return 0;
}
