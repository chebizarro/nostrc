/*
 * test_notify_pump.c — nostrc-a16c: the notify connector waits on the
 * group and DM channels at once.
 *
 * SPDX-License-Identifier: MIT
 *
 * Before the fix the connector blocked in go_channel_receive on the group
 * channel before ever looking at the DM channel, so with a quiet group
 * subscription a DM notification waited for the next group event. Here the
 * group channel stays silent while a DM is sent; it must be delivered
 * promptly. Also: a closed channel drops out without stalling the other,
 * the pump returns once every channel is closed, and `stop` is honoured
 * within one tick.
 */
#include "notify_pump.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  pthread_mutex_t mu;
  size_t n;
  size_t idx[16];
  void *item[16];
} Seen;

static void on_item(void *ud, size_t idx, void *item) {
  Seen *s = ud;
  pthread_mutex_lock(&s->mu);
  if (s->n < 16) { s->idx[s->n] = idx; s->item[s->n] = item; s->n++; }
  pthread_mutex_unlock(&s->mu);
}

typedef struct {
  GoChannel *chans[2];
  _Atomic int stop;
  uint64_t tick_ms;
  Seen seen;
  uint64_t delivered;
} Rig;

static void *pump_thread(void *arg) {
  Rig *r = arg;
  r->delivered = nsn_pump_run(r->chans, 2, &r->stop, r->tick_ms, on_item, &r->seen);
  return NULL;
}

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Wait until `want` items were seen; returns elapsed ms or UINT64_MAX. */
static uint64_t wait_seen(Seen *s, size_t want, uint64_t budget_ms) {
  uint64_t t0 = now_ms();
  for (;;) {
    pthread_mutex_lock(&s->mu);
    size_t n = s->n;
    pthread_mutex_unlock(&s->mu);
    if (n >= want) return now_ms() - t0;
    if (now_ms() - t0 > budget_ms) return UINT64_MAX;
    usleep(2000);
  }
}

static int dm1, grp1, grp2;

static void t_quiet_groups_do_not_delay_dms(void) {
  Rig r;
  memset(&r, 0, sizeof r);
  pthread_mutex_init(&r.seen.mu, NULL);
  r.chans[0] = go_channel_create(4); /* groups: stays quiet */
  r.chans[1] = go_channel_create(4); /* DMs */
  /* A long tick: prompt delivery must come from the select, not a poll. */
  r.tick_ms = 5000;
  pthread_t th;
  assert(pthread_create(&th, NULL, pump_thread, &r) == 0);
  usleep(50000); /* let the pump block */

  assert(go_channel_send(r.chans[1], &dm1) == 0);
  uint64_t ms = wait_seen(&r.seen, 1, 1000);
  printf("  DM delivered after %llu ms with the group channel silent\n",
         (unsigned long long)ms);
  assert(ms != UINT64_MAX && ms < 500);
  assert(r.seen.idx[0] == 1 && r.seen.item[0] == &dm1);

  assert(go_channel_send(r.chans[0], &grp1) == 0);
  assert(wait_seen(&r.seen, 2, 1000) != UINT64_MAX);
  assert(r.seen.idx[1] == 0 && r.seen.item[1] == &grp1);

  /* One channel closing must not stall (or spin on) the other. */
  go_channel_close(r.chans[1]);
  usleep(20000);
  assert(go_channel_send(r.chans[0], &grp2) == 0);
  assert(wait_seen(&r.seen, 3, 1000) != UINT64_MAX);
  assert(r.seen.idx[2] == 0 && r.seen.item[2] == &grp2);

  /* All channels closed: the pump returns on its own. */
  go_channel_close(r.chans[0]);
  uint64_t t0 = now_ms();
  assert(pthread_join(th, NULL) == 0);
  assert(now_ms() - t0 < 1000);
  assert(r.delivered == 3);
  go_channel_unref(r.chans[0]);
  go_channel_unref(r.chans[1]);
  printf("t_quiet_groups_do_not_delay_dms OK\n");
}

static void t_stop_within_a_tick(void) {
  Rig r;
  memset(&r, 0, sizeof r);
  pthread_mutex_init(&r.seen.mu, NULL);
  r.chans[0] = go_channel_create(4);
  r.chans[1] = NULL; /* DMs disabled in prefs */
  r.tick_ms = 100;
  pthread_t th;
  assert(pthread_create(&th, NULL, pump_thread, &r) == 0);
  usleep(50000);
  atomic_store(&r.stop, 1);
  uint64_t t0 = now_ms();
  assert(pthread_join(th, NULL) == 0);
  assert(now_ms() - t0 < 1000);
  assert(r.delivered == 0);
  go_channel_close(r.chans[0]);
  go_channel_unref(r.chans[0]);
  printf("t_stop_within_a_tick OK\n");
}

int main(void) {
  t_quiet_groups_do_not_delay_dms();
  t_stop_within_a_tick();
  /* No channels at all: returns immediately. */
  GoChannel *none[2] = {NULL, NULL};
  assert(nsn_pump_run(none, 2, NULL, 10, on_item, NULL) == 0);
  printf("test_notify_pump: ALL OK\n");
  return 0;
}
