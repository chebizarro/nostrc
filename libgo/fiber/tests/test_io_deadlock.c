/* nostrc-bme1g regression: a fiber blocked in gof_read on a pipe while an
 * OS thread writes every ~2ms used to be able to deadlock the scheduler:
 * the worker idle loop held S.mu while gof_io_have_waiters() took io_mu,
 * while the poller's on_ready held io_mu and its wake took S.mu (ABBA).
 *
 * Before the fix this hung roughly 1/10 runs with one worker and 3/10 with
 * GOF_NWORKERS=4. The CTest timeout is what turns a recurrence red.
 */
#define _POSIX_C_SOURCE 200809L /* nanosleep (glibc strict -std=c11) */
#include "../include/libgo/fiber.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define N_WRITES 200

static int g_read_ok = 0;

static void reader(void *arg) {
  int fd = *(int *)arg;
  char c;
  int total = 0;
  while (total < N_WRITES) {
    ssize_t r = gof_read(fd, &c, 1);
    if (r == 1) total++;
  }
  g_read_ok = 1;
}

static void *writer_thread(void *arg) {
  int fd = *(int *)arg;
  for (int i = 0; i < N_WRITES; i++) {
    char c = 'x';
    if (write(fd, &c, 1) != 1) break;
    struct timespec ts = { 0, 2 * 1000 * 1000 }; /* 2 ms */
    nanosleep(&ts, NULL);
  }
  return NULL;
}

int main(void) {
  int p[2];
  assert(pipe(p) == 0);

  gof_init(0);
  gof_spawn(reader, &p[0], 0);

  pthread_t th;
  assert(pthread_create(&th, NULL, writer_thread, &p[1]) == 0);

  gof_run(); /* returns once the last fiber (reader) finishes */

  pthread_join(th, NULL);
  assert(g_read_ok == 1);
  close(p[0]);
  close(p[1]);
  printf("gof_test_io_deadlock: OK\n");
  return 0;
}
