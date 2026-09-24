/*
 * test_porthome_notify.c — unit test for the notification throttler
 * and quiet-hours logic (bead nostrc-h10m.1).
 *
 * SPDX-License-Identifier: MIT
 *
 * Uses a fixed-clock seam so the throttle window is deterministic and
 * a stub backend so no real notify-send fork happens.
 */

#define _GNU_SOURCE
#include "nh_porthome_notify.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Fixed-clock harness. */
typedef struct {
    int64_t now;
} clock_t_;
static int64_t clock_fn(void *ud) {
    return ((clock_t_ *)ud)->now;
}

/* Backend hook counts deliveries. */
typedef struct {
    int n;
    char last_summary[128];
    char last_body[128];
} backend_t;
static void backend_fn(void *ud, const char *app, const char *icon,
                       const char *summary, const char *body) {
    (void)app; (void)icon;
    backend_t *b = ud;
    b->n++;
    snprintf(b->last_summary, sizeof b->last_summary, "%s", summary ? summary : "");
    snprintf(b->last_body,    sizeof b->last_body,    "%s", body    ? body    : "");
}

/* Record hook counts records. */
typedef struct {
    int n_total;
    int n_delivered;
    int n_suppressed;
} record_t;
static void record_fn(void *ud, int64_t ts, nh_notify_category cat,
                      const char *key, const char *summary,
                      const char *body, bool delivered) {
    (void)ts; (void)cat; (void)key; (void)summary; (void)body;
    record_t *r = ud;
    r->n_total++;
    if (delivered) r->n_delivered++;
    else           r->n_suppressed++;
}

static void test_parse_hhmm(void) {
    int s = -9, e = -9;
    assert(nh_porthome_notify_parse_hhmm("22:00-06:00", &s, &e) == 0);
    assert(s == 22 * 60);
    assert(e == 6 * 60);
    assert(nh_porthome_notify_parse_hhmm("off", &s, &e) == 0);
    assert(s == -1 && e == -1);
    assert(nh_porthome_notify_parse_hhmm("nope", &s, &e) == -1);
    assert(nh_porthome_notify_parse_hhmm("25:00-01:00", &s, &e) == -1);
    printf("parse_hhmm OK\n");
}

static void test_throttle_same_key(void) {
    backend_t bk = {0};
    record_t  rc = {0};
    clock_t_ ck = { .now = 1000000 };
    nh_porthome_notifier *n = nh_porthome_notifier_new();
    nh_porthome_notifier_set_backend(n, backend_fn, &bk);
    nh_porthome_notifier_set_clock  (n, clock_fn,   &ck);
    nh_porthome_notifier_set_record (n, record_fn,  &rc);
    nh_porthome_notifier_set_window (n, 600);

    int r1 = nh_porthome_notify(n, "app", "icon",
                                "sweep dropped 3", "abc",
                                NH_NOTIFY_CAT_SWEEP, "obj-123");
    ck.now += 100;
    int r2 = nh_porthome_notify(n, "app", "icon",
                                "sweep dropped 3", "abc",
                                NH_NOTIFY_CAT_SWEEP, "obj-123");
    assert(r1 == 1 && r2 == 0);
    assert(bk.n == 1);
    assert(rc.n_total == 2);
    assert(rc.n_delivered == 1);
    assert(rc.n_suppressed == 1);

    /* Different key → not throttled. */
    ck.now += 100;
    int r3 = nh_porthome_notify(n, "app", "icon",
                                "sweep dropped 3", "abc",
                                NH_NOTIFY_CAT_SWEEP, "obj-999");
    assert(r3 == 1);
    assert(bk.n == 2);

    /* Different category, same key → not throttled either. */
    ck.now += 10;
    int r4 = nh_porthome_notify(n, "app", "icon",
                                "conflict", "def",
                                NH_NOTIFY_CAT_CONFLICT, "obj-123");
    assert(r4 == 1);
    assert(bk.n == 3);

    /* After the window elapses, same key delivers again. */
    ck.now += 601;
    int r5 = nh_porthome_notify(n, "app", "icon",
                                "sweep dropped 3", "abc",
                                NH_NOTIFY_CAT_SWEEP, "obj-123");
    assert(r5 == 1);
    assert(bk.n == 4);

    nh_porthome_notifier_free(n);
    printf("throttle_same_key OK\n");
}

static void test_quiet_hours(void) {
    backend_t bk = {0};
    record_t  rc = {0};
    /* now = 2026-01-01 23:15 local — inside a 22:00-06:00 window. */
    struct tm tm = { .tm_year = 126, .tm_mon = 0, .tm_mday = 1,
                     .tm_hour = 23, .tm_min = 15, .tm_sec = 0,
                     .tm_isdst = -1 };
    clock_t_ ck = { .now = (int64_t)mktime(&tm) };
    nh_porthome_notifier *n = nh_porthome_notifier_new();
    nh_porthome_notifier_set_backend(n, backend_fn, &bk);
    nh_porthome_notifier_set_clock  (n, clock_fn,   &ck);
    nh_porthome_notifier_set_record (n, record_fn,  &rc);
    nh_porthome_notifier_set_quiet_hours(n, 22 * 60, 6 * 60);

    int r = nh_porthome_notify(n, "app", "icon",
                               "conflict", "x",
                               NH_NOTIFY_CAT_CONFLICT, "k");
    assert(r == 0);
    assert(bk.n == 0);
    assert(rc.n_total == 1);
    assert(rc.n_suppressed == 1);

    /* Now shift the clock outside the window (10:00). */
    struct tm tm2 = { .tm_year = 126, .tm_mon = 0, .tm_mday = 2,
                      .tm_hour = 10, .tm_min = 0, .tm_sec = 0,
                      .tm_isdst = -1 };
    ck.now = (int64_t)mktime(&tm2);
    r = nh_porthome_notify(n, "app", "icon",
                           "conflict", "x",
                           NH_NOTIFY_CAT_CONFLICT, "k2");
    assert(r == 1);
    assert(bk.n == 1);

    nh_porthome_notifier_free(n);
    printf("quiet_hours OK\n");
}

int main(void) {
    test_parse_hhmm();
    test_throttle_same_key();
    test_quiet_hours();
    printf("test_porthome_notify: all tests passed\n");
    return 0;
}
