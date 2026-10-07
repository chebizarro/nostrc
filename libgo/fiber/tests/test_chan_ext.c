/* nostrc-y1y8n scratch check: fiber receiving on an unbuffered gof_chan,
 * fed by an OS thread, GOF_NWORKERS=4. Stalled 10/10 before the broadcast
 * fix. */
#include "../include/libgo/fiber_chan.h"
#include "../include/libgo/fiber.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define N 200
static gof_chan_t *g_ch;
static int g_got = 0;

static void recv_fiber(void *arg) {
  (void)arg;
  for (int i = 0; i < N; i++) {
    void *v = NULL;
    int rc = gof_chan_recv(g_ch, &v);
    assert(rc == 0);
    (void)v;
    g_got++;
  }
}

static void *send_thread(void *arg) {
  (void)arg;
  for (int i = 0; i < N; i++) {
    int rc = gof_chan_send(g_ch, (void*)(long)i);
    assert(rc == 0);
    struct timespec ts = { 0, 1000 * 1000 }; /* 1ms */
    nanosleep(&ts, NULL);
  }
  return NULL;
}

int main(void) {
  gof_init(0);
  g_ch = gof_chan_make(0); /* unbuffered rendezvous */
  assert(g_ch);
  gof_spawn(recv_fiber, NULL, 0);
  pthread_t th;
  assert(pthread_create(&th, NULL, send_thread, NULL) == 0);
  gof_run();
  pthread_join(th, NULL);
  assert(g_got == N);
  printf("test_chan_ext: OK (%d)\n", g_got);
  return 0;
}
