#define _POSIX_C_SOURCE 200809L
#include "libgo/fiber.h"
#include "test_net.h"
#include "go.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int completed;
static int blocked;
static int cancelled;
static gof_socket_t read_fd;
static gof_socket_t recv_fd;

void gof_trace_on_block(gof_socket_t fd, int events) {
  (void)fd; (void)events;
  pthread_mutex_lock(&mu);
  ++blocked;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
}

static void io_wait(void *arg) {
  char byte;
  ssize_t result = arg ? gof_recvfrom(recv_fd, &byte, 1, 0, NULL, NULL, 60000)
                       : gof_read(read_fd, &byte, 1);
  int error = errno;
  assert(result == -1 && error == ECANCELED);
  pthread_mutex_lock(&mu);
  ++cancelled;
  pthread_mutex_unlock(&mu);
}

static uint64_t now_ns(void) {
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void work(void *arg) {
  (void)arg;
  assert(gof_in_fiber());
  pthread_mutex_lock(&mu);
  ++completed;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
}

int main(void) {
  gof_init(0);
  gof_run(); /* Empty foreground run must return, unlike background mode. */
  for (int i = 0; i < 50; ++i) {
    assert(gof_start_background(0) == 0);
    assert(gof_start_background(0) == -1);
    /* Prove that zero live fibers for 200 ms does not retire the pool. */
    struct timespec idle = {0, 200000000};
    while (nanosleep(&idle, &idle) != 0) assert(errno == EINTR);
    assert(go_fiber(work, NULL, 0) == 0);
    pthread_mutex_lock(&mu);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    while (completed != i + 1)
      assert(pthread_cond_timedwait(&cv, &mu, &deadline) == 0);
    pthread_mutex_unlock(&mu);
    uint64_t start = now_ns();
    gof_request_stop();
    assert(gof_join_background() == 0);
    assert(now_ns() - start < 1000000000ull);
    assert(gof_join_background() == -1);
  }
  /* Joining must restore foreground behavior. */
  assert(go_fiber(work, NULL, 0) == 0);
  gof_run();
  assert(completed == 51);
  /* Stop cancels both an indefinite read and a distant deadline. The latter
   * must be removed from the heap, not leave join waiting another minute. */
  gof_socket_t p[2], d[2];
  assert(test_pipe(p) == 0);
  assert(test_socketpair(SOCK_DGRAM, d) == 0);
  read_fd = p[0]; recv_fd = d[0];
  assert(gof_start_background(0) == 0);
  assert(go_fiber(io_wait, NULL, 0) == 0);
  assert(go_fiber(io_wait, &recv_fd, 0) == 0);
  pthread_mutex_lock(&mu);
  while (blocked < 2) pthread_cond_wait(&cv, &mu);
  pthread_mutex_unlock(&mu);
  uint64_t start = now_ns();
  gof_request_stop();
  gof_request_stop(); /* idempotent cancellation */
  assert(gof_join_background() == 0);
  assert(now_ns() - start < 1000000000ull);
  assert(cancelled == 2);
  test_close(p[0]); test_close(p[1]); test_close(d[0]); test_close(d[1]);
  puts("stop: indefinite and 60s I/O waits cancelled promptly");
  puts("background: 50 idle/start/spawn/stop/join cycles; foreground: OK");
  return 0;
}
