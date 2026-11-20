/* No timer may retry an idle read; the writer also races registration 10k times. */
#define _POSIX_C_SOURCE 200809L
#include "libgo/fiber.h"
#include "test_net.h"
#include "../debug/debug.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define ROUNDS 10000
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int requested;
static gof_socket_t pipefd[2];
static uint64_t idle_wakes;
static _Atomic uint64_t sent_ns;
static uint64_t latency_ns;
static int observe_blocks;
static int blocked;
static int eof_readers;

void gof_trace_on_block(gof_socket_t fd, int events) {
  (void)fd; (void)events;
  pthread_mutex_lock(&mu);
  if (observe_blocks) ++blocked;
  else if (requested == 0) requested = 1;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
}

static void eof_reader(void *arg) {
  (void)arg;
  char byte;
  assert(gof_read(pipefd[0], &byte, 1) == 0);
  pthread_mutex_lock(&mu);
  ++eof_readers;
  pthread_mutex_unlock(&mu);
}

static void *close_writer(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mu);
  while (blocked < 2) pthread_cond_wait(&cv, &mu);
  pthread_mutex_unlock(&mu);
  assert(test_close(pipefd[1]) == 0);
  return NULL;
}

static uint64_t now_ns(void) {
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void request(int n) {
  pthread_mutex_lock(&mu);
  requested = n;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
}

static void await_request(int n) {
  pthread_mutex_lock(&mu);
  while (requested < n) pthread_cond_wait(&cv, &mu);
  pthread_mutex_unlock(&mu);
}

static void reader(void *arg) {
  (void)arg;
  char byte;
  assert(gof_read(pipefd[0], &byte, 1) == 1 && byte == 'i');
  latency_ns = now_ns() - atomic_load(&sent_ns);
  for (int i = 0; i < ROUNDS; ++i) {
    request(i + 2);
    assert(gof_read(pipefd[0], &byte, 1) == 1 && byte == 'r');
  }
  request(ROUNDS + 2);
  assert(gof_read(pipefd[0], &byte, 1) == 0); /* peer close / HUP */
}

static void *writer(void *arg) {
  (void)arg;
  await_request(1);
  uint64_t before = atomic_load(&gof_ctx_switches);
  /* The duration is the behavior under test, not synchronization. */
  struct timespec idle = {2, 0};
  while (nanosleep(&idle, &idle) != 0) assert(errno == EINTR);
  idle_wakes = atomic_load(&gof_ctx_switches) - before;
  atomic_store(&sent_ns, now_ns());
  assert(test_raw_write(pipefd[1], "i", 1) == 1);
  for (int i = 0; i < ROUNDS; ++i) {
    await_request(i + 2);
    assert(test_raw_write(pipefd[1], "r", 1) == 1);
  }
  await_request(ROUNDS + 2);
  assert(test_close(pipefd[1]) == 0);
  return NULL;
}

int main(void) {
  assert(test_pipe(pipefd) == 0);
  gof_init(0);
  assert(gof_spawn(reader, NULL, 0));
  pthread_t thread;
  assert(pthread_create(&thread, NULL, writer, NULL) == 0);
  gof_run();
  assert(pthread_join(thread, NULL) == 0);
  assert(test_close(pipefd[0]) == 0);
  printf("idle 2s: %llu spurious resumes; write latency: %.3f ms; races: %d; EOF: OK\n",
         (unsigned long long)idle_wakes, (double)latency_ns / 1000000., ROUNDS);
  fflush(stdout);
  assert(idle_wakes == 0);
  assert(latency_ns < 500000000ull);
  /* Both registered readers must receive the same terminal edge. */
  assert(test_pipe(pipefd) == 0);
  observe_blocks = 1;
  assert(gof_spawn(eof_reader, NULL, 0));
  assert(gof_spawn(eof_reader, NULL, 0));
  assert(pthread_create(&thread, NULL, close_writer, NULL) == 0);
  gof_run();
  assert(pthread_join(thread, NULL) == 0);
  assert(eof_readers == 2);
  assert(test_close(pipefd[0]) == 0);
  puts("shared-fd EOF: both registered readers resumed");
  return 0;
}
